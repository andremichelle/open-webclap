import css from "./NoteOutput.sass?inline"
import {Lifecycle} from "@opendaw/lib-std"
import {createElement} from "@opendaw/lib-jsx"
import {Html} from "@opendaw/lib-dom"
import {MidiDevices} from "@opendaw/studio-core"
import {PluginSession} from "@/opendaw/PluginSession"

const className = Html.adoptStyleSheet(css, "NoteOutput")

type Construct = { lifecycle: Lifecycle, session: PluginSession }

// Where the note effect's notes go: a sine, or a MIDI output device (the studio's MIDI Output instrument)
export const NoteOutput = ({lifecycle, session}: Construct) => {
    const status: HTMLElement = <span className="status">Notes play on a sine</span>
    const select: HTMLSelectElement = (
        <select onchange={() => {
            const device = MidiDevices.findOutputDeviceById(select.value).unwrapOrNull()
            session.setOutput(device === null ? {kind: "sine"} : {kind: "midi", device})
            status.textContent = device === null ? "Notes play on a sine" : `Notes go to ${device.name ?? device.id}`
        }}>
            <option value="">Sine</option>
        </select>
    )
    const list = () => {
        const selected = select.value
        for (const option of Array.from(select.options)) {if (option.value !== "") {option.remove()}}
        MidiDevices.externalOutputDevices().ifSome(devices => devices.forEach(device =>
            select.append(<option value={device.id}>MIDI: {device.name ?? device.id}</option>)))
        select.value = Array.from(select.options).some(option => option.value === selected) ? selected : ""
    }
    if (MidiDevices.canRequestMidiAccess()) {
        MidiDevices.requestPermission().then(() => {
            list()
            MidiDevices.get().ifSome(access => {
                const listener = () => list()
                access.addEventListener("statechange", listener)
                lifecycle.own({terminate: () => access.removeEventListener("statechange", listener)})
            })
        }, () => status.textContent = "Web MIDI not allowed, notes play on a sine")
    }
    return (
        <div className={className}>
            <div className="row">{select}</div>
            {status}
        </div>
    )
}
