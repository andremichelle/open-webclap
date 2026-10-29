import css from "./PluginWindow.sass?inline"
import {Exec, isDefined, Lifecycle, Nullable, Terminator} from "@opendaw/lib-std"
import {createElement} from "@opendaw/lib-jsx"
import {Events, Html} from "@opendaw/lib-dom"
import {WclapBundles, WclapGuis} from "@opendaw/studio-core"
import {PluginSession} from "@/opendaw/PluginSession"
import {MessageLog} from "@/host/MessageLog"
import {Button} from "@/ui/components/Button"

const className = Html.adoptStyleSheet(css, "PluginWindow")

const FRAME_URL = `${import.meta.env.BASE_URL}wclap-frame.html`
const DEFAULT_SIZE = {width: 640, height: 480}
const PANEL_HEIGHT = 176 // openDAW's device panel surface
const SAVE_DELAY_MS = 250

// `file:///bundle/ui/index.html` and `/ui/index.html` both name a path in the bundle (as the studio does)
const pagePath = (uri: string): string =>
    uri.startsWith("file:") ? uri.replace(/^file:\/*/, "/").split("/").slice(2).join("/") : uri.replace(/^\//, "")

const scrollParent = (from: HTMLElement): Nullable<HTMLElement> => {
    for (let node = from.parentElement; node !== null; node = node.parentElement) {
        const {overflowX, overflowY} = getComputedStyle(node)
        if (/auto|scroll/.test(`${overflowX} ${overflowY}`)) {return node}
    }
    return null
}

// onLayout: the window has its size (or will not get one), what is laid out around it can show
// panel: the page openDAW shows in its device panel (opendaw.device-panel/1) instead of the plugin window
type Construct = { lifecycle: Lifecycle, session: PluginSession, log: MessageLog, onLayout: Exec, panel?: boolean }

// The plugin's own page, the way openDAW Studio shows it: wclap-frame.html (from the studio) serves the bundle's
// files through its service worker and relays the page's messages over a port. This side passes them on to the
// engine and back.
export const PluginWindow = ({lifecycle, session, log, onLayout, panel = false}: Construct) => {
    const {engine, deviceUuid, deviceKey} = session
    const viewport: HTMLElement = <div className="viewport"/>
    const size: HTMLElement = <span className="size">waiting for the plugin…</span>
    const reopen: HTMLButtonElement = (
        <Button lifecycle={lifecycle} tooltip="Close and reopen the plugin window (gui destroy, create, a new page)"
                onClick={() => {
                    log.add("out", "host", "— window closed, reopening —")
                    open()
                }}>Reopen</Button>
    )
    reopen.disabled = true
    const window_: { session: Nullable<Terminator> } = {session: null}
    const close = () => {
        window_.session?.terminate()
        window_.session = null
    }
    const open = async () => {
        close()
        const session_ = new Terminator()
        window_.session = session_
        const gui = panel
            ? await engine.wclapOpenPanel(deviceUuid, PANEL_HEIGHT).then(({uri, width}) => ({uri, width, height: PANEL_HEIGHT}))
            : await engine.wclapOpenGui(deviceUuid)
        if (window_.session !== session_) {
            if (gui.uri.length > 0) {engine.wclapCloseGui(deviceUuid)}
            return
        }
        if (gui.uri.length === 0) {
            size.textContent = panel ? "no device panel" : "no plugin window"
            onLayout()
            return
        }
        const bundle = await WclapBundles.fetch(session.url)
        const width = gui.width > 0 ? gui.width : DEFAULT_SIZE.width
        const height = gui.height > 0 ? gui.height : DEFAULT_SIZE.height
        const frame: HTMLIFrameElement = <iframe title="Plugin editor"/>
        const setSize = (width: number, height: number) => {
            frame.style.width = `${width}px`
            frame.style.height = `${height}px`
            size.textContent = `${width} × ${height}`
        }
        setSize(width, height)
        onLayout()
        const saveTimer: { id: number } = {id: 0}
        const scheduleSave = () => {
            clearTimeout(saveTimer.id)
            saveTimer.id = window.setTimeout(() => engine.wclapSaveState(deviceUuid), SAVE_DELAY_MS)
        }
        let port: Nullable<MessagePort> = null
        const post = (message: object, transfer: Array<Transferable> = []) => port?.postMessage(message, transfer)
        session_.ownAll(
            WclapGuis.register(deviceKey, bytes => {
                log.frame("out", bytes)
                post({type: "wclap-message", bytes})
            }),
            WclapGuis.subscribeResize(deviceKey, ({width, height}) => {
                if (panel) {return}
                setSize(width, height)
                log.add("out", "host", `request_resize ${width} × ${height}`)
            }),
            WclapGuis.subscribeShow(deviceKey, visible => {
                if (panel || !visible) {return}
                log.add("out", "host", "request_show")
                element.scrollIntoView({block: "nearest", behavior: "smooth"})
            }),
            Events.subscribe(window, "message", (event: MessageEvent) => {
                if (event.source !== frame.contentWindow || event.data?.type !== "wclap-frame-ready") {return}
                const received = event.ports.at(0)
                if (!isDefined(received)) {return}
                port?.close()
                port = received
                port.onmessage = ({data}: MessageEvent) => {
                    if (data?.type === "wclap-message" && data.bytes instanceof ArrayBuffer) {
                        log.frame("in", data.bytes)
                        engine.wclapReceive(deviceUuid, data.bytes)
                        scheduleSave()
                    } else if (data?.type === "wclap-wheel") {
                        // a wheel event the page did not use: the column around the editor scrolls (deltaMode 1 lines, 2 pages)
                        const parent = scrollParent(element)
                        if (parent === null) {return}
                        const unit = data.deltaMode === 1 ? 16 : data.deltaMode === 2 ? parent.clientHeight : 1
                        parent.scrollBy(data.deltaX * unit, data.deltaY * unit)
                    } else if (data?.type === "wclap-key") {
                        // plain keys reach the page and the site (the computer keyboard plays notes in both)
                        window.dispatchEvent(new KeyboardEvent(data.kind, data.init))
                    } else if (data?.type === "wclap-error") {
                        log.add("in", "error", data.message)
                    }
                }
                port.start()
                post({type: "wclap-open", uuid: deviceKey, page: pagePath(gui.uri), files: bundle.files})
            }),
            {
                terminate: () => {
                    clearTimeout(saveTimer.id)
                    port?.close()
                    engine.wclapSaveState(deviceUuid)
                    engine.wclapCloseGui(deviceUuid)
                    frame.remove()
                }
            }
        )
        frame.src = FRAME_URL
        viewport.replaceChildren(frame)
        reopen.disabled = false
    }
    lifecycle.ownAll(
        session.subscribeStatus(status => {
            if (panel && status.state === "ready" && status.panel !== true) {
                size.textContent = "no device panel"
                return
            }
            if (status.state === "ready" && window_.session === null) {
                open().catch(error => {
                    size.textContent = `could not open: ${error}`
                    onLayout()
                })
            } else if (status.state === "failed") {
                close()
                size.textContent = `failed: ${status.message}`
                onLayout()
            }
        }),
        {terminate: close}
    )
    const element: HTMLElement = (
        <div className={className}>
            <div className="titlebar">
                <span>{panel ? "Device panel" : "Plugin window"}</span>
                {size}
                {reopen}
            </div>
            {viewport}
        </div>
    )
    return element
}
