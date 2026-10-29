import css from "./Header.sass?inline"
import {Lifecycle} from "@opendaw/lib-std"
import {createElement, JsxValue, LocalLink, replaceChildren, RouteLocation} from "@opendaw/lib-jsx"
import {Html} from "@opendaw/lib-dom"
import {PluginKind, Plugins} from "@/plugins"
import {NavMenu} from "@/ui/components/NavMenu"
import {Colors} from "@opendaw/studio-enums"
import {Presentation} from "@/ui/Presentation"
import {Button} from "@/ui/components/Button"
import {Imports} from "@/opendaw/Imports"

const className = Html.adoptStyleSheet(css, "Header")

const linksOf = (kind: PluginKind) => Plugins
    .filter(plugin => plugin.kind === kind)
    .map(plugin => ({href: `/${plugin.id}`, label: plugin.name}))

// What a page shows in the header, after the menus (a plugin page: its name, id and links)
export namespace HeaderTitle {
    export const element: HTMLElement = <div className="title"/>

    // The router builds the next page before it tears the previous one down: a page only clears its own title
    export const show = (lifecycle: Lifecycle, content: JsxValue): void => {
        const holder: HTMLElement = <div style={{display: "contents"}}>{content}</div>
        replaceChildren(element, holder)
        lifecycle.own({terminate: () => {if (holder.parentElement === element) {holder.remove()}}})
    }
}

export const Header = ({lifecycle}: { lifecycle: Lifecycle }) => {
    // only on a plugin page, there is nothing to present elsewhere
    const present: HTMLButtonElement = (
        <button type="button" className="present"
                title="Presentation mode: only the plugin, over the whole screen (Escape leaves it)"
                onclick={() => Presentation.toggle()}>
            <svg viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
                <path d="M16 3H22V9H20V5H16V3ZM2 3H8V5H4V9H2V3ZM20 19V15H22V21H16V19H20ZM4 19H8V21H2V15H4V19Z"/>
            </svg>
        </button>
    )
    lifecycle.own(Presentation.isAvailable.catchupAndSubscribe(owner =>
        present.classList.toggle("hidden", !owner.getValue())))
    // an external WebCLAP bundle from disk, shown on a page of its own
    const file: HTMLInputElement = (
        <input type="file" accept=".gz,.tgz,.wasm" hidden onchange={() => {
            const chosen = file.files?.item(0)
            file.value = ""
            if (chosen) {RouteLocation.get().navigateTo(`/imported/${Imports.add(chosen).id}`)}
        }}/>
    )
    return (
    <header className={className}>
        <h5><LocalLink href="/">open-webclap</LocalLink></h5>
        <hr/>
        <nav>
            <NavMenu lifecycle={lifecycle} label="Instruments" color={Colors.green} links={linksOf("instrument")}/>
            <NavMenu lifecycle={lifecycle} label="Audio FX" color={Colors.blue} links={linksOf("audio-fx")}/>
            <NavMenu lifecycle={lifecycle} label="Note FX" color={Colors.orange} links={linksOf("midi-fx")}/>
        </nav>
        <hr/>
        {present}
        {HeaderTitle.element}
        <div style={{flex: "1"}}/>
        <Button lifecycle={lifecycle} onClick={() => file.click()}
                tooltip="Load an external WebCLAP bundle (.wclap.tar.gz) and show it">Import</Button>
        {file}
        <a className="github" href="https://github.com/andremichelle/open-webclap" target="_blank"
           rel="noopener noreferrer">GitHub</a>
    </header>
    )
}
