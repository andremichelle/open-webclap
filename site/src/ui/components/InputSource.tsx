import css from "./InputSource.sass?inline"
import {Lifecycle} from "@opendaw/lib-std"
import {createElement} from "@opendaw/lib-jsx"
import {Events, Html} from "@opendaw/lib-dom"
import {InputKind, PluginSession} from "@/opendaw/PluginSession"
import {Button} from "@/ui/components/Button"

const className = Html.adoptStyleSheet(css, "InputSource")

const InputLabels: Record<InputKind, string> = {
    none: "Silence",
    microphone: "Microphone",
    file: "Audio file",
    tone: "Test tone",
    noise: "Noise"
}

const Hints: Record<InputKind, string> = {
    none: "",
    microphone: "Your microphone through the effect. Use headphones to avoid feedback.",
    file: "Choose an audio file or drop one here. It plays in a loop.",
    tone: "Sawtooth, 220 Hz, looped",
    noise: "White noise, looped"
}

type Construct = { lifecycle: Lifecycle, session: PluginSession, remember?: boolean }

// the preview's last choice, so a rebuilt plugin gets the same input
let remembered: { kind: InputKind, file: { name: string, buffer: AudioBuffer } | null } | null = null

// What plays into the effect: the microphone (openDAW's monitoring through the effect chain), or a looped
// audio file, test tone or noise on the track
export const InputSource = ({lifecycle, session, remember = false}: Construct) => {
    const {audioContext} = session.env
    let file: { name: string, buffer: AudioBuffer } | null = null
    let request = 0 // a newer choice wins over one still decoding or asking for the microphone
    const status: HTMLElement = <span className="status"/>
    const fileInput: HTMLInputElement = (
        <input type="file" accept="audio/*" hidden onchange={() => {
            const chosen = fileInput.files?.item(0)
            if (chosen) {loadFile(chosen)}
            fileInput.value = ""
        }}/>
    )
    const chooseFile: HTMLElement = (
        <Button lifecycle={lifecycle} onClick={() => fileInput.click()} className="hidden">Choose file…</Button>
    )
    const select: HTMLSelectElement = (
        <select onchange={() => use(select.value as InputKind)}>
            {(Object.keys(InputLabels) as Array<InputKind>).map(kind => <option value={kind}>{InputLabels[kind]}</option>)}
        </select>
    )
    const setStatus = (text: string, error: boolean = false) => {
        status.textContent = text
        status.classList.toggle("error", error)
    }
    const use = async (kind: InputKind) => {
        const id = ++request
        if (remember) {remembered = {kind, file}}
        chooseFile.classList.toggle("hidden", kind !== "file")
        if (kind === "file" && file === null) {
            await session.setInput("none")
            setStatus(Hints.file)
            return
        }
        setStatus(kind === "file" && file !== null
            ? `${file.name} · ${file.buffer.duration.toFixed(1)} s, looped` : Hints[kind])
        try {
            await session.setInput(kind, file ?? undefined)
        } catch (reason) {
            if (id === request) {setStatus(`${InputLabels[kind]}: ${reason instanceof Error ? reason.message : reason}`, true)}
        }
    }
    const loadFile = async (blob: File) => {
        setStatus(`Decoding ${blob.name}…`)
        try {
            file = {name: blob.name, buffer: await audioContext.decodeAudioData(await blob.arrayBuffer())}
        } catch {
            setStatus(`Cannot decode ${blob.name}`, true)
            return
        }
        select.value = "file"
        await use("file")
    }
    const element: HTMLElement = (
        <div className={className}>
            <div className="row">
                {select}
                {chooseFile}
                {fileInput}
            </div>
            <div className="row">{status}</div>
        </div>
    )
    lifecycle.ownAll(
        Events.subscribe(element, "dragover", (event: DragEvent) => {
            event.preventDefault()
            element.classList.add("drop")
        }),
        Events.subscribe(element, "dragleave", () => element.classList.remove("drop")),
        Events.subscribe(element, "drop", (event: DragEvent) => {
            event.preventDefault()
            element.classList.remove("drop")
            const dropped = event.dataTransfer?.files.item(0)
            if (dropped) {loadFile(dropped)}
        }),
        {terminate: () => request++}
    )
    if (remember && remembered !== null) {
        file = remembered.file
        select.value = remembered.kind
        use(remembered.kind)
    }
    return element
}
