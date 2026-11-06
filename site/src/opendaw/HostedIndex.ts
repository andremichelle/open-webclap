// openDAW's catalogue of hosted bundles (assets.opendaw.studio/wclaps/index.json, a folder tree): the bundle a
// plugin is in, found by its clap id, so an upload through openDAW's admin needs no change here

// openDAW's hosted bundles want the same credentials the studio sends (see the studio's OpenDAWHeaders)
// (dev goes through the vite proxy: the host allows CORS for the site's own origin only)
export const HostedRoot = import.meta.env.DEV ? "/wclaps" : "https://assets.opendaw.studio/wclaps"
export const HostedHeaders: RequestInit = {headers: {"Authorization": `Basic ${btoa("openDAW:prototype")}`}}

type Folder = {
    folders?: ReadonlyArray<Folder>
    wclaps?: ReadonlyArray<{ uuid: string, plugins: ReadonlyArray<{ clapId: string }> }>
}

export namespace HostedIndex {
    let bundles: Promise<ReadonlyMap<string, string>> | null = null

    // clap id -> bundle uuid; a plugin in several bundles resolves to the last one listed
    const load = async (): Promise<ReadonlyMap<string, string>> => {
        const response = await fetch(`${HostedRoot}/index.json`, HostedHeaders)
        if (!response.ok) {throw new Error(`${HostedRoot}/index.json: ${response.status}`)}
        const map = new Map<string, string>()
        const collect = (folder: Folder): void => {
            folder.wclaps?.forEach(({uuid, plugins}) => plugins.forEach(({clapId}) => map.set(clapId, uuid)))
            folder.folders?.forEach(collect)
        }
        const index: { folders: ReadonlyArray<Folder> } = await response.json()
        index.folders.forEach(collect)
        return map
    }

    const loaded = (): Promise<ReadonlyMap<string, string>> => bundles ??= load().catch(reason => {
        bundles = null // a failed fetch is tried again next time
        throw reason
    })

    // the clap ids openDAW hosts a bundle for
    export const clapIds = async (): Promise<ReadonlySet<string>> => new Set((await loaded()).keys())

    // The url of the hosted bundle holding this plugin, null while openDAW hosts none
    export const bundleUrl = async (clapId: string): Promise<string | null> => {
        const uuid = (await loaded()).get(clapId)
        return uuid === undefined ? null : `${HostedRoot}/${uuid}.wclap`
    }
}
