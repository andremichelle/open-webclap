import css from "./StepGrid.sass?inline"
import {Lifecycle} from "@opendaw/lib-std"
import {createElement} from "@opendaw/lib-jsx"
import {Html} from "@opendaw/lib-dom"
import {PatternSteps, PluginSession} from "@/opendaw/PluginSession"
import {Button} from "@/ui/components/Button"

const className = Html.adoptStyleSheet(css, "StepGrid")

const LowestPitch = 60
const Pitches = 13
const Names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

type Construct = { lifecycle: Lifecycle, session: PluginSession }

// A one bar loop of sixteenths from C4 to C5 into the note effect, the piano roll without a timeline
export const StepGrid = ({lifecycle, session}: Construct) => {
    const playing = {value: false}
    const play: HTMLButtonElement = (
        <Button lifecycle={lifecycle} tooltip="Play the pattern in a loop" onClick={() => {
            playing.value = !playing.value
            session.playPattern(playing.value)
            play.textContent = playing.value ? "Stop" : "Play"
            play.classList.toggle("active", playing.value)
        }}>Play</Button>
    )
    lifecycle.own({terminate: () => {if (playing.value) {session.playPattern(false)}}})
    const rows = Array.from({length: Pitches}, (_, index) => LowestPitch + Pitches - 1 - index)
    return (
        <div className={className}>
            <div className="bar">{play}<span className="hint">Click cells to place sixteenth notes</span></div>
            <div className="grid" style={{"--steps": String(PatternSteps)}}>
                {rows.map(pitch => [
                    <span className={Html.buildClassList("name", Names[pitch % 12].length > 1 && "sharp")}>
                        {Names[pitch % 12]}{Math.floor(pitch / 12) - 1}
                    </span>,
                    ...Array.from({length: PatternSteps}, (_, step) => {
                        const cell: HTMLElement = (
                            <div className={Html.buildClassList("cell", step % 4 === 0 && "beat")}
                                 onpointerdown={(event: PointerEvent) => {
                                     if (event.button !== 0) {return}
                                     cell.classList.toggle("on", session.toggleStep(step, pitch))
                                 }}/>
                        )
                        cell.classList.toggle("on", session.hasStep(step, pitch))
                        return cell
                    })
                ])}
            </div>
        </div>
    )
}
