/*
    Six Sines as module.wasm: the engine as a CLAP plugin, the editor runs in the page (ui.wasm).

    Derived from Six Sines' src/clap/six-sines-clap.cpp, Copyright 2024-2025, Paul Walker and Various authors,
    released under the MIT license (https://github.com/baconpaul/six-sines).

    The plugin is upstream's SixSinesClap (src/clap/six-sines-clap.cpp, MIT) with the JUCE GUI shim, preset
    discovery and the VST3/AUv2 extensions left out (single output only), plus a webview bridge: clap.gui with
    the webview API and clap.webview. The engine (Synth) is unchanged.

    The bridge plays the role of the editor towards the engine. Upstream's editor shares the main thread
    with the plugin: it edits Synth::patchMain directly, pushes MainToAudioMsg into Synth::mainToAudio and
    drains Synth::audioToMain while it is open (Synth::editorActive). Here the editor's queue arrives as
    toAudio frames and is applied the same way, the engine's answers leave as toMain frames, and what the
    editor writes to patchMain besides values arrives as patchMeta. See six_sines_wire.h.

    The page gets its updates from on_main_thread after host->request_callback(). A host that never calls
    back gets them from process() instead, at most 15 times a second.
*/

#include "configuration.h"
#include <clap/clap.h>
#include <clap/ext/draft/webview.h>

#include <clap/helpers/plugin.hh>
#include "synth/synth.h"

#include <clap/helpers/plugin.hxx>
#include <clap/helpers/host-proxy.hxx>

#include <map>
#include <memory>
#include "sst/plugininfra/patch-support/patch_base_clap_adapter.h"
#include "sst/plugininfra/cpufeatures.h"
#include "sst/voicemanager/midi1_to_voicemanager.h"

#include "six_sines_wire.h"

namespace baconpaul::six_sines
{
namespace clapimpl
{
namespace wire = six_sines_webclap::wire;
namespace proto = juce::webclap::protocol;

static constexpr clap::helpers::MisbehaviourHandler misLevel =
    clap::helpers::MisbehaviourHandler::Ignore;
static constexpr clap::helpers::CheckingLevel checkLevel = clap::helpers::CheckingLevel::Minimal;

using plugHelper_t = clap::helpers::Plugin<misLevel, checkLevel>;

const clap_plugin_descriptor *getDescriptor()
{
    static const char *features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                     CLAP_PLUGIN_FEATURE_SYNTHESIZER, "Free and Open Source",
                                     "Audio Rate Modulation", nullptr};

    static clap_plugin_descriptor desc = {
        CLAP_VERSION,
        "org.baconpaul.six-sines",
        "Six Sines",
        "BaconPaul",
        "https://github.com/baconpaul/six-sines",
        "",
        "",
        sst::plugininfra::VersionInformation::project_version_and_hash,
        "Synth with audio rate modulation, the editor runs in the page",
        &features[0]};
    return &desc;
}

struct SixSinesWebClap : public plugHelper_t
{
    // The size the editor opens with (SixSinesEditor::edWidth/edHeight at 100 %)
    static constexpr uint32_t defaultWidth{1048}, defaultHeight{690};

    SixSinesWebClap(const clap_host *h) : plugHelper_t(getDescriptor(), h)
    {
        engine = std::make_unique<Synth>(false);
        engine->clapHost = h;
    }

    std::unique_ptr<Synth> engine;
    size_t blockPos{0};

  protected:
    bool init() noexcept override
    {
        auto h = _host.host();
        hostWebview = static_cast<const clap_host_webview_t *>(h->get_extension(h, CLAP_EXT_WEBVIEW));
        hostGui = static_cast<const clap_host_gui_t *>(h->get_extension(h, CLAP_EXT_GUI));
        return true;
    }

    bool activate(double sampleRate, uint32_t minFrameCount,
                  uint32_t maxFrameCount) noexcept override
    {
        // The audio thread is stopped here; seed it from the main-thread source of truth.
        engine->patch.copyValuesFrom(engine->patchMain);
        engine->setSampleRate(sampleRate);
        this->sampleRate = sampleRate;
        return true;
    }

    void onMainThread() noexcept override
    {
        engine->onMainThread();
        sendPageUpdates();
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return 1; }
    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info *info) const noexcept override
    {
        if (index != 0)
            return false;
        if (isInput)
        {
            info->id = 82649;
            strncpy(info->name, "Audio In", sizeof(info->name));
            info->flags = 0;
        }
        else
        {
            info->id = 75241;
            strncpy(info->name, "Main Out", sizeof(info->name));
            info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        }
        info->in_place_pair = CLAP_INVALID_ID;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        return true;
    }

    bool implementsNotePorts() const noexcept override { return true; }
    uint32_t notePortsCount(bool isInput) const noexcept override { return isInput ? 1 : 0; }
    bool notePortsInfo(uint32_t index, bool isInput,
                       clap_note_port_info *info) const noexcept override
    {
        if (!isInput || index != 0)
            return false;

        info->id = 17252;
        info->supported_dialects =
            CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE | CLAP_NOTE_DIALECT_CLAP;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        strncpy(info->name, "Note Input", CLAP_NAME_SIZE - 1);
        return true;
    }

    clap_process_status process(const clap_process *process) noexcept override
    {
        auto fpuguard = sst::plugininfra::cpufeatures::FPUStateGuard();

        auto ev = process->in_events;
        auto outq = process->out_events;
        auto sz = ev->size(ev);

        const clap_event_header_t *nextEvent{nullptr};
        uint32_t nextEventIndex{0};
        if (sz != 0)
        {
            nextEvent = ev->get(ev, nextEventIndex);
        }

        if (process->transport)
        {
            engine->monoValues.tempoSyncRatio = process->transport->tempo / 120.0;
            auto tflags = process->transport->flags;
            // Only trust the song position when the host is both playing and exposes a
            // seconds timeline; otherwise free-run our own clock.
            engine->monoValues.isPlayingAndHasSecondsTimeline =
                (tflags & CLAP_TRANSPORT_IS_PLAYING) &&
                (tflags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE);
            if (engine->monoValues.isPlayingAndHasSecondsTimeline)
            {
                engine->monoValues.hostSongPosSeconds =
                    process->transport->song_pos_seconds / (double)CLAP_SECTIME_FACTOR;
                engine->monoValues.songPosSeconds = engine->monoValues.hostSongPosSeconds;
            }
        }
        else
        {
            engine->monoValues.tempoSyncRatio = 1.f;
            engine->monoValues.isPlayingAndHasSecondsTimeline = false;
        }
        engine->monoValues.songPosNeedsResync = true;

        engine->preloadTables(sz != 0);
        engine->refreshMtsMaster();

        float *out[2] = {process->audio_outputs[0].data32[0], process->audio_outputs[0].data32[1]};

        const float *audioInL{nullptr}, *audioInR{nullptr};
        if (process->audio_inputs_count > 0 && process->audio_inputs[0].data32)
        {
            audioInL = process->audio_inputs[0].data32[0];
            audioInR = process->audio_inputs[0].data32[1];
        }

        for (auto s = 0U; s < process->frames_count; ++s)
        {
            engine->pushAudioIn(audioInL ? audioInL[s] : 0.f, audioInR ? audioInR[s] : 0.f);

            if (blockPos == 0)
            {
                // Only realy need to run events when we do the block process
                while (nextEvent && nextEvent->time <= s)
                {
                    handleEvent(nextEvent);
                    nextEventIndex++;
                    if (nextEventIndex < sz)
                        nextEvent = ev->get(ev, nextEventIndex);
                    else
                        nextEvent = nullptr;
                }

                engine->process(outq);
            }

            out[0][s] = engine->output[0][blockPos];
            out[1][s] = engine->output[1][blockPos];

            blockPos++;
            if (blockPos == blockSize)
            {
                blockPos = 0;
            }
        }

        while (nextEvent)
        {
            handleEvent(nextEvent);
            nextEventIndex++;
            if (nextEventIndex < sz)
                nextEvent = ev->get(ev, nextEventIndex);
            else
                nextEvent = nullptr;
        }

        schedulePageUpdates(process->frames_count);
        return CLAP_PROCESS_CONTINUE;
    }

    void reset() noexcept override { engine->voiceManager->allSoundsOff(); }

    bool handleEvent(const clap_event_header_t *nextEvent)
    {
        auto &vm = engine->voiceManager;
        if (nextEvent->space_id == CLAP_CORE_EVENT_SPACE_ID)
        {
            switch (nextEvent->type)
            {
            case CLAP_EVENT_MIDI:
            {
                auto mevt = reinterpret_cast<const clap_event_midi *>(nextEvent);
                sst::voicemanager::applyMidi1Message(*vm, mevt->port_index, mevt->data);
            }
            break;

            case CLAP_EVENT_NOTE_ON:
            {
                auto nevt = reinterpret_cast<const clap_event_note *>(nextEvent);
                vm->processNoteOnEvent(nevt->port_index, nevt->channel, nevt->key, nevt->note_id,
                                       nevt->velocity, 0.f);
            }
            break;

            case CLAP_EVENT_NOTE_OFF:
            {
                auto nevt = reinterpret_cast<const clap_event_note *>(nextEvent);
                vm->processNoteOffEvent(nevt->port_index, nevt->channel, nevt->key, nevt->note_id,
                                        nevt->velocity);
            }
            break;
            case CLAP_EVENT_PARAM_VALUE:
            {
                auto pevt = reinterpret_cast<const clap_event_param_value *>(nextEvent);
                auto par =
                    sst::plugininfra::patch_support::paramFromClapEvent<Param>(pevt, engine->patch);
                if (par)
                {
                    engine->handleParamValue(par, pevt->param_id, pevt->value);
                }
            }
            break;

            case CLAP_EVENT_NOTE_EXPRESSION:
            {
                auto nevt = reinterpret_cast<const clap_event_note_expression *>(nextEvent);
                vm->routeNoteExpression(nevt->port_index, nevt->channel, nevt->key, nevt->note_id,
                                        nevt->expression_id, nevt->value);
            }
            break;
            default:
                break;
            }
        }
        return true;
    }

    bool implementsState() const noexcept override { return true; }
    bool stateSave(const clap_ostream *ostream) noexcept override
    {
        // patchMain is authoritative. Without a page, drain pending audio-thread updates into it first;
        // with one, sendPageUpdates() keeps it current.
        if (!engine->editorActive.load())
            engine->drainAudioToMainInto(engine->patchMain);

        return sst::plugininfra::patch_support::patchToOutStream(engine->patchMain, ostream, true);
    }
    bool stateLoad(const clap_istream *istream) noexcept override
    {
        // Load into temps so a parse failure never leaves the engine half-written.
        auto tmp = std::make_unique<Patch>();
        Synth::DawStateMain loadedState{};
        tmp->dawExtraStateFrom = [&](TiXmlElement &e) { Synth::fromDawExtraState(e, loadedState); };

        if (!sst::plugininfra::patch_support::inStreamToPatch(istream, *tmp))
            return false;

        engine->patchMain.copyValuesFrom(*tmp);

        // 1.1-era sessions carry no <mpe> element; derive MPE from the legacy in-patch slots
        if (!loadedState.main.mpeFromExtraState)
        {
            loadedState.audio.mpeActive = engine->patchMain.output.legacyMpeActive.value > 0.5f;
            loadedState.audio.mpeBendRange =
                (int)std::round(engine->patchMain.output.legacyMpeBendRange.value);
        }
        engine->dawStateMain = loadedState;
        engine->uiForceRebuild++; // the page gets the new patch

        if (isActive())
        {
            Synth::sendEntirePatchToAudio(engine->patchMain, engine->mainToAudio, _host.host());
        }
        else if (_host.canUseParams())
        {
            _host.paramsRescan(CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
            _host.paramsRescan(CLAP_PARAM_RESCAN_INFO);
        }

        Synth::MainToAudioMsg des{Synth::MainToAudioMsg::SET_AUDIO_DAW_STATE};
        des.audioDawState = engine->dawStateMain.audio;
        engine->mainToAudio.push(des);

        // Upstream builds a loaded patch's wavetables in the editor's idle. Here the engine has no editor
        // next to it, so it builds them itself.
        engine->reconcileWavetables();
        requestPageUpdate();
        return true;
    }

    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return engine->patchMain.params.size(); }
    bool paramsInfo(uint32_t paramIndex, clap_param_info *info) const noexcept override
    {
        auto ok =
            sst::plugininfra::patch_support::patchParamsInfo(paramIndex, info, engine->patchMain);
        if (!ok)
            return ok;

        // param events are resolved on the audio thread, so the cookie must point into `patch`
        info->cookie = engine->clapCookieFor(info->id);

        // The macro amplitude param shows the user's macro name: "Foo (Macro N)"
        auto *param = engine->patchMain.params[paramIndex];
        if (param->meta.hasFeature(isPrimaryMacroFeature))
        {
            int idx = (info->id - Patch::MacroNode::idBase) / Patch::MacroNode::idStride;
            if (idx >= 0 && idx < (int)numMacros)
            {
                const auto &nameBuf = engine->patchMain.macroNames[idx];
                std::string userName(nameBuf.data());
                auto def = Patch::MacroNode::defaultGroupName(idx);
                if (!userName.empty() && userName != def)
                {
                    auto fullName = userName + " (" + def + ")";
                    strncpy(info->name, fullName.c_str(), CLAP_NAME_SIZE - 1);
                    info->name[CLAP_NAME_SIZE - 1] = 0;
                }
            }
        }
        return ok;
    }
    bool paramsValue(clap_id paramId, double *value) noexcept override
    {
        return sst::plugininfra::patch_support::patchParamsValue(paramId, value, engine->patchMain);
    }
    bool paramsValueToText(clap_id paramId, double value, char *display,
                           uint32_t size) noexcept override
    {
        return sst::plugininfra::patch_support::patchParamsValueToText(paramId, value, display,
                                                                       size, engine->patchMain);
    }
    bool paramsTextToValue(clap_id paramId, const char *display, double *value) noexcept override
    {
        return sst::plugininfra::patch_support::patchParamsTextToValue(paramId, display, value,
                                                                       engine->patchMain);
    }
    void paramsFlush(const clap_input_events *in, const clap_output_events *out) noexcept override
    {
        if (isActive())
        {
            auto sz = in->size(in);
            for (uint32_t i = 0; i < sz; ++i)
                handleEvent(in->get(in, i));
            engine->processUIQueue(out);
        }
        else
        {
            engine->paramsFlushMainThread(in, out);
        }
        requestPageUpdate();
    }

    //==========================================================================================================
    // The page (clap.gui with the webview API, clap.webview)

  public:
    bool receive(const void *data, uint32_t size)
    {
        proto::Reader r(data, size);
        const auto type = r.get<uint8_t>();
        r.get<uint8_t>();
        r.get<uint16_t>();

        if (!r.ok)
            return false;

        switch (type)
        {
        case proto::hello:
        {
            // From now on the page drains audioToMain (see sendPageUpdates). Anything queued before is
            // already history, the snapshot carries the result.
            if (!engine->editorActive.exchange(true))
                engine->drainAudioToMainInto(engine->patchMain);
            pageReady = true;
            pageNeedsSnapshot = true;
            pageValues.clear();
            sendPageUpdates();
            break;
        }

        case wire::toAudio:
        {
            auto count = r.get<uint32_t>();
            bool sentSomething{false};
            for (uint32_t i = 0; i < count && r.ok; ++i)
            {
                auto m = wire::getMainToAudio(r);
                if (!r.ok)
                    break;
                sentSomething = applyFromPage(m) || sentSomething;
            }
            // Inactive, the queue is drained by paramsFlush (main thread), which needs the host to call it
            if (sentSomething && !isActive() && _host.canUseParams())
                _host.paramsRequestFlush();
            break;
        }

        case wire::patchMeta:
        {
            std::array<std::array<char, 64>, numMacros> macrosBefore = engine->patchMain.macroNames;
            if (wire::applyPatchMeta(r, engine->patchMain, engine->dawStateMain.main))
            {
                if (macrosBefore != engine->patchMain.macroNames && _host.canUseParams())
                    _host.paramsRescan(CLAP_PARAM_RESCAN_INFO);
                engine->reconcileWavetables();
                if (_host.canUseState())
                    _host.stateMarkDirty();
            }
            break;
        }

        case wire::scope:
        {
            auto on = r.get<uint8_t>() != 0;
            engine->audioOutputRing.clear();
            if (on)
                engine->audioOutputRing.subscribe();
            else
                engine->audioOutputRing.unsubscribe();
            break;
        }

        case proto::resize:
        {
            auto width = r.get<uint32_t>();
            auto height = r.get<uint32_t>();
            if (r.ok && width > 0 && height > 0 && (width != editorWidth || height != editorHeight))
            {
                editorWidth = width;
                editorHeight = height;
                if (hostGui && hostGui->request_resize)
                    hostGui->request_resize(_host.host(), width, height);
            }
            break;
        }

        default:
            break;
        }
        return true;
    }

    /** What upstream's editor does on the shared main thread: write patchMain, push to the engine.
        Returns true if the message went into the queue. */
    bool applyFromPage(const Synth::MainToAudioMsg &m)
    {
        using M = Synth::MainToAudioMsg;
        auto &pm = engine->patchMain;
        switch (m.action)
        {
        case M::SET_PARAM:
        case M::SET_PARAM_WITHOUT_NOTIFYING:
        {
            auto it = pm.paramMap.find(m.paramId);
            if (it == pm.paramMap.end())
                return false; // the engine would throw on an unknown id
            it->second->value = m.value;
            break;
        }
        case M::BEGIN_EDIT:
        case M::END_EDIT:
            if (pm.paramMap.find(m.paramId) == pm.paramMap.end())
                return false;
            break;
        case M::SET_WAVETABLE:
            return false; // a staging slot of the page's stand-in, the engine builds its own (patchMeta)
        case M::SET_AUDIO_DAW_STATE:
            engine->dawStateMain.audio = m.audioDawState;
            break;
        default:
            break;
        }

        engine->mainToAudio.push(m);

        // A preset load in the page ends with SEND_POST_LOAD, after all its values. Upstream's preset
        // manager tells the host to re-read here (Synth::sendEntirePatchToAudio).
        if (m.action == M::SEND_POST_LOAD && _host.canUseParams())
        {
            _host.paramsRescan(CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
            _host.paramsRescan(CLAP_PARAM_RESCAN_INFO);
            _host.paramsRequestFlush();
        }
        return true;
    }

    void requestPageUpdate()
    {
        if (!pageReady || pageUpdateRequested)
            return;
        pageUpdateRequested = true;
        samplesSinceRequest = 0;
        _host.requestCallback();
    }

    /** From process(): ask for on_main_thread at about 60 Hz while the page is open, and send from here if
        the host does not call back. */
    void schedulePageUpdates(uint32_t frames)
    {
        if (!pageReady)
            return;

        samplesSinceRequest += frames;

        if (pageUpdateRequested)
        {
            if (samplesSinceRequest >= sampleRate / 15)
                sendPageUpdates();
        }
        else if (samplesSinceRequest >= sampleRate / 60)
        {
            requestPageUpdate();
        }
    }

    void send(const proto::Writer &w)
    {
        hostWebview->send(_host.host(), w.bytes.data(), (uint32_t)w.bytes.size());
    }

    void sendPage(uint8_t type)
    {
        auto state = engine->patchMain.toState(true);
        proto::Writer w(type);
        if (type == proto::snapshot)
        {
            w.u32(0); // no parameter list: the patch carries the values
            w.u32((uint32_t)state.size());
        }
        w.put(state.data(), state.size());
        send(w);
    }

    void sendPageUpdates()
    {
        pageUpdateRequested = false;
        samplesSinceRequest = 0;

        if (!pageReady || !hostWebview || !hostWebview->send)
            return;

        // Build what the patch references (a no-op when nothing changed). A false return means a staging
        // slot is still in flight: the next call retries.
        engine->reconcileWavetables();

        // Host-driven values first (audioToMain), so patchMain is current for a patch sent below. Every action
        // is a value, so only the last one per action and id goes. Displays also skip what the page shows already,
        // a parameter never: the page may have moved it since.
        std::map<uint64_t, Synth::AudioToMainMsg> latest;
        while (auto m = engine->audioToMain.pop())
        {
            Synth::handleAudioToMainMessage(engine->patchMain, *m);
            if (m->action != Synth::AudioToMainMsg::MTS_POINTER)
                latest[((uint64_t)m->action << 32) | m->paramId] = *m;
        }
        proto::Writer toMain(wire::toMain);
        toMain.u32(0);
        uint32_t count{0};
        for (const auto &[key, m] : latest)
        {
            if (m.action != Synth::AudioToMainMsg::UPDATE_PARAM)
            {
                const auto known = pageValues.find(key);
                if (known != pageValues.end() && known->second.first == m.value && known->second.second == m.value2)
                    continue;
                pageValues[key] = {m.value, m.value2};
            }
            wire::put(toMain, m);
            ++count;
        }

        auto fr = engine->uiForceRebuild.load();
        if (std::exchange(pageNeedsSnapshot, false))
        {
            lastForceRebuild = fr;
            sendPage(proto::snapshot);
        }
        else if (fr != lastForceRebuild)
        {
            lastForceRebuild = fr;
            sendPage(proto::state);
        }

        if (count > 0)
        {
            std::memcpy(toMain.bytes.data() + 4, &count, sizeof(count));
            send(toMain);
        }

        if (engine->audioOutputRing.subscribed())
        {
            proto::Writer audio(wire::audio);
            audio.u32(0);
            uint32_t frames{0};
            while (frames < maxAudioFrames)
            {
                auto p = engine->audioOutputRing.pop();
                if (!p)
                    break;
                audio.f32(p->first);
                audio.f32(p->second);
                ++frames;
            }
            if (frames > 0)
            {
                std::memcpy(audio.bytes.data() + 4, &frames, sizeof(frames));
                send(audio);
            }
        }
    }

    void pageClosed()
    {
        guiCreated = false;
        pageReady = false;
        pageUpdateRequested = false;
        engine->audioOutputRing.unsubscribe();
        engine->audioOutputRing.clear();
        engine->editorActive = false;
        // audioToMain is ours to drain again (Synth::onMainThread)
        _host.requestCallback();
    }

    int32_t getUri(char *uri, uint32_t capacity) const
    {
        static constexpr const char *page = "/ui/index.html";
        auto length = (uint32_t)std::strlen(page) + 1;
        if (uri && capacity > 0)
        {
            auto n = std::min(length, capacity) - 1;
            std::memcpy(uri, page, n);
            uri[n] = 0;
        }
        return (int32_t)length;
    }

    const void *extension(const char *id) noexcept override
    {
        static const clap_plugin_gui_t gui{
            [](const clap_plugin_t *, const char *api, bool isFloating)
            { return !isFloating && std::strcmp(api, CLAP_WINDOW_API_WEBVIEW) == 0; },
            [](const clap_plugin_t *, const char **api, bool *isFloating)
            {
                *api = CLAP_WINDOW_API_WEBVIEW;
                *isFloating = false;
                return true;
            },
            [](const clap_plugin_t *p, const char *api, bool isFloating)
            {
                if (isFloating || std::strcmp(api, CLAP_WINDOW_API_WEBVIEW) != 0)
                    return false;
                self(p).guiCreated = true;
                return true;
            },
            [](const clap_plugin_t *p) { self(p).pageClosed(); },
            [](const clap_plugin_t *, double) { return false; },
            [](const clap_plugin_t *p, uint32_t *width, uint32_t *height)
            {
                *width = self(p).editorWidth;
                *height = self(p).editorHeight;
                return true;
            },
            [](const clap_plugin_t *) { return false; }, // the editor zooms itself and asks for the size
            [](const clap_plugin_t *, clap_gui_resize_hints_t *) { return false; },
            [](const clap_plugin_t *p, uint32_t *width, uint32_t *height)
            {
                *width = self(p).editorWidth;
                *height = self(p).editorHeight;
                return true;
            },
            [](const clap_plugin_t *p, uint32_t width, uint32_t height)
            { return width == self(p).editorWidth && height == self(p).editorHeight; },
            [](const clap_plugin_t *, const clap_window_t *) { return true; },
            [](const clap_plugin_t *, const clap_window_t *) { return false; },
            [](const clap_plugin_t *, const char *) {},
            [](const clap_plugin_t *) { return true; },
            [](const clap_plugin_t *) { return true; }};

        static const clap_plugin_webview_t webview{
            [](const clap_plugin_t *p, char *uri, uint32_t capacity)
            { return self(p).getUri(uri, capacity); },
            [](const clap_plugin_t *, const char *, char *, uint32_t, const clap_ostream_t *)
            { return false; }, // hosts serve the bundle
            [](const clap_plugin_t *p, const void *buffer, uint32_t size)
            { return self(p).receive(buffer, size); }};

        if (std::strcmp(id, CLAP_EXT_GUI) == 0)
            return &gui;
        if (std::strcmp(id, CLAP_EXT_WEBVIEW) == 0)
            return &webview;
        return nullptr;
    }

  private:
    static SixSinesWebClap &self(const clap_plugin_t *p)
    {
        return *static_cast<SixSinesWebClap *>(p->plugin_data);
    }

    static constexpr uint32_t maxAudioFrames{16384};

    const clap_host_webview_t *hostWebview{nullptr};
    const clap_host_gui_t *hostGui{nullptr};
    double sampleRate{48000};
    uint32_t editorWidth{defaultWidth}, editorHeight{defaultHeight};
    bool guiCreated{false}, pageReady{false}, pageNeedsSnapshot{false}, pageUpdateRequested{false};
    uint32_t samplesSinceRequest{0};
    uint32_t lastForceRebuild{0};
    std::map<uint64_t, std::pair<float, float>> pageValues;
};

} // namespace clapimpl

//==============================================================================================================
// CLAP entry

namespace
{
uint32_t getPluginCount(const clap_plugin_factory *) { return 1; }

const clap_plugin_descriptor *getPluginDescriptor(const clap_plugin_factory *, uint32_t index)
{
    return index == 0 ? clapimpl::getDescriptor() : nullptr;
}

const clap_plugin *createPlugin(const clap_plugin_factory *, const clap_host *host, const char *id)
{
    if (std::strcmp(id, clapimpl::getDescriptor()->id) != 0)
        return nullptr;
    return (new clapimpl::SixSinesWebClap(host))->clapPlugin();
}

const void *getFactory(const char *id)
{
    static const clap_plugin_factory factory{getPluginCount, getPluginDescriptor, createPlugin};
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
}

bool entryInit(const char *) { return true; }
void entryDeinit() {}
} // namespace
} // namespace baconpaul::six_sines

extern "C" const CLAP_EXPORT clap_plugin_entry clap_entry{
    CLAP_VERSION, baconpaul::six_sines::entryInit, baconpaul::six_sines::entryDeinit,
    baconpaul::six_sines::getFactory};

namespace chlp = clap::helpers;
namespace bpss = baconpaul::six_sines::clapimpl;

template class chlp::Plugin<bpss::misLevel, bpss::checkLevel>;
template class chlp::HostProxy<bpss::misLevel, bpss::checkLevel>;
