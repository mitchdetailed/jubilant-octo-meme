# Choosing laps

Every graph, report and gauge shows the same selection: one **Main** range (usually a lap), an optional
**Overlay 0** (the reference lap you compare against) and any number of numbered **overlays** 1, 2, 3 and so on.
You choose them in the Data window.

## The Data window (F8)

F8 shows the Data window and puts the keyboard focus in it. It lists every open file, and under each file its
laps (or its outings, when the active worksheet shows outings) in time order. The columns:

| Column | Shows | Click |
|---|---|---|
| **M** | A dot on the Main range | Makes the range Main |
| **O** | A box on Overlay 0 | Makes it Overlay 0, or clears it |
| **1** | The overlay number | Adds it as the next numbered overlay, or removes it |
| Name | Out Lap, Lap 7, In Lap, Partial; *italic* when the lap is not trusted | |
| Time | The lap time (format: Tools > Options > Analysis "Lap time format") | |
| Δ Fastest | The difference to the fastest lap of the same file | |
| Offset | The alignment offset when one is set | |
| Trusted | Tick or untick to trust the lap (an undoable edit) | |
| Local maths | An icon when the file has its own maths | |

Keys in the Data window: UP and DOWN move the highlight; **ENTER** makes the highlighted lap Main;
**CTRL+ENTER** toggles it as Overlay 0; **ALT+ENTER** toggles it as a numbered overlay; **DELETE** on a file row
closes the file; ESC returns to the worksheet. The context menu has the same actions, plus the filters, Reload
File (for a file that changed on disk), and the Lap Editor and Details editor.

Removing a numbered overlay renumbers the others so the numbers stay 1, 2, 3 without gaps. The **reference
overlay** that variance, Δ columns and reports compare with is Overlay 0 when it is set, otherwise overlay 1.

## Keys that change the selection

These work from a worksheet component (and the global ones from any dock):

| Key | Does |
|---|---|
| N / P | Main moves to the next or previous lap of its file, in the order the Data window shows; the zoom stays on the same part of the lap |
| Q | Swap Main and Overlay 0 |
| F4 | Hide or show the overlays without forgetting them |
| SHIFT+F4 | Clear Overlay 0 and every numbered overlay |
| CTRL+F | The fastest lap of the Main file becomes Main |
| CTRL+ALT+F | The fastest lap becomes Main and every other lap within the fast-lap tolerance becomes a numbered overlay (Tools > Options > Analysis "Fast-lap tolerance", 1 % by default) |
| J | Draw Main in one colour instead of each channel's colour |
| W | Zoom to the Main lap |
| CTRL+A | Close every file |

**Which lap is fastest.** When the file has trusted laps, only trusted flying laps count. When it has none, its
flying and partial laps count, trusted or not. Out laps, in laps and pit laps never count.

Tools > Options > General "Select the fastest lap on load" makes the fastest lap Main whenever a file opens.

## Filters

Right-click in the Data window, **Filters**, to show fewer laps of a file:

- **All**: every lap.
- **Selected**: only the laps that are Main or an overlay.
- **Fastest** and **Fastest 3**: the fastest lap, or the three fastest, by the rule above.
- **Within 1 % of Fastest**.
- **Lap Numbers…**: a list such as `3, 5-7` (single laps and inclusive ranges, separated by commas). A mistake
  keeps the previous filter and the editor says what is wrong.

A filter never hides a lap that is Main or an overlay: such a lap stays listed, marked with a filter icon.
**Apply Filter To All Sources** in the same menu gives every open file the Main file's filter.
Filters are saved with the project.

## Trust, laps and details

A lap you do not trust (a yellow-flag lap, a lap with a missed beacon) can be unticked in the **Trusted** column.
Untrusted laps are shown in italics, are left out of the fastest-lap rule while the file has trusted laps, and
are left out of automatic graph scaling unless Tools > Options > Analysis "Include untrusted data in autoscale"
is on.

**Tools > Lap Editor…** fixes the laps themselves: insert a missed beacon by time or by lap time, or at the graph
cursor (SHIFT+F7 in a graph), move a beacon by an offset (in time, or by distance), see the beacons the laps
ignore, and restore the logged laps.
**Tools > Details…** (CTRL+D) edits the file's details (driver, venue, vehicle, comments and so on). Both kinds of
edit are stored in the file's sidecar beside the log and can be undone with CTRL+Z.

## When you reopen a project

With Tools > Options > General "Load previous data with the project" on (the default), opening a project reopens
the files you had open and restores Main, Overlay 0, the numbered overlays, single-colour Main and the filters.
A lap that no longer exists (its file was edited elsewhere) is dropped with a note in Messages.
