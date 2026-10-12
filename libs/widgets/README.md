# esses_widgets

Layer 5, Qt Widgets (01 §2.1, 04 §7, 06; D-026, D-027, D-030). The shell widgets, the dock facade, the worksheet
canvas, the component framework with the M1 components and the M1 docks. Built only with `ESSES_BUILD_QT=ON`.

Namespace `esses::widgets` (06 §3.1). Links `esses_qtmodel`, `esses_qtrender` (and the Qt-free libraries), Qt
Widgets and, privately, `esses::qtads` (the Qt Advanced Docking System, a shared library under LGPL-2.1-or-later;
only with `ESSES_DOCK_BACKEND=ads`, the default; `cmake/qtads.cmake`). No other library may link ADS. Compiled with `QT_NO_KEYWORDS` (PUBLIC): write `Q_SIGNALS`, `Q_SLOTS`, `Q_EMIT`. Never calls `TaskPool`
directly: builds go through `qt::RenderService` (06 §1).

Three independent glob roots (AUTOMOC on), so two agents can work here without touching a shared file:

| Root | Headers | Contents |
|---|---|---|
| `src/shell/` | `include/esses/widgets/shell/` | `DockHost` + `IDockBackend` + `AdsBackend` + `QDockWidgetBackend` (`dock_host.hpp`, `ads_backend.hpp`, `qdockwidget_backend.hpp`; `ads_backend.cpp` is the only file that includes ADS and is compiled only when ADS is built), `MainWindow`, `WorksheetCanvas`, `CommandActions`, `KeyEventRouter` |
| `src/docks/` | `include/esses/widgets/docks/` | `DataWindow` (F8), `ChannelsWindow` (C), `ValuesWindow` (V), `MessagesWindow` |
| `src/components/` | `include/esses/widgets/components/` | `WorksheetContext`, `ExportOptions`, `ExportSink`, `ComponentWidget` (+ factory, `registerBuiltinComponents`), `TimeDistanceGraph` (`time_distance`), `ValuesList` (`values_list`), `PlaceholderComponent` |

Every `tests/**/tst_<name>.cpp` is one Qt Test executable (`ctest -R widgets.tst_`), run offscreen. The
cross-layer suites (AC-M1-01 walkthrough, AC-M1-09 F9, AC-M1-10 prefetch, AC-M1-05 render parity) live in
`tests/ui`.

Design references: `DockHost` 06 §3 (AC-M1-13); `MainWindow` 06 §2; `WorksheetCanvas` 06 §4.3;
`ComponentWidget` 04 §7.1 / 06 §4.4; `TimeDistanceGraph` 04 §8; `ValuesList` 04 §15; docks 06 §§6-8, 11;
`CommandActions` / `KeyEventRouter` 06 §14.

## M1-STUB items the implementing agents must replace

None left in `src/`. `tests/tst_widgets_contract.cpp` is replaced by the real tests below.

- shell (`src/shell/`, without `WorksheetCanvas`) and docks (`src/docks/`): done, see "Shell and docks (M1)".
- components (`src/components/`) and `WorksheetCanvas`: done (no `M1-STUB` left), see below.

## Shell and docks (M1)

- **DockHost** (D-153). Two backends behind `IDockBackend`: `AdsBackend` (the Qt Advanced Docking System, the
  default whenever the build has ADS) and `QDockWidgetBackend` (the fallback). `DockHost::makeDefaultBackend`
  takes `defaultBackendName()`: the environment variable `ESSES_DOCK_BACKEND` (`ads`, `qdockwidget`) when it
  names a backend of the build, else the build's CMake option `ESSES_DOCK_BACKEND`;
  `availableBackends()`/`makeBackend(name, host)` serve the tests. Registry: a dock registered in the same
  default area as an earlier one is tabbed under it (Channels under Data), the earlier one staying the current
  tab. `IDockBackend::setCentral` puts the worksheet area in the host: `QMainWindow::setCentralWidget`, or ADS's
  central dock widget (`esses.worksheetArea`; not closable, movable, floatable or pinnable) inside the
  `CDockManager` that is the host's central widget. The dock's own controls (close and float buttons,
  title-bar drags, ADS's pin and side tabs) reach the state through `IDockBackend::setStateObserver`. `toggle`
  (V) returns to the last visible state. `focus` activates the dock's window when another one is active.
  - ADS: the state is read from ADS each time (closed, auto-hide, floating container, docked). AutoHide =
    pinned to ADS's side bar (the side of the area the dock was registered or reset in; while the window is
    shown, the side ADS's own pin button would choose); `focus` or the side tab flies it out, and it collapses
    when the focus leaves it (06 §3.3 item 5) or on a click outside (ADS). Floating = in a floating container.
    The ADS configuration is its default with opaque splitters and auto-hide. Perspective: `{"backend": "ads",
    "version": 1, "adsVersion": "5.1.1", "state": <base64 CDockManager::saveState(1)>}`; a dock the perspective
    does not know goes to its registered area and state. On Linux off X11 (offscreen, Wayland, ...) `AdsBackend`
    sets `Qt::AA_DontCreateNativeWidgetSiblings` for the application: ADS's Linux floating window takes its
    native window id on every show, which would otherwise make the dock manager's whole tree native windows, and
    the offscreen platform crashed destroying such a main window once a dock had floated.
  - QDockWidget: AutoHide = hidden plus a checkable button on a side tool bar of its area
    (`esses.autohide.<side>`); the button or `DockHost::focus` flies it out (shown docked, state still
    AutoHide) until the focus leaves it. Perspective: `{"backend": "qdockwidget", "version": 1, "mainWindow":
    <base64 QMainWindow::saveState>, "docks": {id: state}}`.
  - A perspective of the other backend or a damaged one is refused (`restorePerspective` false; the shell then
    resets the docks with an Info row).
- **Layout persistence (`LayoutFile`, `layout-1`).** `<project>/.local/layout.json`, or `<config>/layout.json`
  without a project. `MainWindow::restoreLayout` (constructor, `openProject`): window geometry, the active
  workbook's perspective (else `_default`), Values "All channels", the active worksheet (`active`, else the
  workbook's `activeWorksheet`); a refused perspective or unreadable file resets the docks with an Info row
  (Project). `saveLayout` writes it on workbook switch, Reset Dock Windows, project change and exit
  (`closeEvent`, else the destructor; `setSaveLayoutOnExit(false)` for `--dump-commands`).
- **MainWindow.** Wires the workspace as `libs/qtmodel/README.md` describes (units from `<data>/units.json`
  plus `<config>/units.user.json`, settings, shipped defaults, corrected-speed schema and settings, rule
  runner, `connectSourceLoader`, the Messages log sink, the chunk-cache budget hook -> Messages Budget rows);
  one `WorksheetViewModel` + `WorksheetContext` per worksheet of the active workbook; workbook combo and
  worksheet tab bar above the canvas; the status bar shows `WorksheetViewModel::cursorInfo` (t, d, lap, Δt, Δd)
  and hints/errors for 5 s. Commands registered here: `file.open_log`, `file.save_project`,
  `file.open_project`, `view.data_window`, `view.channels_window`, `view.values_window`,
  `view.messages_window`, `view.return_focus` (canvas `restoreMaximised`, else `returnFocus`),
  `view.reset_docks`, `view.worksheet_n`, `view.prev_worksheet`, `view.next_worksheet`; then
  `registerSelectionCommands` / `registerViewCommands` (`data.overlays_toggle` re-registered checkable
  "Overlays" for the toolbar) and every other keymap command disabled with a "planned for <milestone>" /
  "reserved" tool tip. Menus follow 06 §2.2 in keymap row order (commands with arguments stay out); the toolbar
  is open, save | time/distance, overlays, single colour. Component edits (`WorksheetCanvas::componentEdited`)
  replace the worksheet in the session's workbook. Channel drops (`application/x-esses-channels`) on a
  component that does not take drops itself are handled by the shell (`addChannel`, CTRL = new panel).
- **KeyEventRouter.** Application event filter on KeyPress: modal or popup -> nothing; focus in a dock ->
  `[dock context, Global]`, plain keys (letters, digits, arrows, ENTER, SPACE, HOME/END, PAGE, printable)
  offered to the widget first and resolved when they propagate unaccepted; function keys, ESC, DELETE and
  CTRL/ALT chords to the binder first; focus in the canvas -> `[component commandContext(), Component,
  Worksheet, Global]` binder first (plain keys stay in an `esses.textEntry` widget); no focus ->
  `[Worksheet, Global]`; another window -> chords and function keys only, `[Global]`. DELETE is sent as
  "Delete" (the keymap spelling; Qt writes "Del"). A visit at the focus widget (or at the window when nothing
  has the focus) is always a new event, and a KeyRelease forgets the last press: key events are stack objects,
  so a later event may reuse the address of one a dock widget accepted.
- **ESC in the worksheet** (`view.return_focus`): first `ComponentWidget::cancelTransient()` of the focused
  component (the graph's rubber band), then restore a maximised component, then return the focus.
- **Docks.** `DataWindow`: styling proxy over `DataWindowModel` (italic + grey untrusted rows, key colours of
  the slots from the scheme with text tool tips), tree drawn in the Name column, commands
  `data.select_main`/`toggle_overlay0`/`toggle_overlay`/`close_file` on the highlighted row (close = the
  shell's `SourceLoader::close`), clicks in M/O/1, double-click = Main, Trusted checkbox, context menu from
  the shared actions, highlight follows Main. `ChannelsWindow`: plain filter proxy (recursive), first match
  highlighted, ENTER/CTRL+ENTER/double-click -> `addChannelRequested`, DOWN/TAB into the list, drag source.
  `ValuesWindow`: follows the focused component and the active worksheet, "All channels" (persisted), Copy
  Value/Row. `MessagesWindow`: severity buttons with counts, text filter, Clear, Copy, Save As, action rows
  run on double-click/ENTER; auto-shown on the first Error unless settings `ui.doNotAutoShowMessages`.

| Test | Covers |
|---|---|
| `tests/shell/tst_dock_host` | AC-M1-13 on every backend of the build (Qt Test global data rows `ads` and `qdockwidget`; a probe per backend reads its widgets and drives its own controls): registry and tabbing, the worksheet area, float/dock by command and by the dock's own controls (off X11 leaving no native window inside the main window), auto-hide side button and fly-out collapse on focus loss, V toggle and the close buttons, save/restore in the same and in a fresh window (states, floating, moved area), refused foreign/damaged perspectives and the other backend's, `ESSES_DOCK_BACKEND`, `LayoutFile` round trip (with `adsVersion`), unknown members kept, bad files refused |
| `tests/shell/tst_main_window` | assembly (menus in order with key text and no QAction shortcuts, toolbar, every keymap command registered, planned ones greyed, docks, worksheet tabs); F8, ESC, V, C, F11/F12, 1/2/9 through the router, plain keys kept by the Channels search; session restore without a project and per project (layout file content, workbook id keys), Reset Dock Windows; damaged and foreign layouts; `-l` open into the Data window, Import Log rows, CTRL+F, F9, F4 and the toolbar's checked states; Channels ENTER to the focused component; ESC ends a rubber band first; F12/F11 keep the worksheet's components; one Budget row per cache warning with two windows on one runtime; every runtime marks the GUI thread NoBlock (AC-M1-12) |
| `tests/docks/tst_data_window` | ENTER / keypad ENTER, CTRL+ENTER, ALT+ENTER (compact renumbering), DELETE on source vs range rows, arrows and letters stay in the tree, CTRL+F from the dock with the highlight following Main and N/P, M/O/1 clicks and double-click, Trusted checkbox (italic), slot colours and tool tips |
| `tests/docks/tst_values_window` | rows follow the focused component (kept when none), Main and Overlay 1 cells update on a cursor move, time vs distance alignment when the active worksheet changes, "All channels", Copy Row |
| `tests/docks/tst_channels_messages` | Channels filter, first match, ENTER/CTRL+ENTER/DOWN/double-click, drag MIME and parser; Messages severity filters and counts, text filter on text/source, log text and Save As, action rows, Clear |

The shell and dock tests compile `libs/qtmodel/tests/qtmodel_test_support.cpp` into their own executable
(`#include`), so no helper is added to the shared `tests/` glob. AC-M1-06 (one Qt Test per implemented keymap
row through the real key path) is `tests/ui/keymap/tst_keymap.cpp`.

## Components and the worksheet canvas (M1)

- **Wiring.** `ComponentWidget` connects itself to its `WorksheetContext`: `WorksheetViewModel::changed` ->
  `onViewChanged(state, mask)` without the Size bit, `DataSelectionModel::changed` -> `onSelectionChanged`,
  `MathsService::channelReady/statusChanged` and `DistanceService::distanceReady/noDistance` ->
  `onDataChanged`. The shell forwards none of these (doing so would only duplicate requests). A component's own
  size arrives through `resizeEvent`, coalesced to 30 Hz (leading edge at once, then at most every 33 ms; the
  final size always lands); a real DPR change (`QEvent::DevicePixelRatioChange`/`ScreenChangeInternal` with a
  new `devicePixelRatioF()`) is delivered as `Size | Scheme`.
- **TimeDistanceGraph layers.** Static: `RenderService::prepareInputs` (current view, selection, device size,
  DPR) then `submitBuild` (replace key `trace/<worksheetId>/<componentId>`, `Priority::Visible`;
  `TimeDistanceBuilder::buildStatic` decimates and emits on the TaskPool, and the same worker rasterises the list
  with `qt::RasterBackend::rasteriseStatic` in parallel strips: 04 §6.3 step 2 "worker build + raster", added in
  the M1 integration for AC-M1-02) delivered on the GUI thread through the Dispatcher, which only stores the image
  (`LayerCache::storeStatic`; `rasteriseStatic` on the GUI thread only when no image came); requests coalesce to
  one per event-loop turn,
  every delivered build is shown (superseded ones as interim frames), a failed build keeps the old image.
  Dynamic: `buildDynamic` on the GUI thread on Cursor/Datum only, `update(LayerCache::dynamicDamage(old, new))`;
  "…" values retry every 50 ms. Chrome: focus border, caption, active-channel square, pending stripe, banner,
  rebuilt on focus/spec/scheme/pending. A component hidden for 30 s releases its Static image (D-058).
- **Mouse** (routed through `hitAt`, the HitTester kept with the Static layer). Click/drag in a panel = cursor (snapped to the nearest Main sample of the active channel,
  `CursorSource::Click`); wheel = x0.8 / x1.25 about the pointer; drag on the x axis = pan; double-click in the
  trace area, move, click = rubber-band zoom (Dynamic feedback: mouse tracking is on while the band is open,
  so the button-less moves reach the graph; ESC cancels through `cancelTransient`); wheel deltas below a
  notch (high-resolution wheels, touchpads) add up to whole notches; click on a key label = active
  channel; double-click on the caption = maximise (canvas). Keyboard zoom/pan/cursor/F9 are the qtmodel commands
  on the `WorksheetViewModel`; the focused graph reports its trace width (`setSize`) and its active channel's
  sample period (`setActiveSamplePeriod`).
- **Edits.** `addChannel` (like-units grouping by the quantity of the group's first channel resolvable in the
  Main source; `newPanel` or no match = a new group named after the channel, explicit edges compressed),
  `setGroupScale` (Manual vs Channel/Auto), `setActiveChannel` and `ValuesList::addChannel` emit
  `specEditRequested`; `WorksheetCanvas` applies the edit to its `WorksheetDef` and the component and emits
  `componentEdited(spec, undoText)` (the shell stores it in its workbook; the `QUndoCommand` wrap is M2).
- **WorksheetCanvas.** Placement rounding identical to `OffscreenRenderer` (`placement()`); paint order by `z`
  (stable); TAB order `z` then reading order (`focusOrder()`), TAB/SHIFT+TAB from inside a component cycle the
  components (`focusNextPrevChild`); any keyboard focus moving into a component focuses it; the focused
  component is remembered per worksheet id for the session; `setWorksheet` keeps the keyboard focus where it
  was unless it was inside the canvas; `returnFocus()`, `toggleMaximise()`, `restoreMaximised()`,
  `applyScheme()`; relayouts coalesced to 30 Hz like the components (`layoutThrottleActive()` and
  `ComponentWidget::sizeThrottleActive()` let tests wait for an idle throttle). Worksheet switches keep the
  components of the worksheet left, hidden (04 §6.2, D-058): switching back to it (same key, definition and
  context) shows the same widgets with their Static images, and a hidden graph releases its image after 30 s;
  another definition under the key rebuilds that worksheet; `clear()` (workbook or project change) destroys
  every worksheet's components. `MainWindow::selectWorksheet` of the shown worksheet does nothing.

| Test (`tests/components/`) | Covers |
|---|---|
| `tst_time_distance_graph` | Static delivered on the GUI thread; a cursor move (programmatic, mouse press/drag) rebuilds and repaints only Dynamic (no Static submit, no Chrome rebuild, Static image untouched, paint ⊆ damage); wheel zoom about the pointer, x-axis pan, rubber band, keyboard zoom; D-017 colours and draw order (channel, `overlay0`, `overlayPalette`, J, F4) in both domains; F9 through the view model (0.5 s apart in time, aligned in distance, back within one sample, banner without distance); like-units grouping; Manual vs Channel scale; key-label click; 30 Hz Size coalescing; `renderToImage`; nothing blocks a NoBlock GUI thread (AC-M1-12) |
| `tst_worksheet_canvas` | placement and paint order, placeholder for an unregistered type; focus routing (TAB order, click, TAB key, keyboard focus, `returnFocus`, per-worksheet memory, no focus stealing on a switch); maximise/restore; the edit path; 30 Hz relayout (waits for an idle throttle); kept worksheets (D-058); schemes |
| `tst_values_list` | rows/columns/heading/widths from the body, values for Main and the reference overlay following the cursor, `addChannel`, scheme fonts, `renderToImage` |
| `tst_component_dpr` | two offscreen screens (96 and 192 dpi, `configfile`): layers at device size on the 2x screen; moving the window there rebuilds every layer at DPR 2 with the logical placement unchanged |

`tests/components/component_test_support.*` (compiled into every widgets test; namespace
`esses::widgets::cmptest`): golden fonts, the shipped units and schemes, synthetic circuit sources with wheel
speeds and throttle, component specs and a `World` with the qtmodel services of one worksheet.

## Deviations from the design text

- `DockHost(QMainWindow&, QWidget* central, unique_ptr<IDockBackend> = {})`: the backend seam (`IDockBackend`,
  D-153). The ADS code 06 §3.1 puts in `DockHost.cpp` is `AdsBackend` (`ads_backend.cpp`); 06 §3.1's
  `QDockWidgetHost` is `QDockWidgetBackend`, and it is built on every OS, not only where S-3 fails (07 §4.1).
  `DockHost::backend()` exposes the backend for AC-M1-13 on both.
  Additions to 06 §3.1: `IDockBackend::setCentral/resetDock/isFlownOut/container/dockIdOf/setStateObserver/
  libraryVersion`, `DockHost::isVisible/dockIds/defaultArea/defaultState/focusedDockId/dockIdOf/content/
  availableBackends/defaultBackendName/makeBackend`, `stateChanged()`, `dockStateName/parseDockState`,
  `AdsBackend::dockManager`. `ESSES_DOCK_BACKEND` is both a CMake option (the build's default; `qdockwidget`
  builds without ADS) and an environment variable (a run-time choice among the backends the build has), where
  06 §3.1 has a build-time switch only. ADS is linked as `esses::qtads` (08 §3.7.3 has `esses::ads`).
- `LayoutFile` (`layout-1`) lives in `esses_widgets` (the outline places `.local/layout.json` under
  `esses_qtmodel`); besides 06 §3.4's `adsVersion` (written only when ADS wrote the perspectives) it writes
  `dockBackend`; the perspective member stays `ads` whatever backend wrote it. Without a project the layout is kept in
  `<config>/layout.json` (settings-1 has only `window.geometry`). There is no `schemas/layout-1.schema.json`
  yet.
- Menus: items show the key as "Title<TAB>Key" text (no `QAction` shortcut, so the KeyEventRouter stays the
  only key path); commands with arguments (`view.worksheet_n`, `view.workbook_n`, `component.colour_band`) are
  not in the menus; entries not applicable to the focused component are not hidden yet (06 §2.2; M3 with the
  component menus). No Recent submenus, Zoom Summary Bar, command palette or full screen in M1.
- `DataSelectionModel::setDisplayedGroupPath` is set from the active worksheet's `linkTo` on every switch but
  does not notify the Data window (both shipped worksheets link to Laps); filters are M2.
- The Messages Import Log is a run of rows per file, not a collapsible group (M2 with the importers).
- `MainWindow` lives here, not in `esses_app` (01 §2.1): 07 §4.1 lists it under `esses_widgets` and the
  `tests/ui` walkthrough must construct it; `apps/app` keeps `main()` only (D-156, proposed).
- `ComponentWidget` adds `cancelTransient()` (ESC ends a gesture before `view.return_focus`) and
  `sizeThrottleActive()`; `WorksheetCanvas` adds `layoutThrottleActive()` and `keptWorksheetCount()`.
- "…" key values in the graph and the Values window still retry every 50 ms while they wait (no back-off yet;
  event-driven retries via chunk-sealed notifications are an M2 item, `docs/milestones/M1.md` §6).
- `ComponentWidget` uses `QStringList contextCommands()` (command ids) where 04 §7.1 writes `QList<CommandId>`,
  takes a parent widget in `create`, and adds `registerType`/`isRegistered`; `WorksheetContext` carries the
  worksheet id (the RenderService build key) and the scheme as `shared_ptr<const Scheme>`.
- `CommandActions` and `KeyEventRouter` hold the Qt Gui halves of 06 §14's `CommandRegistry::action` and
  `KeymapBinder` (see `libs/qtmodel/README.md`).
- Components (M1): `ComponentWidget` adds `commandContext()` (the first context of the 06 §14.2 key chain:
  Graph for `time_distance`), `isFocusedComponent`/`setFocusedComponent` (the canvas's focused component,
  independent of the Qt keyboard focus), `maximiseRequested`, `layoutDevicePixelRatio`,
  `sizeChangesDelivered`; `TimeDistanceGraph` adds `setGroupScale`, `setActiveChannel`, layer accessors and
  counters for the tests and the bench (`inputs`, `layout`, `hits`, `staticUpToDate`, `setBuildObserver`,
  `lastDynamicDamage`, ...). `links` are always followed (private cursor/zoom with `links.* = false` arrive
  with the properties dialog, M3); vertical zoom/pan (ALT+UP/DOWN) is not in M1's hotkey batch; the pending
  stripe shows only for a Static build still in flight after 100 ms (no flicker on every zoom step).
- `WorksheetCanvas` applies component edits directly and reports `componentEdited` (06 §1 rule 4 asks for a
  `QUndoCommand`; the undo stack arrives in M2, see `libs/qtmodel/README.md`). Move/resize/snap,
  multi-select, splitters, the clipboard and layout lock are M3 (06 §4.3 items 2-5, 8-11).
- `ValuesList`: no datum column (`ValuesModel` has none; `columns.datum` is preserved but not shown).
