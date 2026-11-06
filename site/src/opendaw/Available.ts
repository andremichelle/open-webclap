import {PluginEntry, Plugins} from "@/plugins"
import {HostedIndex} from "@/opendaw/HostedIndex"

// The plugins the site lists and routes: deployed, those it serves itself (bundled) or openDAW hosts, so a port that
// was never uploaded has no page that cannot load; in dev all of them (?local loads the builds in build/). Read once
// at start; without openDAW's index (offline) all of them, their pages say what is missing.
export namespace Available {
    let plugins: ReadonlyArray<PluginEntry> = Plugins

    export const load = async (): Promise<void> => {
        if (import.meta.env.DEV) {return}
        const hosted = await HostedIndex.clapIds().catch(() => null)
        if (hosted === null) {return}
        plugins = Plugins.filter(plugin => plugin.bundled !== undefined || hosted.has(plugin.clapId))
    }

    export const list = (): ReadonlyArray<PluginEntry> => plugins
}
