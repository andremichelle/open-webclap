import {Terminator} from "@opendaw/lib-std"
import {createElement, Frag, Router} from "@opendaw/lib-jsx"
import {Available} from "@/opendaw/Available"
import {Header} from "@/ui/Header"
import {HomePage} from "@/ui/pages/HomePage"
import {ImportedPage, PluginPage} from "@/ui/pages/PluginPage"
import {NotFoundPage} from "@/ui/pages/NotFoundPage"

export const App = () => {
    const terminator = new Terminator()
    return (
        <Frag>
            <Header lifecycle={terminator}/>
            <Router<null>
                runtime={terminator}
                service={null}
                fallback={NotFoundPage}
                routes={[
                    {path: "/", factory: HomePage},
                    ...Available.list().map(plugin => ({path: `/${plugin.id}`, factory: PluginPage(plugin)})),
                    {path: "/imported/*", factory: ImportedPage}
                ]}
            />
        </Frag>
    )
}
