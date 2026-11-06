// Serving from a sub-path (a fork's GitHub Pages, https://<owner>.github.io/<repo>/): vite's base makes the
// assets load, and this makes the router base-aware. Upstream (base "/", the custom domain root) is a no-op.
import {RouteLocation} from "@opendaw/lib-jsx"

const base = import.meta.env.BASE_URL // "/" at the domain root, "/open-webclap/" on a fork's project page

if (base !== "/") {
    const strip = (path: string): string =>
        path.startsWith(base) ? path.slice(base.length - 1) || "/" : path
    const proto = RouteLocation.prototype as unknown as Record<string, any>
    const pathDescriptor = Object.getOwnPropertyDescriptor(proto, "path")
    if (pathDescriptor?.get !== undefined) {
        Object.defineProperty(proto, "path", {
            ...pathDescriptor,
            get(this: unknown) {return strip(pathDescriptor.get!.call(this))}
        })
    }
    for (const method of ["navigateTo", "replaceWith"]) {
        const original = proto[method]
        proto[method] = function(this: unknown, path: string) {
            return original.call(this, base + path.replace(/^\//, ""))
        }
    }
}
