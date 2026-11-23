import {Terminable, Terminator} from "@opendaw/lib-std"
import {createElement, PageFactory, replaceChildren} from "@opendaw/lib-jsx"
import {WclapBundles} from "@opendaw/studio-core"
import {PluginSession} from "@/opendaw/PluginSession"
import {PluginView} from "@/ui/pages/PluginPage"

// A plugin an agent is building with the opendaw-webclap skill: its preview.mjs serves this site on localhost and the
// build at /__local/<file>, and announces every rebuild on /__local/events. A rebuild replaces the plugin in place,
// its settings and the input kept.
export const PreviewPage: PageFactory<null> = ({lifecycle}) => {
    const file = new URLSearchParams(location.search).get("bundle") ?? ""
    const local = location.hostname === "localhost" || location.hostname === "127.0.0.1"
    document.title = "Preview · open-webclap"
    if (!local || file.length === 0) {
        return (
            <div className="PluginPage">
                <div className="body">
                    <div className="message">
                        <p>This page shows a plugin while your coding agent builds it. Ask the agent to preview the
                            plugin: it opens this page from your computer.</p>
                    </div>
                </div>
            </div>
        )
    }
    const holder: HTMLElement = <div style={{display: "contents"}}/>
    let current: Terminable | null = null
    let session: PluginSession | null = null
    // the last settings of a running plugin, kept over rebuilds that come quicker than a plugin loads
    let state: Int8Array | null = null
    let version = 0
    const show = () => {
        if (session !== null) {state = session.device.pluginState.getValue().slice()}
        current?.terminate()
        session = null
        const terminator = new Terminator()
        current = terminator
        const id = ++version
        replaceChildren(holder, (
            <PluginView lifecycle={terminator} name={file} panel windowed rememberInput
                        links={<div className="links"><span>local build · {file}</span></div>}
                        open={async (env, loading) => {
                            loading.setValue({text: `Loading ${file}…`, fraction: null})
                            const response = await fetch(`/__local/${encodeURIComponent(file)}`, {cache: "no-store"})
                            if (!response.ok) {throw new Error(`${file} is not built yet (${response.status})`)}
                            const url = `preview://${id}/${encodeURIComponent(file)}`
                            await WclapBundles.register(url, await response.arrayBuffer())
                            const created = await PluginSession.create(env, url)
                            if (id !== version) {
                                created.terminate()
                                throw new Error("replaced by a newer build")
                            }
                            const saved = state
                            if (saved !== null && saved.length > 0) {
                                created.project.editing.modify(() => created.device.pluginState.setValue(saved), false)
                            }
                            session = created
                            return created
                        }}
                        subtitle={null}
                        hint={<p>Ask your agent to build the plugin again, this page reloads it by itself.</p>}/>
        ))
    }
    show()
    const events = new EventSource("/__local/events")
    events.addEventListener("change", () => show())
    lifecycle.own({
        terminate: () => {
            events.close()
            current?.terminate()
        }
    })
    return <div style={{display: "contents"}}>{holder}</div>
}
