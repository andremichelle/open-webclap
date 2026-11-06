import "./main.sass"
import {initializeColors} from "@opendaw/studio-enums"
import {replaceChildren} from "@opendaw/lib-jsx"
import {AnimationFrame} from "@opendaw/lib-dom"
import {App} from "@/ui/App"
import {Isolation} from "@/opendaw/Isolation"
import {Available} from "@/opendaw/Available"

initializeColors(document.documentElement)

// The engine needs SharedArrayBuffer, so the page must be cross-origin isolated. Static hosts (GitHub Pages)
// send no COOP/COEP headers: a service worker adds them, the first visit reloads once it is in charge.
Isolation.recover().then(async isolated => {
    if (!isolated) {
        document.body.textContent = "Reload to enable audio (the page needs cross-origin isolation)."
        return
    }
    await Available.load()
    replaceChildren(document.body, App())
    AnimationFrame.start(window)
})
