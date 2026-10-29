import css from "./HomePage.sass?inline"
import {createElement, LocalLink, PageFactory} from "@opendaw/lib-jsx"
import {Html} from "@opendaw/lib-dom"
import {bundleFileName, PluginEntry, Plugins} from "@/plugins"
import {PluginSession} from "@/opendaw/PluginSession"

const className = Html.adoptStyleSheet(css, "HomePage")

const KindLabels = {"instrument": "Instrument", "audio-fx": "Audio effect", "midi-fx": "Note effect"} as const

// Fetches the bundle (openDAW's want credentials, so no plain link) and saves it
const DownloadLink = ({plugin}: { plugin: PluginEntry }) => {
    let busy = false
    const link: HTMLAnchorElement = <a onclick={() => {
        if (busy) {return}
        busy = true
        link.textContent = "Downloading…"
        link.title = ""
        PluginSession.locate(plugin, "hosted").then(async ({url, init}) => {
            const response = await fetch(url, init)
            if (!response.ok) {throw new Error(`${url}: ${response.status}`)}
            const href = URL.createObjectURL(new Blob([await response.arrayBuffer()], {type: "application/gzip"}))
            const save: HTMLAnchorElement = <a href={href} download={bundleFileName(plugin)}/>
            save.click()
            setTimeout(() => URL.revokeObjectURL(href), 1000)
            link.textContent = "Download"
        }).catch((reason: unknown) => {
            link.textContent = "Download failed"
            link.title = reason instanceof Error ? reason.message : String(reason)
        }).finally(() => busy = false)
    }}>Download</a>
    return link
}

const PluginCard = ({plugin}: { plugin: PluginEntry }) => (
    <article>
        <header>
            <h2><LocalLink href={`/${plugin.id}`}>{plugin.name}</LocalLink></h2>
            <span className={Html.buildClassList("kind", plugin.kind)}>{KindLabels[plugin.kind]}</span>
        </header>
        <p>{plugin.description}</p>
        <footer>
            <a href={plugin.credit} target="_blank" rel="noopener noreferrer">Original</a>
            <a href={plugin.source} target="_blank" rel="noopener noreferrer">Port source</a>
            <DownloadLink plugin={plugin}/>
            <span>{plugin.license}</span>
        </footer>
    </article>
)

export const HomePage: PageFactory<null> = () => (
    <div className={className}>
        <h1>open-webclap</h1>
        <p className="intro">
            Open-source audio plugins ported to WebCLAP. Each page runs the plugin in this browser: its DSP in an
            AudioWorklet, its editor in the plugin window. Instruments play from the keyboard or MIDI, effects
            from the microphone, an audio file or a test signal. The site is built on
            the <a href="https://www.npmjs.com/package/@opendaw/studio-sdk" target="_blank"
                   rel="noopener noreferrer">openDAW SDK</a>, the toolchain behind <a href="https://opendaw.studio"
                   target="_blank" rel="noopener noreferrer">openDAW Studio</a>, and the same bundles are in the
            studio's WebCLAP catalog.
        </p>
        <div className="grid">
            {Plugins.map(plugin => <PluginCard plugin={plugin}/>)}
        </div>
    </div>
)
