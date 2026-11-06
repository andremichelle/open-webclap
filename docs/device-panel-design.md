# Device panel design

How a plugin's page looks in openDAW's device panel: flat, quiet, compact, next to the stock devices without
standing out. Signalsmith Basics is the reference (`ports/signalsmith-basics/page/index.html`, `?preview` shows all
seven effects). [Building a WebCLAP for openDAW](building-for-opendaw.md) has the panel's mechanics (176 px,
`opendaw.device-panel/1`, the width feature); this is about the look.

The idea: nothing is drawn that does not carry information. No backgrounds, borders, outlines, tiles or gaps that
pretend to be borders. A display is a curve on the panel with room around it; a knob is a thin arc around a dark
cap; text is small and quiet, and the colour says what it is (a name, a value, something being changed).

## The card

- **No name.** openDAW's device header already shows the plugin's name; the page starts with its content.
- **The panel's colour**, `hsl(197, 14%, 12%)` (`--color-panel-background`, classic scheme), on `body` and the card,
  so the panel is seamless. Nothing else has a background.
- **Padding as the stock devices'**: 8 px top and bottom, 4 px at the sides.
- **Groups 16 px apart**: the display, the knobs, the parameter rows. Within the knobs, columns 10 px apart.
- **One fixed width**, declared by the plugin (`get_width(176)` and `opendaw:device-panel-width:<width>`), measured
  from the page's own content with its font loaded. The page sets the card to that width; its preview warns when
  the content outgrows it.

## Colours

| What | Colour |
|---|---|
| Names (labels under knobs, parameter names, a choice's name) and a knob's value (arc and pointer) | cream at 80 % over the panel, `#b9bda4` |
| Values (parameter rows) | grey, `hsl(197, 9%, 54%)` |
| A choice's current step | grey, a little brighter, `hsl(197, 12%, 74%)`, on `rgba(255, 255, 255, 0.04)` |
| A choice's other steps | dim grey, `hsl(197, 10%, 30%)` |
| A value while hovered or dragged | the studio's blue, `hsl(189, 100%, 65%)` |
| Knob cap | `hsl(197, 11%, 17%)` |
| Knob track | `hsl(197, 15%, 84%)` at 14 % |
| Displays | the studio's display blue, `hsl(200, 83%, 60%)` (`DisplayPaint`) |
| Display captions (scale numbers, units) | `hsl(197, 10%, 60%)` at about 50 % |

- **Names and values never share a colour.** A value in the names' colour reads as a name (and the other way
  round): a choice's steps are values, so they are grey, while its name below is cream.
- Colours come from openDAW's palette (`Colors.ts`), faded towards the panel to make them recede: mix with the
  panel colour as if transparent over it (darker and less saturated at once), not just darker.
- No per-plugin accent colours: every display is the studio's display blue, every knob the same.

## Type

- Rubik, 9 px, regular weight. Display captions 7 px.
- Normal case everywhere, captions included: "Low Cut", "On", "10 ms", "1k". No capitals for effect.
- **The shortest name that reads**: "Smooth", not "Smoothing"; "Res", "Exp Ratio". A word that would make a column
  wider than its knob is worth shortening.
- **No redundant labels.** A choice whose steps say what it does ("Pass, Reflect, Duplicate, Both") needs no name.
- **Values never move the layout.** Digits are tabular (`font-variant-numeric: tabular-nums`) and a value is as wide
  as the widest text its parameter can show (measured over the whole range, or every step), so "9" to "10" moves
  nothing. Think of it with every value that changes, not after.

## Displays

- **No background, no frame.** The display is drawn on the panel; its room is the gap to the controls (16 px), not
  an inset of its own (2 px at most), so it reaches the card's padding.
- **A scale with numbers gets no lines.** The numbers place it. Lines are only references (dashed: unity, 0 Hz, the
  current depth) and the grid of a display without numbers (a transfer curve).
- **No lines along the edges** (a baseline, an outer grid line, a 0 dB line on the top edge).
- **The unit with its numbers**: "… 40 50 ms" on the axis, not "MS" in a corner. Captions that belong together share
  a line (a "GR" label on the line of its "-24").
- Traces 1.25 px at 85 %, the area under them faded from 16 % to nothing, dots 2 px, bars and tails at 40 to 65 %.
  No glow.
- Axes hold still: fixed to the parameter's whole range, so turning a knob moves the curve, not the scale.

## Knobs

Three sizes, on a grid of three 50 px rows (3 px apart), a knob's label 4 px below it:

| Size | Across | For |
|---|---|---|
| Large | 44 px | the effect's main control (Depth, Drive, Thresh, Limit, Decay) |
| Medium | 28 px | the rest |
| Small | 20 px | secondary trims (Hysteresis, Link, a small effect's Shift and Mix) |

- The knob after the studio's (`Knob.tsx`): a 1.5 px track open at the bottom, the value as an arc from its anchor
  (the start, or the top for a bipolar one), a 1.25 px pointer, the dark cap inside.
- **The large knob is centred by its circle**, not by the knob and label together: its label hangs below the middle.
- **A row's labels share a line**: a cell's content sits on its bottom, whatever the knob's size.
- **Columns hug their content**: as wide as their widest knob or label, so the gaps are the same everywhere.
- Medium knobs three to a column; four make a square of two rows.
- **Levels last**: Mix, Out, Dry and Wet at the bottom of the last knob column.
- **A column with a single knob is suspicious**, not forbidden: the large knob stands alone; a single medium knob
  in a column is usually a layout that wants rethinking (a square, a stack, a parameter row).
- Group by function (compressor, timing, filters); signal flow is a guide, not a rule worth wasted space.

## Parameter rows

When there are more parameters than fit as knobs (about eight), the main ones stay knobs and the minor ones become
rows: the name left (cream), the value right (grey), 13 px high, groups 6 px apart, 6 px more before the column.

- Dragged up and down like a knob; a choice steps on a click (Auto Gain: Off, On).
- Hover reports the parameter (`clap.param-hovered`), so openDAW's right-click menu and double-click value entry work
  on it, as on a knob.
- **An effect with one parameter** shows it as a single row in the display's corner (the Analyser's "Res 10"),
  not as a knob beside the display.

## Choices

The steps as words, one per line, 13 px high: the current one grey on a faint background, the others dim; the name
below in cream (or none, see above). A click on a step selects it.

## While changing

Hovering or dragging a knob shows its value in the label's place, in the studio's blue; a parameter row's value turns
blue. Nothing else lights up: no hover backgrounds, no outlines.

## Checklist

- [ ] no name, no backgrounds, borders, outlines or tiles; the panel's colour
- [ ] padding 8 / 4 px, groups 16 px apart, columns 10 px apart and hugging their content
- [ ] names cream, values grey, never mixed; displays in the display blue
- [ ] Rubik 9 px, normal case, the shortest names, no redundant labels
- [ ] values in tabular digits at a fixed width
- [ ] displays without background or edges, no lines for a scale with numbers, units with their numbers
- [ ] large knob centred by its circle, rows' labels on one line, levels last
- [ ] many parameters: the minor ones as rows; one parameter: a row in the display's corner
- [ ] a fixed width, declared by the plugin, checked against the content
