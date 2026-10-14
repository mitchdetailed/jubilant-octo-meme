# esses_widgets

Layer 5, Qt Widgets (01 §2.1, 04 §7, 06; D-026, D-027, D-030). The shell widgets, the dock facade, the worksheet
canvas, the component framework with the M1 components and the M1 docks. Built only with `ESSES_BUILD_QT=ON`.

Namespace `esses::widgets` (06 §3.1). Links `esses_qtmodel`, `esses_qtrender` (and the Qt-free libraries), Qt
Widgets and, privately, `esses::qtads` (the Qt Advanced Docking System, a shared library under LGPL-2.1-or-later;
only with `ESSES_DOCK_BACKEND=ads`, the default; `cmake/qtads.cmake`). No other library may link ADS. Compiled with `QT_NO_KEYWORDS` (PUBLIC): write `Q_SIGNALS`, `Q_SLOTS`, `Q_EMIT`. Never calls `TaskPool`
directly: builds go through `qt::RenderService` (06 §1).

Independent glob roots (AUTOMOC on), so parallel agents can work here without touching a shared file (the
three M2 editor roots are listed under "M2 contract" below):

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
  the shared actions, highlight follows Main; M2: the Filters submenu, Apply Filter To All Sources and the
  `LapFilterEditor` (below). `ChannelsWindow`: filter proxy (recursive) over `ChannelsModel::filterAccepts` (M2:
  word prefixes, loose matching from the project), first match highlighted, ENTER/CTRL+ENTER/double-click ->
  `addChannelRequested`, DOWN/TAB into the list, drag source.
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
| `tests/docks/tst_data_window_filters` | (M2) the Filters submenu of a source (every kind, checked, per source), slots kept with the filter icon and tool tip, the filtered source row's icon and name; the lap-number editor (error while typing, Apply refused with the previous filter kept, prefilled on reopen, descending ranges, one editor per window); Apply Filter To All Sources (local action and the shared `data.filter_all_sources`) and its flag; filters in `project.json` `dataWindow`, never on the undo stack |
| `tests/docks/tst_channels_search` | (M2, D-175) word-prefix search through the box (`W S FL`, `temp (ecu)`, `pres front` over `_` and bracket separators, `oil-pres`), order and distinct words, the "no match" note, loose matching re-filtering at once and following `options.general.looseChannelSearch` through an options edit and its undo |

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

## M2 contract (07 §5.1; `docs/milestones/M2.md` §2)

| Root | Headers (`include/esses/widgets/<root>/`) | Contents | Design |
|---|---|---|---|
| `src/editors_maths/` | `maths_editor.hpp`, `channel_status_window.hpp`, `active_channel_commands.hpp`, `channel_documents_dialogs.hpp` (and `editors_data/channel_editor.hpp`) | `MathsEditor` (CTRL+M), `ChannelStatusWindow` (CTRL+U, dock `channelstatus`), `ActiveChannelCommands` (the `channel.*` rows), `ChannelEditor` (CTRL+E) with `AliasesDialog`/`MappingsDialog` | 03 §11, 06 §10, §13.1, §13.2, §14.4 |
| `src/editors_data/` | `channel_editor.hpp`, `mapping_panel.hpp`, `details_editor.hpp`, `lap_editor.hpp` | `ChannelEditor` (CTRL+E, with the Aliases/Mappings sub-editors), `MappingPanel`, `DetailsEditor` (CTRL+D), `LapEditor` | 06 §13.1, §13.3, §13.4, §13.8 |
| `src/editors_options/` | `options_dialog.hpp`, `scheme_selector.hpp` | `OptionsDialog` (Tools > Options), `SchemeSelector` | 06 §13.7, §16 |

Tests: one Qt Test per root under `tests/<root>/` (`widgets.tst_maths_editors_contract`,
`tst_data_editors_contract`, `tst_options_editors_contract`: construction over the real services offscreen),
replaced by their owners' behaviour tests (03 §11 rows, 06 §13, AC-M2-05, -11, -12). The shell work of 07 §5.1
(Edit-menu undo/redo, the lap-stretch toolbar toggle, active-channel hotkeys, the Data-window filters and the
Channels-window search, both in `src/docks/`) is not part of these roots.

### M2-STUB items the implementing agents must replace

- `src/editors_maths/editors_maths_stubs.cpp` (removed): `MathsEditor` and `ChannelStatusWindow` are implemented in
  `maths_editor.cpp` and `channel_status_window.cpp`, `ChannelEditor` in `channel_editor.cpp` (see "M2 maths editors"
  below); `tst_maths_editors_contract` stays as the construction smoke test.
- `src/editors_data/editors_data_stubs.cpp` (removed): `ChannelEditor` moved to `src/editors_maths/` (see its
  header); `MappingPanel`, `DetailsEditor` and `LapEditor` are implemented (see "M2 data editors and dock work"
  below).
- `src/editors_options/editors_options_stubs.cpp` (removed): `OptionsDialog` and `SchemeSelector` are implemented
  in `options_dialog.cpp` and `scheme_selector.cpp` (see "M2 options editors and the shell's M2 wiring" below);
  `tst_options_editors_contract` is replaced by `tst_options_dialog` and `tst_scheme_selector`.

Contract rules (all M2 libraries): an `M2-STUB` is replaced in place (or moved into the owner's root) by
its implementation and the marker removed; `grep -rn "M2-STUB" libs` lists what is left. A signature that another
library already consumes (the shell, `apps/app`) changes only together with that consumer; the owner may
reshape the others. Tests of a stub's behaviour belong to its owner; the `*contract*` tests cover what is final.

### Deviations of the M2 contract from the design text

- `SchemeSelector` is a new public name for 07 §5.1's "scheme selector" (06 §2.1 toolbar combo, §13.7 Colours tab).
- The Aliases and Mappings dialogs of 06 §13.1 item 7 are `AliasesDialog` and `MappingsDialog`
  (`editors_maths/channel_documents_dialogs.hpp`, public so that `tools.aliases`/`tools.mappings` and the tests reach
  them), opened by `ChannelEditor::openAliases`/`openMappings`.

## M2 maths editors (`src/editors_maths/`; 07 §5.1, AC-M2-12, D-177)

Each header states the behaviour as built; D-177 (proposed) records where it departs from 03 §11 and 06 §10,
§13.1, §13.2 and §14.4.

- **MathsEditor** (03 §11, 06 §13.2). Modules pane (global modules in project order with enable boxes, Add /
  Delete / Up / Down / Import / Export and the condition line; "Local maths" with one node per open source;
  "Transient" per source with filters from a channel label, read-only), items table (enable box, kind, name, rate,
  unit, status for the Main source at 10 Hz, comment; Add Expression / Constant / Filter / Scale & Offset, Delete,
  Cut, Copy, Paste keeping names, Recalculate) and the item page (Expression, Constant, Filter, Scale & Offset;
  other kinds shown and preserved). The expression field is checked by `qt::LiveDiagnostics` 150 ms after a
  keystroke against the Main source (`setMainSource`; else the first open source; data-less: `I-UNRESOLVED`);
  the line under it shows the most severe diagnostic (Errors red, Warnings amber, Info grey; property
  `esses.level`), every span is underlined and the full list is below. The `[unit]` popup (a list over the dialog
  that never takes the focus) offers the units of the referenced channel's, constant's or item's quantity
  (compat units left out), filters while typing, ENTER/TAB insert the symbol and close the bracket, ESC closes it
  only. Insert Range (the Main source's group paths, double-quoted) and Insert Channel (`'Name'[unit]`, the
  override's display unit else the stored unit). The tooltip over an operator is the precedence table with the
  current `unaryMinusAbovePower` reading of `-2^2`; over a function, its signature, arities and description.
  Apply, or leaving the item (another row, module, `openAt`, a structural edit), writes the item as one
  `EditMathModulesCommand` (global) or `SetLocalMathsCommand` (local); an Error refuses it, and close/ESC are
  refused while the unapplied item has one, with the reason and Revert in a bar. A new item or constant exists
  only once applied (its page starts dirty). Closing applies a valid page and, when this dialog edited a global
  module and a project is open, saves the maths modules and their order (`ProjectSession::saveMathsModules`; the
  project's other edits stay unsaved, D-190; `message()` on failure).
- **ChannelStatusWindow** (06 §10, AC-M2-12). The model through a filter proxy (word prefix on the channel or a
  substring of Source / Kind / Status / Resolved to; "Errors only" on `ErrorRole`), sortable (Rate by value),
  error rows red, warnings amber, pending italic; the detail pane shows the row with its module, status,
  "Resolved to" and every trace step; the current row survives the model's resets by row id. Copy Table (TSV with a
  header line). Double-click or ENTER: `openItemRequested("maths"|"alias"|"mapping", ...)` and, when registered and
  enabled, `tools.maths_editor [module, item]`, `tools.aliases [alias]`, `tools.mappings [logged name]` (the
  mapping's logged name from its "Resolved to"); built-in outputs and missing references open nothing.
  `focusChannel` (channel.status) clears a filter that hides the row.
- **ChannelEditor** (06 §13.1). Rows: every channel of every open source and, with a `MathsService`, the maths
  outputs that are not self-maths of a logged channel (Category "Maths"; Corr Dist's unit is editable). The cells
  show the override where it applies (a display unit of another family than that file's channel is not applied
  there). Unit (combo delegate of the quantity's units), Min/Max (numbers in the display unit, stored as
  `{v, unit}`), DPS, Colour (-1 = automatic) and Interp edit one channel at a time through `SetOverrideCommand`;
  "Clear override" is a `ClearOverrideCommand`; a unit that another open file's channel cannot take is refused
  with that file's name (`editRefused`, `lastRefusal`), as are unknown units, interpolations and non-numbers.
  Sort by value (`Qt::UserRole`), filter with `ChannelsModel::filterAccepts` (loose with the project option).
- **AliasesDialog / MappingsDialog** (06 §13.1 item 7). Modeless editors on a copy of the document (reloaded while
  clean); Apply refuses empty or repeated names, empty candidates, circular aliases (`Aliases::cycleThrough`),
  mapped-twice logged names and unknown unit stamps, else pushes one `EditAliasesCommand` / `EditMappingsCommand`.
- **ActiveChannelCommands** (06 §14.4 "Active channel", 03 §11 item 10). `registerCommands` adds
  `channel.next_active`, `prev_active`, `hide`, `remove`, `edit_maths`, `status`, `filter`, `scale_offset` and
  `zero_at_cursor` (Component context, the keys of `data/keymaps/default.json`), enabled while the focused component
  has an active channel (the first visible trace when it names none; hide/remove on a `time_distance` graph). Spec
  edits go out as the component's `specEditRequested`; CTRL+SHIFT+F builds the Main source's transient module
  (`setTransientModule`, dropped on `sourceClosed`); CTRL+SHIFT+O and CTRL+SHIFT+Z write a `scale_offset` item of the
  Main source's local maths (`SetLocalMathsCommand`, the channel's DPS kept); zero at cursor takes the value
  displayed at the cursor through `qt::ValuesModel` (the single conversion rule of 04 §3.2) in the trace's display
  unit, as a delta. `hint()` carries the status-bar texts (a logged or built-in channel, a value not available yet).

Wiring (the shell, not done here): one `MathsEditor(session, maths, &loader)` per workspace behind
`tools.maths_editor` (with `setMainSource` from the selection's Main and `openAt(args)` when the command carries
`[module, item]`), the `channelstatus` dock with a `ChannelStatusWindow` over a `ChannelStatusModel(maths,
&session)` behind `tools.channel_status`/`view.channel_status`, a `ChannelEditor(session, loader, &maths)` behind
`tools.channel_editor` (and its dialogs behind `tools.aliases`/`tools.mappings`), and an `ActiveChannelCommands`
with a `Host` of the canvas's focused component, the active worksheet and those two windows, its `hint()` to the
status bar. The `channel.*` keymap rows stay `planned` until that wiring and their `KEYMAP_TEST` rows exist.

| Test | Covers |
|---|---|
| `tests/editors_maths/tst_maths_editor` | the live diagnostics after the debounce with level colours and underlines; W-UNIT-MISSING amber and "warning does not block Apply"; Apply as one undo step and undo reloading the page; "cannot close with error" (close and ESC), no apply and no switch while invalid, Revert; an item switch applying the edit, the quote refused in names, E-NAME-EMPTY; editing without data (I-UNRESOLVED); "[unit] popup lists quantity units" (filtering, ENTER/arrows, existing brackets, a constant, ESC closing the popup only, compat units hidden); "Insert Range inserts quoted path" (also from its menu) and Insert Channel; hover help with the compat switch, a function's help, the tooltip event; module add (reserved and repeated ids refused) / enable / condition / move / delete / import / export and item add / constant (unknown unit refused) / filter / copy / paste with names kept / cut / enable / delete, one step each, undo to the start; local maths in the sidecar and undo; transient maths listed read-only; the status column and Recalculate; auto-save on close only after a module edit, and of the modules only (`closeSavesOnlyTheModules`, D-190) |
| `tests/editors_maths/tst_channel_status_window` | AC-M2-12: the mapping, alias, case-fold, missing and logged-name steps in the detail pane, "(missing)" alias and workbook references kept; error colour and the trace tool tip; the filter (word prefix and other columns), errors only, Copy Table, sorting, the current row across a reset, status updates in the detail pane; `focusChannel` through a filter; double-click and ENTER on a maths item, a constant, an alias and a mapping (signal and command), nothing for built-ins and references |
| `tests/editors_maths/tst_channel_editor` | rows of two files and the maths outputs, the search, sorting by value, `focusChannel` through a filter; unit / min / max / DPS / colour / interp edits as one `SetOverrideCommand` each with the display conversions and refused input, Clear override and undo; the unit rule with the conflicting file named and lifted when it closes; the Corr Dist unit; the Aliases (cycles and repeats refused, enable boxes) and Mappings (unknown units and repeats refused) editors as one step each |
| `tests/editors_maths/tst_active_channel` | the nine keymap rows resolving to the commands in the graph's chain and PgDown dispatched; disabled without a focused component; next/previous wrapping and skipping hidden traces, hide and remove (an emptied group goes); CTRL+SHIFT+M at the item and hints for logged and built-in channels; CTRL+SHIFT+S on a maths item and an alias, a hint for a logged channel; CTRL+SHIFT+F transient (never a document or sidecar edit, replaced, gone after close and reopen); CTRL+SHIFT+O local maths with the DPS kept and an unknown unit refused; CTRL+SHIFT+Z on a deg C channel stored in K reading 0 at the cursor, twice |
| `tests/editors_maths/tst_maths_editors_contract` | construction over the real services offscreen |

## M2 data editors and dock work (`src/editors_data/`, `src/docks/`; 07 §5.1, AC-M2-05, AC-M2-11)

`MappingPanel`, `DetailsEditor` and `LapEditor` live in `src/editors_data/{mapping_panel,details_editor,lap_editor}.cpp`
(their M2-STUBs are gone; `ChannelEditor` moved to `src/editors_maths/`). Each header states the behaviour as
built; D-179 (proposed) records where it departs from 05 §8.1 and 06 §5.4, §6, §7, §13.3, §13.4 and §13.8.

- **MappingPanel** (05 §8.1, 06 §13.8, D-168). Follows `SourceLoader::mappingRequired()` itself and queues further
  pending opens behind the one it shows (`showPending` is the same entry; a shown or queued id is ignored). The
  bar names the file and how many channels wait. Rows: Vendor name, Vendor unit, Sample values, Analysis name,
  Quantity, Unit, Rule, Apply to future files, Status; Quantity, Unit and Rule edit through combo boxes whose
  choices are the cell's `ChoicesRole` (units of the row's quantity as canonical symbols, compat units left out;
  a unit picked first sets the quantity, a unit of another quantity is refused). Proposals: the project's unit
  map of the importer, else the units table, else the importer's inferred quantity (Scan re-runs them). Resolve
  refuses while a row has no quantity and unit; otherwise one "Resolve mapping" step: the ticked rows into
  `channels/unitmaps/<importerStem>.json` (`SetUnitMapCommand`), the analysis names as project Mappings
  (`qt::applyMappingDecision`), then task B with every row. Save pushes the complete rows into the unit map
  ("Save unit map", written to disk by the project's save and the journal) and leaves the open pending until
  Resolve or Open Anyway. Open Anyway opens with the complete rows and the rest unitless; Cancel drops the open
  (Import Log `open.mapping_cancelled`). `pendingChanged(bool)` tells the shell when to show it.
- **LapEditor** (06 §13.3, D-014, D-016). Rows: the laps of the profile's primary group in time order and the
  ignored beacons of its marker group (greyed, class "Ignored"); columns Number (header from the profile's
  `terms.range`), Class, Start (session time `m:ss.fff`), Duration, Trusted, Stretch (`LapStretchService`; blank
  outside the tolerance, grey while stretching is off), Provenance (Logged, Companion, User, Derived, Edited).
  Follows the Main source until a source is picked in the combo. Insert by session time, by lap time after the
  highlighted lap, or at the main cursor (one `InsertMarkerCommand`; the laps rebuild before the call returns);
  per-beacon time offset of the highlighted lap's start beacon (a marker edit for a logged beacon, a move for a
  user beacon); beacon offset by distance with Apply (`MarkerEditCommand` `offsetDistance` in metres; shown
  "pending": no resolver moves the beacons yet, `docs/milestones/M2.md` open issues); Trusted through
  `DataSelectionModel::setTrusted`; Restore Logged Laps (one command); the "Lap distance stretching" check box and
  the Stretch Report. Free functions for the shell: `insertBeaconAtCursor`, `registerInsertBeaconCommand`
  (`cursor.insert_marker`, Graph, Shift+F7) and `connectLapStretchToggle` (the toolbar's `data.lap_stretch_toggle`
  action follows the service whoever toggles it).
- **DetailsEditor** (06 §13.4, D-015). Columns: Detail, then one per open source; rows: the profile's details,
  then the other ids of the open sources except the derived ones (File, Statistics). A cell edits as text (empty
  allowed), `<number> [unit]` (the profile's unit when none is typed; a unit of another quantity or an unknown one
  is refused; the decimals as typed are kept) or an ISO 8601 date-time, through one `SetDetailCommand`; an edited
  cell shows a dot and its original in the tool tip; Restore Original (button and context menu); sort by any
  column, Profile Order; Copy as TSV (`tableText`, `copyAsTsv`). A Venue edit emits `venueRenamed` (the
  track-map prompt arrives with track maps, M3).
- **DataWindow filters** (06 §5.4, §6). The context menu is built by `contextMenu(index)` (also without
  `CommandActions`): Filters (checkable All, Selected, Fastest, Fastest 3, Within 1 % of Fastest, Lap
  Numbers...), Apply Filter To All Sources (the shared `data.filter_all_sources` action, else a local one),
  Trusted, Lap Editor... and Details... when the shell has registered `tools.lap_editor`/`tools.details`, Close
  File. `LapFilterEditor` is non-modal: the parse error shows as you type, ENTER or Apply sets the filter or
  keeps the previous one and the editor open with the error. Ranges kept only by their slot and filtered
  source rows carry a filter icon (and say so in the tool tip).
- **ChannelsWindow search** (06 §7 item 3, D-175). `ChannelsModel::filterAccepts` with `setLooseMatch`;
  `followSession` follows `options.general.looseChannelSearch` (on open and every document change); a query that
  matches nothing shows a note under the box; the box's tool tip explains the syntax.

| Test | Covers |
|---|---|
| `tests/editors_data/tst_mapping_panel` | AC-M2-11 over the real open pipeline: the panel appears by itself, publication blocked (no worker waits), Resolve refused while incomplete, edits through the model (units filtered by quantity, unit-first, refusals), one "Resolve mapping" step (ticked rows into the unit map, the analysis name as a Mapping), the units on the published source, `channels/unitmaps/generic.csv.json` written by the project's save, the next file asking only for what the map lacks, Save completing the map without opening, a third file opening silently, undo; the combo-box editors; Scan over a map saved meanwhile; queued opens, Cancel (Import Log) and Open Anyway |
| `tests/editors_data/tst_lap_editor` | the time text; rows, terms and provenance; AC-M2-05 in the editor (inserts by session time, by lap time, through the controls, synchronous rebuild, one command each, a later lap keeping its trust flag and offset); the cursor insert and `cursor.insert_marker`; per-beacon offsets (logged and user beacons), the offset by distance (pending, 0 removes it), Restore Logged Laps deleting the op; ignored beacons and a Partial lap after a reset; the source combo and following Main; the Stretch column, report and toggle shared with the toolbar action (no undo command) |
| `tests/editors_data/tst_details_editor` | typed parsing; profile-first rows without derived details; edits through the model as SetDetailCommands with provenance User, the dot and the original; empty strings; numbers kept as typed (also on reopen) and refused units; Restore Original, undo and redo; the Venue signal; Copy as TSV; sort and Profile Order; a closed source leaving the grid |

## M2 options editors and the shell's M2 wiring (`src/editors_options/`, `src/shell/`; 07 §5.1, AC-M2-06, D-178)

- **OptionsDialog** (06 §13.7, 02 §6.2). Tabs General, Analysis, Colours, Fonts, Keys (the two read-only tabs are
  built, not slipped). Every field's object name is its option key (`general.autosaveMinutes`,
  `maths.compat.unaryMinusAbovePower`, ...; user settings by their settings key) and shows the project option, or
  the 02 §6.2 default when the member is absent. Apply/OK write only the members the user changed as one
  `SetProjectOptionsCommand` "Change options" (nothing for no change; the view state in `project.json` is never
  touched) and the user settings (`language`, `ui.schemeSelectorOnToolbar`, `scheme.followOs`); an invalid field
  (a spin box holding no number) refuses both with the reason under the tabs. Shown again it re-reads the options;
  after an undo or redo it follows them unless edits are pending. Additions to 06: the lap-distance stretching
  tolerance on the Analysis tab (`lapStretch.tolerancePct`, D-174), and the Keys tab's import (checked for conflicts
  against the shipped keymap, written to `<config>/keymap.json`, applied at the next start), export (the effective
  keymap) and reveal (`setRevealHandler` replaces the file manager in tests).
- **SchemeSelector** (06 §2.1, §13.7, §16). The shipped light and dark schemes, then `<config>/schemes/*.scheme.json`
  by name (a file that does not load or repeats a name is left out with a `logMessage`; a partial one is filled
  from the shipped scheme of its background, one message per item). A choice calls `ProjectSession::setScheme`
  (live, `scheme.active`) and turns `scheme.followOs` off; `applySystemScheme` follows `QStyleHints::colorScheme()`
  while that is on; `restoreActive` brings back a user scheme at start-up; Duplicate ("<name> copy", selected),
  Import (a new name only) and Export; `palette()` maps a scheme to the application palette (06 §16 rule 6).
- **RecoveryDialog** (`src/shell/recovery_dialog.cpp`; 06 §18 step 3, AC-M2-06). "Recover unsaved changes?" with the
  journalled documents (a file changed on disk since is marked) and the replay's log; Recover, Discard, Later
  (closing it is Later); window-modal, opened with `open()`, never a nested event loop.
- **MainWindow (M2).** Adopts the qtmodel wiring: `connectProjectSession`, `connectMathsSelection`,
  `registerTextProfiles` (messages as Import warnings), a `LapStretchService` with `registerLapStretchCommand` (Data
  menu and toolbar), `registerEditCommands` (Edit menu and toolbar, the undo/redo text in the tool tips) and a
  `SessionRestore`. A component edit from the canvas is a `WorkbookCommand`; every `documentsChanged()` re-syncs the
  workbook selector, the worksheet tabs (rebuilt when worksheets were added, removed, renamed or moved, keeping the
  shown one by id) and the shown worksheet's components. `tools.options` opens the dialog window-modal with its
  geometry from `layout.json` (`LayoutFile::dialogGeometry`). The toolbar is open, save | F9, overlays, single
  colour, lap stretch | the scheme selector (shown by `ui.schemeSelectorOnToolbar`) | undo, redo. A scheme change
  updates every worksheet context, repaints the canvas and the Data window and sets the application palette;
  `QStyleHints::colorSchemeChanged` reaches the selector. `openProject(folder, logs)`: leaving a project first
  (`confirmLeaveProject`), the recovery prompt when a journal is pending, then `SessionRestore` with the logs (after
  the answer, so a recovered selection is the one restored). Leaving with edits saves (`options.general.saveOnExit`)
  or asks Save/Discard/Cancel (`setExitPrompt` in tests; Discard deletes the journal); leaving with view-state
  changes only writes them, unless a journal was put off with Later. The title marks a dirty project with "*". The
  Messages dock auto-shows on the first error only while `options.general.autoShowMessagesOnError` is on (and the
  user's `ui.doNotAutoShowMessages` is off).
- **KeyEventRouter (M2).** The platform's text-editing chords (undo, redo, cut, copy, paste, select all) in an
  editable `QLineEdit`, `QPlainTextEdit`, `QTextEdit`, spin box or `esses.textEntry` widget go to the field first,
  like plain keys, in docks, the canvas and other windows (06 §13.2 item 6).
- **The M2 editors in the shell** (D-180): the `MappingPanel` sits between the workbook/worksheet bar and the canvas,
  hidden until its `pendingChanged(true)`, with the loader's hand-off on (unknown units stop at it); the
  `channelstatus` dock (Bottom, Closed) holds a `ChannelStatusWindow` over the window's `ChannelStatusModel`
  (`tools.channel_status` CTRL+U with an optional channel argument, and `view.channel_status`, both show, focus and
  refresh it); the Maths Editor (`tools.maths_editor` CTRL+M, `[module, item]`; Main source from the selection),
  Channel Editor (`tools.channel_editor` CTRL+E, `[channel]`) with its Aliases (`tools.aliases [alias]`) and Mappings
  (`tools.mappings [logged name]`) editors, Details (`tools.details` CTRL+D) and Lap Editor (`tools.lap_editor`, with
  the lap-stretch service) are modeless dialogs created on first use (accessors `mathsEditor()`, `channelEditor()`,
  `lapEditor()`, `detailsEditor()`), shown, raised and activated by their command, their geometry kept in
  `layout.json` `dialogs.<key>` when they close; `registerInsertBeaconCommand` (SHIFT+F7) and an
  `ActiveChannelCommands` (`activeChannelCommands()`, rows under Component/Active Channel) are registered; editor
  messages go to Messages, hints and refusals to the status bar. The canvas has the focus when the window comes up.

| Test | Covers |
|---|---|
| `tests/editors_options/tst_options_dialog` | the five tabs; defaults for absent members; Apply as one undoable command with only the changed members, undo/redo shown in the fields, the view state untouched, no command without a change; user settings and a setting changed elsewhere; an invalid field refusing Apply and OK; Cancel dropping edits; the Fonts tab following a scheme change; the Keys tab (user keys vs defaults, import with the conflict check, export, reveal); Duplicate revealing the user folder |
| `tests/editors_options/tst_scheme_selector` | shipped then user schemes, skipped and filled files with their messages; a choice live in the session and every selector, `scheme.active`, follow OS turned off, never on the undo stack; `restoreActive`; follow OS (Qt 6.8+: a dark OS picks the dark scheme); Duplicate, Import, Export; the palette mapping |
| `tests/shell/tst_shell_m2` | Edit, Tools and Data menu items and the toolbar; component edits undone and redone by CTRL+Z/CTRL+Y (worksheet and Data window) and the toolbar with the canvas following; CTRL+Z in the Channels search box as text undo; the lap-stretch toggle as view state; Options window-modal (keys paused), its geometry in `layout.json`, an applied option on the stack; a scheme switch reaching the graph, the hidden worksheet and the palette, kept per user; save on exit, Save/Discard/Cancel when leaving for exit or another project, the view state written on exit; auto-show Messages; the RecoveryDialog's list and answers |
| `tests/ui/tst_lap_editor` | AC-M2-05 in the shell on a 60-lap outing: Tools > Lap Editor, SHIFT+F7 in the graph rebuilding the laps before the key returns as one step, a later lap's trust flag and offsets following its start marker, CTRL+Z/CTRL+Y, a beacon time offset, Restore Logged Laps as one step |
| `tests/ui/tst_mapping_panel` | AC-M2-11 in the shell: a CSV with unknown units beside an `.ld`, the panel above the worksheet area, publication blocked, Resolve as one step with CTRL+Z/CTRL+Y, CTRL+S writing the unit map, a silent next open; Save then Open Anyway, a later file asking only for what the map lacks |
| `tests/ui/tst_project_recovery` | AC-M2-06 in the shell: a reopen restores the previous data, selection, single colour, filter and worksheet; a kill after the autosave leaves the files intact and the next open prompts, waits, recovers (options, the graph's channel, the journalled selection) and saves with CTRL+S; Later and Discard |
| `tests/ui/keymap/tst_keymap` (M2 rows) | `edit.undo`, `edit.redo` (keys, also from the Data window), `tools.options`, `data.lap_stretch_toggle`, `view.messages_window`, `view.reset_docks` (menu-only rows through their action); the editors' rows (D-180): `tools.maths_editor`, `tools.channel_status`, `view.channel_status`, `tools.channel_editor`, `tools.aliases`, `tools.mappings`, `tools.details`, `tools.lap_editor`, `data.filter_all_sources`, `cursor.insert_marker` and the nine `channel.*` keys over the focused graph |

## M3 contract (07 §6.1; `docs/milestones/M3.md` §2; D-193)

New roots (the `components` glob is recursive; `properties_shell` is a new entry of the root list in
`CMakeLists.txt`). Component headers stay flat in `include/esses/widgets/components/`:

| Root | Headers | Contents | Design |
|---|---|---|---|
| `src/components/time_distance/` | `time_distance_graph.hpp` | `TimeDistanceGraph` (source moved here from `src/components/`) | 04 §8 |
| `src/components/plots/` | `scatter_plot.hpp`, `histogram.hpp`, `outing_graph.hpp`, `numeric_gauge.hpp`, `bar_gauge.hpp`, `layered_component.hpp` | `ScatterPlot` (`scatter`), `Histogram` (`histogram`), `OutingGraph` (`outing`; moved here with its owner, D-198 (h)), `NumericGauge` (`gauge_numeric`), `BarGauge` (`gauge_bar`), `LayeredComponent` | 04 §9-11, §16-17 |
| `src/components/reports_track/` | `channel_report.hpp`, `time_report.hpp`, `track_position.hpp`, `track_map_window.hpp`, `track_editor.hpp` | `ChannelReport`, `TimeReport`, `TrackPosition`, `TrackMapWindow` (the Track Map dock's content), `TrackEditor` | 04 §12-14, 06 §9, §13.5 |
| `src/properties_shell/` | `include/esses/widgets/properties_shell/` | `PropertiesDialog` (`openFor`, `find`), `FieldBinder` (`FieldKind`, `FieldBinding`), `ZoomSummaryBar`, `CorrectedSpeedDialog`, `LayoutEditor`, `CommandPalette`, `AnimationController`, `ComponentClipboard` | 06 §4.2-4.5, §12, §13.6, §15; 04 §7.2 |

`registerBuiltinComponents()` registers all ten MVP types; `ComponentWidget::openProperties` calls
`PropertiesDialog::openFor` (D-193 (f)). The shell work of 07 §6.1 that lives in shared M1 files (`WorksheetCanvas`
multi-select, splitters, maximise and clipboard; `MainWindow`'s zoom summary bar, Track Map dock, palette,
animation and Layout Editor wiring; the L/K/G/S and remaining hotkey rows; the shared behaviours in
`component_widget.cpp`) belongs to the properties/shell work; the component owners ask through the coordinator.

### M3-STUB items the implementing agents must replace

- `src/components/reports_track/`: replaced (below, "M3: the reports and track components"). Their
  `overlayPolicy()` (04 §7.4) and `commandContext()` (06 §14.2) are final.
- `src/properties_shell/`: replaced (below, "M3: properties and shell"; was `PropertiesDialog` but `component`
  and `binder`, `FieldBinder::load`/`store`/`validate`/`schemaPointers`, `ZoomSummaryBar`'s painting and mouse,
  `CorrectedSpeedDialog`, `LayoutEditor`, `CommandPalette`, `AnimationController`'s driving of the worksheet,
  `ComponentClipboard::toMimeData`/`fromMimeData`/`placeForPaste`).

Final (tested by `tests/components/tst_m3_components_contract.cpp` and
`tests/properties_shell/tst_properties_shell_contract.cpp`): the registrations, overlay policies and key
contexts; `TrackMapWindow::setContext`/`context`; `FieldBinder::bind`/`bindings`/`boundPointers`/`isBound` (a
`Custom` row binds its subtree); `AnimationController::kRates` and its state signals;
`ComponentClipboard::kMimeType` and `Payload`. `tst_worksheet_canvas` now uses the v1.0 type `gauge_dial` as its
unregistered component.

Deviation: `PropertiesDialog::openFor` instead of `open`, which would hide `QDialog::open()` (MSVC C4263).

### M3: the plots components (as built, D-198)

`src/components/plots/` holds `ScatterPlot`, `Histogram`, `OutingGraph`, `NumericGauge` and `BarGauge`; every
`M3-STUB` of the root is replaced. They derive from `LayeredComponent` (`layered_component.hpp`, a public base
added by this work), which gives them `TimeDistanceGraph`'s layer pipeline with their plotcore builders
(`ScatterGeometry`, `HistogramGeometry`, `OutingGeometry`, `GaugeGeometry`): the Static build and raster on the
TaskPool through `RenderService`, the Dynamic and Chrome lists on the GUI thread, the 100 ms pending stripe and a
50 ms retry of values waiting for a chunk, the 30 s release of a hidden component, `renderToImage` (the current
composite; another DPR rebuilds Dynamic and Chrome over the scaled Static image) and `hitAt` from the Static hit
regions. Each component names the change bits that rebuild its Static layer (04 §7.4): scatter and histogram
every view change but the cursor and the datum; the outing graph and the gauges keep Zoom and Offsets on the
Dynamic layer too. Spec edits go out as `specEditRequested` (the canvas's undo command) and come back through
`applySpec`.

| Component | Behaviours | Export Data (CSV) |
|---|---|---|
| `ScatterPlot` | G overlapped → tiled → average, S points → pointsTrend → trend, Y cross → ring → none, K/L legends (`handleCommand`); LEFT/RIGHT and CTRL step one/ten Main points; a click picks the nearest Main point within 8 logical px (`nearestPoint`'s tag) and sets the cursor; box zoom (double-click, move, click; ESC), wheel x0.8 and an axis double-click (restore) on its own axes, as widget view state injected as manual scales (`adjustInputs`, `ScatterGeometry::axes`); `component.copy_equation` copies the drawn Main equation; ENTER adds a Y trace, CTRL+ENTER sets X | `set,x,y[,z]` per point of every set, `<Y>@<slot>` |
| `Histogram` | G overlapped ↔ tiled, S bar → line → text, K/L; a click on a bin of a tile makes its trace active; the wheel zooms the worksheet about the cursor; ENTER adds a trace | `bin_lo,bin_hi,<trace@slot>...` in the y mode |
| `OutingGraph` | the zoom rectangle's ends (setZoom) and bottom edge (pan at constant span) from `zoomRect()` with a 5 logical px grab (`hitAt` reports `ZoomSpan` and the `HandleKind`); a click or drag places the cursor; the wheel (x0.8 about the pointer) and the rubber band (ESC) set the worksheet zoom; a double-click on a lap cell zooms to it; S line ↔ points, L key labels ↔ none; like-units grouping; `enableOverlays` widens `overlayPolicy()` to AllOverlays | `Time [s]` and its channels in Main |
| `NumericGauge`, `BarGauge` | ALT+n (`component.colour_band [n]`) toggles colour band n (a band beyond the list is a handled no-op); the channel drop replaces the channel | `Time [s]` and the channel in Main |

Exports run on the TaskPool (`plots::runOffGui`: `RenderService` with a key of their own, Background) while the GUI
thread waits in a local event loop that excludes user input; the ranges are 04 §7.2 item 8's. ALT+n is not handled
by scatter and histogram (a `BandAxis` has no per-band visibility, D-198 (f)); the display commands a component
does not handle return false so that the shell's generic behaviour applies (`ComponentWidget::handleCommand`).
For the shell work: `component.copy_equation` (Component menu only) and the Export Data dialog (an `ExportSink`
over the chosen file) are to be registered and wired in `MainWindow`; the keymap rows L, K, G, S, ALT+n and Y
(summary) can become `implemented` with their `KEYMAP_TEST`s over these components.

Tests (offscreen): `tst_scatter_plot` (points and the Dynamic-only cursor, Overlay 0 beneath Main in its colour,
the click pick and LEFT/RIGHT, the cycles and toggles, the trend equation and Copy Equation, box zoom, wheel,
ESC and the axis restore, the colour legend, the CSV), `tst_histogram` (ten bin hit regions, the cursor bin on
the Dynamic layer, outlined overlays beneath Main, the cycles, the active trace by a click, the wheel, the CSV
against a count made in the test), `tst_outing_graph` (a zoom and a cursor move rebuild no traces, the handles,
a lap's double-click, the wheel, the rubber band and ESC, the cursor click, S/L, like-units grouping,
`enableOverlays`, the CSV), `tst_gauges` (Main and the reference at the cursor against the source's formula,
Dynamic-only cursor and zoom, colour bands and ALT+n with the no-op beyond the list, background mode, the
channel drop, the bar from its origin and the reference marker, `bandMode` bar, the CSV over the current and the
padded range); shared helpers in `tests/components/plots_test_support.hpp` (header only). Pixel goldens:
`tests/golden/images/tst_golden_plots.cpp` (three cases per component, provisional Windows renders, D-127).

### M3: the time_distance completion (`src/components/time_distance/`, as built, D-201)

`TimeDistanceGraph` covers parity-first §5.4 / 04 §8 over `plot::TimeDistanceBuilder` (the builder's M3 parts are in
`libs/plotcore/README.md`, "M3: the time_distance completion"):

- **Inputs**: after `RenderService::prepareInputs` the graph applies the WorksheetViewModel's pending alignment
  offsets (a sub-axis drag or a key burst moves the traces at once, 04 §3.3 item 4) and the project's channel
  override scale limits (`ProjectSession::overrides()` through the selection's session, in base units:
  `ResolvedChannel::limitMin`/`limitMax`). Each Static build also returns a `plot::TdStaticInfo` (band entries,
  markers, measurement positions, scales, the scrollbar extent), kept for the mouse and the tooltips.
- **Commands** (`handleCommand`; the shell offers them to the focused component first): F3 `graph.variance`, M
  `graph.measurements`, E `graph.status_errors`, SHIFT+E `graph.device_events`, Y `cursor.style` (line →
  crosshair → none), G `component.cycle_layout` (panels ↔ overlapped), S `component.cycle_style` (line ↔ points)
  as undoable body edits; CTRL+SHIFT+C `graph.copy_cursor_values` (`name\tvalue\tunit` per configured channel at
  the Main cursor; with a datum `name\tvalue@main\tvalue@datum\tdelta\tunit`); ALT+UP/DOWN/F2 `zoom.v_in`/
  `v_out`/`v_full` and SHIFT+UP/DOWN `pan.up`/`down` on the active group's vertical zoom (local view state, not
  persisted); the programmatic forms of the mouse rows `zoom.rubber_band` (a band from the cursor),
  `zoom.full_axis` (`["y"]` vertical), `zoom.wheel` (`[notches]` about the cursor), `pan.drag_axis`
  (`[fraction]`), `cursor.place` (`[x]` snapped) and `offset.drag` (`[reference, amount]`, one commit).
- **Offsets** (AC-M3-09): the offset keys and O are the shell's rows on the WorksheetViewModel; the focused graph
  reports each visible slot's offset step (`WorksheetViewModel::setSourceSamplePeriod`: one sample period of the
  active channel in that slot's source), so a range moves by its own source's period, 0.25 m in distance. A
  sub-axis drag holds the commit (`beginOffsetDrag`/`endOffsetDrag`) and commits once at the release.
- **Mouse**: cursor placement honours `cursorSnap`; double-click a range or section band entry: zoom to it (a click
  there never moves the cursor); the scrollbar thumb drag pans, a click on its track pages (±90 %), a double-click
  is full zoom (F2; the vertical bar: full vertical); dragging a y axis or the vertical thumb pans vertically; ALT
  + rubber band zooms the active group vertically, CTRL a box; dragging a Lo/Hi handle (overlapped) or a panel
  splitter (in the axis strip) edits `lo`/`hi` or the edges at the release (one undo command); hovering a min/max
  measurement draws its position line (Dynamic, widget-local) and a double-click moves the cursor there;
  tooltips name markers, annotations, device events, band entries, offsets and status rows (a bit field as its
  set bits); right-click an annotation: Edit / Remove (undoable sidecar edits through
  `DataSelectionModel::commitSidecar`; Add Annotation is the shell's `cursor.add_annotation`).
- **Layers**: the datum appearing or disappearing rebuilds the Static layer (the key's value@datum and Δ
  columns); moving it stays Dynamic only. Export Data writes the graph's channels as CSV through the plots
  work's `plots::timeSeriesCsv` on the pool (`plots_common.hpp`).
- **Tests** (offscreen): `tests/components/tst_time_distance_m3` (the body commands and what they show, F3 with
  and without overlays, the datum's Static/Dynamic split, the measurement hover line and double-click, band
  double-clicks, the scrollbar, the vertical zoom by keys, rubber band and scrollbar, splitter and Hi-handle
  drags, the offset sub-axis drag with pending offsets in the inputs and one commit at the release, per-source
  offset steps, copy of the cursor values, the CSV, annotation Add / Edit / Remove (the menu's Remove included)
  through the selection's SidecarCommit hook with their undo texts, the project's override limits scaling the
  group and drawing the min / max lines, tooltips of annotations, device events and status rows; the test `World`
  takes an optional `ProjectSession` for the selection);
  `tst_time_distance_graph` (M1) now places the datum before measuring that a datum move is Dynamic only. Pixel
  goldens: the thirteen M3 cases of `tests/golden/images/tst_golden_images.cpp` (provisional Windows renders).
- **For the shell work**: the graph answers `zoom.v_in`, `zoom.v_out`, `zoom.v_full`, `pan.up`, `pan.down` and
  `graph.copy_cursor_values` (their rows can become `implemented` with `KEYMAP_TEST`s over the graph) and the mouse
  rows' programmatic forms above (`zoom.rubber_band`, `zoom.full_axis`, `zoom.wheel`, `pan.drag_axis`,
  `cursor.place`, `offset.drag`), which can be offered to the focused component like the dispatch rows.

### M3: the reports and track components (as built, D-199)

`src/components/reports_track/` holds `ChannelReport`, `TimeReport`, `TrackPosition`, `TrackMapWindow` and
`TrackEditor`; every `M3-STUB` of the root is replaced.

- **Report tables** (`report_table.hpp`, private): two `QTableView`s over one `ReportStyleProxy` and one selection
  model, the left one showing column 0 only (the frozen row labels), their vertical scrolling coupled; column widths
  come from the cells' text metrics and the layout is settled before a Print to Clipboard, so a table always
  renders the same; the caption bar (04 §7.3) above and the focus border (scheme `cptFocusBorder`) around; fonts
  `reportTable`/`reportHeading`, colours `data*`. The proxy maps `qt::ReportModel`'s styling roles to
  `Qt::BackgroundRole` (the highlight or band colour at 45 % over the table background) and `Qt::FontRole`
  (italic untrusted rows, Missing and Error cells; bold fastest times and summary rows).
- **`ChannelReport`**: a `qt::ReportModel` over `RenderService::pool()`/`dispatcher()`/`statCache()`; the body's
  `report` (`ReportSpec::fromJson`), `layout`, `deltaVsOverlay0`, `highlight` (on with #2060ff/#ff3030 by default)
  and `timeFormat`; refresh on Selection and on Data of a source of the selection, and on Zoom and Domain while
  `report.options.zoomLinked`; `addChannel` appends the channel with avg, min and max (one spec edit); Export Data
  writes the table as CSV, `copyTsv` puts it on the clipboard (also the table's context menu), Print to Clipboard is
  `QWidget::render`. G (`component.cycle_layout`) flips the layout through the shell's generic edit.
- **`TimeReport`**: the same over `TimeReportSpec::fromJson(body)` and `timeFormat`; `toggleBand(n)`, also
  `handleCommand("component.colour_band", [n])` and ALT+n in `keyPressEvent` where no command runs, flips
  `bands[n].visible` (one spec edit; a band beyond the list does nothing).
- **`TrackPosition`**: its own copy of the layer pipeline (Static on Size, Scheme, Selection, Data, Offsets, Scale
  and `TrackMapService::mapChanged`; Dynamic on Cursor, Datum, Zoom, Domain) over `plot::TrackGeometry`; the map is
  `ComponentBuildInputs::trackMap` from `RenderService::prepareInputs`. PLUS / "=" / MINUS (`track.zoom_in`/
  `zoom_out` through `handleCommand`, or the keys themselves), the wheel about the pointer, a drag and a
  double-click (fit, `track.fit`) while focused; each step edits the body's `scale` at once and the gesture is one
  spec edit 400 ms after it rests; a backgrounded map ignores presses.
- **`TrackMapWindow`**: the embedded `TrackPosition` rebuilt over each worksheet context (links forced on, the
  dock's spec kept; its edits apply at once, not undoable); the actions Fit, Show Sections, Show Section Labels,
  Track Editor..., Regenerate Track... (the last two signal the shell); a "No worksheet" note without a context.
- **`TrackEditor`** (`(ProjectSession&, DataSelectionModel&, MathsService&, TrackMapService&, parent)`; the
  contract's `SourceLoader` parameter is gone): the lap picker over every lap of the open sources (Main by
  default), GPS (inner/outer laps, smoothing) or Speed + Lateral G (the lateral channel among the source's
  acceleration channels, "positive to the right"); Generate through `TrackMapService::generate` (a pending maths
  input retries on `channelReady`/`statusChanged`) with a preview and the closure error / RMS fit; Accept writes the
  map with default sections (`SectionDetector::defaults`) as one `TrackMapCommand` (a later Accept keeps the user
  sections that still fit); the sections table with add (numerically, or from `setCursorSpan`'s span), edit,
  delete (each one command; >= 10 m and on the track); Regenerate Default Sections; rename and delete of the
  track file; "Auto-generate on open" (`trackMaps.autoGenerateOnOpen`, a `SetProjectOptionsCommand`).

Tests (offscreen, `tests/components/reports_track/`, helpers in `rt_support.hpp` over the qtmodel fixtures
`libs/qtmodel/tests/reports/report_track_fixtures.hpp`, header only): `tst_channel_report` (the frozen column,
cells, highlight tints, italic untrusted rows, refresh on selection, data and the zoom link, the body's layout and
delta columns, `addChannel`, CSV and TSV, caption, fonts, the focus border, Print to Clipboard at DPR 2),
`tst_time_report` (laps, sections and summaries, band tints and bold fastest times, ALT+n and `handleCommand`,
italic untrusted laps, the time format, refresh, CSV and TSV), `tst_track_position` (the venue map and the cursor
dot at the hand-computed track position, Dynamic-only cursor, zoom and domain changes, the banner and a map
arriving, the Track commands through a `KeymapBinder`, keys, wheel about the pointer, drag, double-click, one spec
edit per gesture, a background map, Print to Clipboard against `OffscreenRenderer`), `tst_track_map_window`,
`tst_track_editor` (generation of the AC-M3-04 synthetic circuit within 2 m RMS, Accept with default sections
>= 10 m, undo/redo of the file, user sections persisting and reloading, regeneration keeping them, rename, delete,
the project option, a missing Speed+LatG input) and `tst_reports_track_goldens` (three cases per component at DPR
1 and 2: provisional pixel goldens under `tests/golden/images/{channel_report,time_report,track_position}`, exact
table goldens under `tests/golden/tables/` and DrawList JSON goldens `tests/golden/drawlist/track_position/
widget_*`).

### M3: properties and shell (`src/properties_shell/`, the shell files; as built, D-200)

Every `M3-STUB` of `src/properties_shell/` is replaced. New public headers beside the contract's:
`field_editors.hpp` (the editors a `FieldBinder` binds, `jsonptr`, `SchemaTree`), `shell_dialogs.hpp`
(`SelectWorksheetDialog`, `UnitsListDialog`, `OpenLogDialog`) and `component_commands.hpp`
(`genericComponentEdit`, `hasGenericComponentEdit`); private `property_pages.hpp` (the tabs and binding tables
of the ten MVP types). Shell files: `worksheet_canvas.{hpp,cpp}` (rewritten), `main_window.hpp`,
`main_window.cpp`, `main_window_impl.hpp` (the `MainWindow::Impl`, now shared by two sources) and
`main_window_m3.cpp` (the M3 widgets, menus and commands); `component_widget.hpp` gains `handleCommand` and
`captionBarHeight`.

- **`ComponentWidget::handleCommand(id, args)`** (default false): the shell offers the rows only a component can
  carry out to the focused component (`zoom.v_*`, `pan.up`/`down`, `graph.copy_cursor_values`, `track.*`,
  `component.copy_equation` and the mouse rows' programmatic forms); a row is enabled while the focused
  component's key context is one of the row's; the worksheet rows of hotkey batch 1 go to the focused component
  first while the keyboard is in the canvas. Rows a component does not take that edit its body (L, K, G, S,
  ALT+n, Y, E, SHIFT+E, F3, M) are `genericComponentEdit`, one spec edit each.
- **`FieldBinder`**: `load`/`store`/`validate`/`schemaPointers` and `setSchema`, `loadJson`/`storeJson`,
  `editorFor`, `isModified`. A row whose editor still shows what `load()` put there leaves the body's member as
  it was (absent stays absent, numbers exact); a changed row is validated against the body schema's node of its
  pointer. Leaf editors are plain Qt widgets (`NumberEdit` keeps the exact value until edited); subtrees are
  `FieldEditor`s (`UnitValueEdit`, `FieldForm`, `RecordListEditor`, `FixedArrayEdit`, `OptionalEditor`), whose
  `leafEditor(pointer)` reaches any descendant (creating records on the way).
- **`PropertiesDialog`**: modeless, one per component (`openFor`/`find`), tabs from `property_pages.cpp`, the
  "Component" header group (caption, links, legend) on the Display tab through an internal binder; Apply/OK one
  spec edit "Edit <type> properties", Cancel restores the spec at open; refreshes on
  `WorksheetCanvas::componentSpecApplied` (undo/redo), closes with its component; `focusChannel` (CTRL+N);
  geometry per type through `setGeometryStore` (layout.json `dialogs."properties.<type>"`).
- **`WorksheetCanvas`**: in-place sync (widgets keep their state when their id remains), selection (click,
  CTRL+click, band on empty canvas, `selectAll`), move/resize/splitter/nudge with grid snap and the 4 x 4 cell
  minimum, `sendToBackground`/`bringToFront`, `setLayoutLocked`; every layout change is `layoutEdited(text,
  mergeKey)`, a merged `WorkbookCommand` in the shell; a press on a component's own control in the edge zone
  stays the component's.
- **`ComponentClipboard`**: the payload as MIME type and text/plain; `placeForPaste` keeps sizes in whole cells,
  offsets one cell per paste into the same worksheet (the shell counts per worksheet and clipboard content),
  fresh ids. `MainWindow::copyComponents`/`cutComponents`/`pasteComponents`/`deleteComponents`.
- **`CommandPalette`** (CTRL+SHIFT+P): providers for `>`, `@`, `#`; invocation after the popup closed with the
  previous focus restored (the context a key would see); prefix matches, then recent (`palette.recent`), then
  title; argument rows per keymap row; `?` lists disabled commands.
- **`AnimationController`**: drives `WorksheetViewModel::setAnimating`/`setAnimationRate`, keeps its state
  without a worksheet, follows worksheet switches, stops at the end of the outing; `data.animate` (A, tool bar)
  and `data.animation_rate [rate]` (Data > Animation Rate, `options.animation.rate`, one undo step).
- **`ZoomSummaryBar`**: above the worksheet tabs; `buildDrawList(Summary, LayoutContext, Scheme)` (DrawList
  goldens `tests/golden/drawlist/zoom_summary/`, provisional PNGs `tests/golden/images/zoom_summary/`); click
  sets the cursor, drag moves the span, SHIFT+click extends it (`zoom.extend`), double-click zooms full.
- **`CorrectedSpeedDialog`** (Tools): the module's settings, enabled flag and `options.lapStretch` as one
  `SetProjectOptionsCommand`. **`LayoutEditor`** (CTRL+F7): the workbook/worksheet tree and its edits as the
  qtmodel workbook commands, Lock Layout (`ui.layoutLocked`), import/export through `Prompts`.
- **Shell** (`MainWindow`): the zoom summary bar, the Track Map dock (`trackmap`, AutoHide right) and one
  `TrackMapService` (`trackMaps()`) for RenderService, SourceLoader, the dock and the Track Editor; the M3 rows
  of 06 §14.4 (file, view, tools, component, offset, cursor rows), full screen, Print to Clipboard, Export Data,
  `component.copy_equation` (menu only); `setPrompts` replaces the file, folder, text and choice dialogs.

Tests (offscreen): `tests/properties_shell/` `tst_properties_roundtrip` (AC-M3-08: every body-schema leaf of
the ten types bound, mutated, applied as one undo step, saved and reopened identical, full and empty bodies),
`tst_field_binder`, `tst_component_clipboard` (AC-M3-11), `tst_palette` (AC-M3-12, with the key-versus-palette
harness), `tst_zoom_summary_bar` (DrawList JSON goldens exact, PNGs provisional), `tst_corrected_speed_dialog`,
`tst_layout_editor`, `tst_properties_shell_contract`; `tests/shell/` `tst_worksheet_canvas_m3` (selection,
gestures, splitters, nudge, z order, lock), `tst_shell_m3` (summary bar, Track Map dock, animation without
Static builds, the dialogs of the rows, full screen, offsets and annotations, merged geometry edits, the
component rows following the focus, projects and Close All); `tests/ui/keymap/keymap_m3_cases.hpp` (every M3
keymap row, mouse rows by pointer events). AC-M3-10: `bench/plot_bench` `plot.animation_frame_10x`.
