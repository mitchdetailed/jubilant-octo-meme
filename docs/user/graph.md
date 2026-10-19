# Graphs and worksheets

## Workbooks, worksheets and components

A **workbook** is a set of **worksheets**, shown as tabs; a worksheet holds **components** (graphs, reports,
gauges) placed freely on it. Each workbook also remembers its own arrangement of the dock windows. A project can
hold several workbooks; Esses ships a default circuit workbook to start from.

| Key | Does |
|---|---|
| 1 … 9, 0 | Worksheet tab 1 to 9, and 10 (with a worksheet component focused) |
| F11 / F12 | Previous / next worksheet |
| CTRL+1 … CTRL+0 | Workbook 1 to 10 |
| CTRL+F11 / CTRL+F12 | Previous / next workbook |
| F7 | Select Worksheet: pick a workbook and worksheet from a list |
| CTRL+F7 | The Layout Editor |
| TAB / SHIFT+TAB | Focus the next / previous component |

**The Layout Editor** (CTRL+F7) adds, renames, duplicates, moves, copies, deletes, imports and exports workbooks
and worksheets, sets a worksheet's domain (time or distance), and can lock the layout of a project so components
cannot be moved by accident.

**Adding and arranging components.** Component > Add (or right-click an empty part of the worksheet) adds one of
the ten component types below in the largest free area. Drag a component's caption bar (or SHIFT+drag anywhere on
it) to move it, drag an edge or corner to resize it; positions snap to a 1 % grid unless you hold ALT, and
ALT+arrow keys nudge it one grid step (where the focused component does not use those keys itself). CTRL+drag on
an edge moves that edge for every component that touches it. CTRL+X, CTRL+C, CTRL+V and DELETE cut, copy, paste
and delete the selected components; every change can be undone. Component > Maximise fills the worksheet with the
focused component until you restore it.

**Properties.** F5 opens the focused component's properties: a dialog with a tab per group of settings, which
stays open while you work. Apply makes the changes (one undo step), OK applies and
closes, Cancel undoes what the dialog applied. CTRL+N opens it at the active channel.

**Adding channels.** C opens the Channels window with the cursor in its search box. Type the start of each word
of a channel name (`w s fl` finds Wheel Speed FL; text in brackets is searchable too; Tools > Options > General
"Loose channel-name search" matches anywhere in the words). Then ENTER adds the channel to the focused component
and CTRL+ENTER adds it as a new panel; you can also drag channels onto a component (hold CTRL when you drop for a
new panel).

### The components

| Component | Shows |
|---|---|
| Time/Distance Graph | Channels against time or distance, in panels or overlapped, with the cursor, the datum, overlays, variance and measurements |
| Outing Graph | A whole outing with a rectangle that sets the zoom of the linked graphs |
| Scatter Plot | One channel against others, as points and/or trend lines, optionally coloured by a third channel |
| Histogram | How long (or how often) channels spend in each band of values |
| Channel Report | Statistics of channels (minimum, maximum, mean, and so on) per lap or per range |
| Time Report | Section and lap times with the best of each section |
| Track Position | The track map with the position of Main and the overlays at the cursor |
| Values List | Channel values at the cursor for Main and Overlay 0, with the difference |
| Numeric Gauge, Bar Gauge | One channel's value at the cursor, with colour bands |

Keys for the focused component: **L** legend, **K** colour legend, **G** cycle the layout (panels or overlapped),
**S** cycle the trace style, **ALT+0 … ALT+9** colour bands, **PAGE UP / PAGE DOWN** the active channel,
**CTRL+H** hide the active channel, **CTRL+DELETE** remove it. Component > Export Data… writes the component's
data as CSV, and Component > Print to Clipboard copies a picture of it.

## Time and distance

**F9** switches the active worksheet between time and distance, keeping the zoom on the same part of the lap.
Distance comes from **Corr Dist**, which Esses computes from a speed channel (the built-in corrected speed and
distance channels, Corr Speed and Corr Dist). **Tools > Corrected Speed and Distance…** chooses where speed and
distance come from (for example the fastest wheel of a set of wheel speeds), limits, an optional correction from
longitudinal G, and **lap distance stretching**: every lap stretched to the same reference length, so laps line
up corner for corner. The toolbar's lap-stretch button switches stretching on and off; the Lap Editor lists each
lap's stretch.

## Zoom, cursor and datum

The zoom is shared by every worksheet, and the strip above the worksheet tabs (the zoom summary) shows the whole
Main outing with its laps, the zoomed span and the cursor: click it to place the cursor, drag the span to move
it, SHIFT+click to extend the span to that point, double-click to see everything.

| Key or mouse | Does (in a graph) |
|---|---|
| UP / DOWN, mouse wheel | Zoom in / out about the cursor (the wheel: about the pointer) |
| Double-click, move, click | Zoom to the span you drew (ALT: vertically, CTRL: both ways) |
| F2 | Show the whole Main outing |
| W | Zoom to the Main lap |
| Z | Zoom to the span between the datum and the cursor |
| SHIFT+LEFT / SHIFT+RIGHT | Pan by a tenth of the span; B / F page back and forward; H centres on the cursor |
| ALT+UP / ALT+DOWN, ALT+F2 | Vertical zoom of the active channel's group, and its full scale; SHIFT+UP / SHIFT+DOWN pan it |
| Click | Place the cursor on the nearest sample |
| LEFT / RIGHT | Move the cursor one pixel's worth; CTRL+LEFT / CTRL+RIGHT ten times as far; HOME / END to the edges |
| Y | Cycle the cursor style: line, crosshair, values only |
| D | Datum on or off |
| SPACE | Place the datum at the cursor |
| X | Swap the datum and the cursor |
| A | Animate the cursor (Data > Animation Rate sets the speed) |

With the datum on, the graph's key shows each channel's value at the datum and the difference to the cursor.

**Comparing laps.**

- **O** turns on the offset axis: drag Main's or the overlay's small axis to slide that lap in time or
  distance, or use CTRL+SHIFT+LEFT/RIGHT (Main) and ALT+SHIFT+LEFT/RIGHT (the reference overlay), ten times as far
  with UP/DOWN. Turning the axis off clears the offsets.
- **F3** shows the variance panel: the time gained or lost by each overlay against Main along the lap (it needs a
  visible overlay).
- **M** shows the minimum, maximum and mean of each channel over the visible span.
- **E** shows the status and errors strip; **SHIFT+E** hides or shows the logger's device events.
- **CTRL+SHIFT+C** copies every channel's name, value and unit at the cursor.
- Add Annotation… in the Component menu puts a note at the cursor.

A component's properties ("Follow the zoom", "Follow the cursor", "Follow the datum") can unlink it from the
shared zoom, cursor or datum, so it keeps its own.

## High-DPI and several monitors

Esses follows the display scale of each monitor (100 %, 125 %, 150 %, 200 % and others). Moving the window, or a
floating dock window, to a monitor with another scale re-lays out and redraws its components at that monitor's
scale. At fractional scales (125 %, 150 %) grid lines, ticks, the cursor and outlines are drawn on whole screen
pixels so they stay sharp. Tables (reports, the Values window, the dock lists) are drawn by the toolkit, and their
thin grid lines can look slightly uneven at fractional scales.

If a graph looks blurred after you change the display scale while Esses is running, restart Esses and please
report it (see [Troubleshooting](troubleshooting.md#reporting-a-problem)) with your monitors' scales.
