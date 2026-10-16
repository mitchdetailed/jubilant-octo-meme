# 06 — UI shell: `esses_qtmodel`, `esses_widgets`, `esses_app`

**Scope.** This document specifies the three Qt layers of Esses: the observable application model (`esses_qtmodel`, Qt Core only), the widget library (`esses_widgets`, Qt Widgets + Qt Advanced Docking System) and the executable (`esses_app`). It covers `MainWindow`, `DockHost`, the Workspace › Workbook › Worksheet › Component navigation and `WorksheetCanvas`, `DataSelectionModel` and `WorksheetViewModel`, the six MVP docks, the properties dialogs and all editors, `CommandRegistry` + `keymap.json` with the complete i2 hotkey table, the command palette, scheme JSON, HiDPI/multi-monitor, session restore, file associations and CLI arguments, accessibility basics and the deliberate deviations from i2. Component internals (trace building, geometry, reports) and the plot engine are specified elsewhere; this document only fixes how components plug into the shell. Behaviour not fixed here follows `background/design-panel/parity-first.md` §5.3, §5.10, §7.1–§7.7 per D-001.

**Related decisions.** D-003, D-006, D-007, D-010, D-013, D-014, D-015, D-016, D-017, D-018, D-019, D-022, D-025, D-026, D-027, D-029, D-030, D-031, D-032, D-033, D-034, D-035, D-036.

---

## 1. Targets, boundaries and rules

| Target | Contents | Qt | Thread rule |
|---|---|---|---|
| `esses_qtmodel` | `DataSelectionModel`, `WorkspaceViewModel` (shared zoom/cursor/datum, §4.5), `WorksheetViewModel`, `ProjectSession` (+ `QUndoStack` commands), `SourceLoader`, `MathsService`, `RenderService` (keyed, cancellable build submission over `TaskPool` with `Dispatcher` delivery, 04 §6.3), `Dispatcher`, `CommandRegistry`, `KeymapBinder`, `SettingsStore`, `MessagesModel`, Qt item models (`DataWindowModel`, `ChannelsModel`, `ValuesModel`, `ChannelStatusModel`) | `Qt::Core`, plus Qt Gui's `QUndoStack`/`QUndoCommand` and no other Qt Gui class (D-161); no widgets, no `QImage` | All `QObject`s live on the GUI thread (D-031) |
| `esses_widgets` | `DockHost`, `MainWindow` building blocks, `WorksheetCanvas`, `ComponentWidget` and the ten MVP components, the six docks, `PropertiesDialog`, all editors, `MappingPanel`, `ZoomSummaryBar`, `CommandPalette`, `SelectWorksheetDialog`, `LayoutEditor` | `Qt::Widgets`, `Qt::Gui`, ADS (shared lib, D-026) | GUI thread only; never calls `TaskPool` directly, only `esses_qtmodel` services (trace, geometry and summary builds go through `RenderService::submitBuild`) |
| `esses_app` | `main()`, `MainWindow` assembly, menus/toolbars from `CommandRegistry`, argument parsing, single-instance forwarding, file associations, crash-safe exit | `Qt::Widgets` | — |

Rules:

1. `QString` appears from `esses_qtmodel` upwards; conversion from UTF-8 `std::string` happens in the item models and view-models (`QString::fromStdString`), never below (D-007).
2. Every worker result enters this layer through exactly one `Dispatcher::post(QObject* context, std::function<void()>)`; the closure is skipped when `context` has been destroyed, captures identifiers and values rather than raw pointers, and no `QObject` is touched off the GUI thread (D-031, 01 §5.3 rule 3).
3. Widgets hold Qt-free documents by value or `std::shared_ptr<const …>` (`ComponentSpec`, `SelectionSnapshot`, `ViewState`, `std::shared_ptr<const DataSource>`) and never hold raw pointers into `esses_project` documents owned by `ProjectSession`.
4. Every user-visible mutation of a project document goes through a `QUndoCommand` on `ProjectSession::undoStack()` (D-032); view state (selection, cursor, datum, zoom, domain, dock layout, file open/close) does not.
5. Every action a user can take is a command id in `CommandRegistry` (D-030). Menus, toolbars, context menus, hotkeys, the command palette and the Qt Test rows are generated from that table; a widget never creates a bare `QAction` with its own shortcut.
6. No string, identifier, asset or file extension in these targets may contain `motec` or `i2` except the importer ids and the Open-dialog format labels on the allow-list (D-003).

---

## 2. `MainWindow`

### 2.1 Anatomy

| Region | Widget | Contents |
|---|---|---|
| Menu bar | `QMenuBar` built from `CommandRegistry` menus | File · Edit · Layout · Data · Component · Tools · View · Help |
| Toolbar | `QToolBar` ("Main") | Open log (`file.open_log`), Save project (`file.save_project`), Time/Distance toggle (`data.toggle_domain`, checked = distance), Overlays on/off (`data.overlays_toggle`), Single-colour Main (`data.single_colour_main`), Lap stretch on/off (`data.lap_stretch_toggle`), Animate (`data.animate`), Scheme selector combo (visible when Options › Colours "show scheme selector on toolbar" is on), Filter-all-sources (`data.filter_all_sources`), Undo/Redo |
| Central widget | `QWidget` with vertical layout | Workbook selector row (`QComboBox` of workbooks + "+" button; CTRL+0–9) · `ZoomSummaryBar` · worksheet `QTabBar` (0–9, F11/F12, right-click menu: New, Rename, Duplicate, Delete, Move left/right, Layout Editor…) · `WorksheetCanvas` |
| Docks | ADS `CDockManager` via `DockHost` | Data (F8), Channels (C), Values (V), Track Map, Channel Status (CTRL+U), Messages |
| Status bar | `QStatusBar` with fixed-width `QLabel`s | `t` cursor time (project time format), `d` cursor distance (Corr Dist units), lap label of the range under the cursor, `Δt` / `Δd` when the datum is on, maths queue indicator (`n pending`, click opens Channel Status), memory budget indicator (click opens Messages), transient importer messages (5 s) |

The central widget is installed as the ADS central widget so docks never cover it unless floating. The worksheet tab bar shows the active workbook's worksheets only; the workbook combo lists every `workbooks/*.workbook.json` in the project in `project.json` order.

### 2.2 Menus

| Menu | Items (command ids) |
|---|---|
| File | New Project…, Open Project…, Recent Projects ▸, Save Project (`file.save_project`), Save Project As… (v1.0 archive), Close Project · Open Log File… (`file.open_log`), Recent Log Files ▸, Close All Log Files (`file.close_all`), Import Data… (text profiles; opens the Mapping panel on unknown units), Export Data… (CSV in MVP; `.ld`/`.mat` v1.0), Print… (v1.0, greyed in MVP), Print to Clipboard (`component.print_clipboard`) · Exit |
| Edit | Undo (`edit.undo`), Redo (`edit.redo`) · Cut/Copy/Paste/Delete component (`component.cut`…) · Select All Components (`component.select_all`) |
| Layout | New Worksheet, Rename Worksheet, Duplicate Worksheet, Delete Worksheet, New Workbook, Rename Workbook, Delete Workbook, Layout Editor… (`layout.editor`), Select Worksheet… (`view.select_worksheet`), Lock Layout (`layout.lock`) |
| Data | Time/Distance (`data.toggle_domain`), Overlays On/Off, Clear Overlays, Single Colour Main, Next Range, Previous Range, Select Fastest Lap, Select All Fast Laps, Swap Main and Overlay 0, Animate, Animation Rate ▸ (0.25×, 0.5×, 1×, 2×, 4×), Lap Distance Stretching, Filters ▸ (per-source filter of the Main source), Apply Filter To All Sources |
| Component | Add ▸ (per registered type), Properties…, Active Channel ▸, Cursor ▸, Zoom ▸, Pan ▸, Display ▸, Export Data…, Print to Clipboard, Maximise, Send to Background — every entry is a §14.4 command id |
| Tools | Channel Editor…, Channel Status, Aliases…, Mappings…, Maths Editor…, Details…, Lap Editor…, Track Editor…, Corrected Speed and Distance…, Units List…, Options…, Setup Sheets (reserved, greyed) |
| View | Data Window, Channels Window, Values Window, Track Map Window, Channel Status, Messages, Zoom Summary Bar, Toolbar, Status Bar · Reset Dock Windows (`view.reset_docks`) · Full Screen (`view.fullscreen`) · Command Palette… (`view.command_palette`) · Return Focus To Worksheet (`view.return_focus`) |
| Help | Help Contents (`help.contents`), Keyboard Shortcuts (opens the palette filtered to bound commands), About (versions of Esses, Qt, ADS; licence notices link) |

Menu items show the key from the active keymap, so rebinding updates the menus. The Component menu is also the component context menu (right-click) with the same ids; entries not applicable to the focused component type are hidden, not greyed.

---

## 3. `DockHost` over Qt Advanced Docking System

### 3.1 Facade

```cpp
namespace esses::widgets {
enum class DockArea { Left, Right, Top, Bottom, Center };
enum class DockState { Docked, Floating, AutoHide, Closed };

class DockHost : public QObject {
  Q_OBJECT
public:
  explicit DockHost(QMainWindow& host, QWidget* central);
  void addDock(const QString& id, const QString& title, QWidget* content,
               DockArea defaultArea, DockState defaultState, const QString& iconName);
  void show(const QString& id);               // make visible (restores from AutoHide/Closed), no focus change
  void focus(const QString& id);              // show + give keyboard focus to the dock's primary widget
  void toggle(const QString& id);             // visible <-> Closed (V semantics)
  void setState(const QString& id, DockState);
  DockState state(const QString& id) const;
  QByteArray savePerspective() const;         // opaque ADS state (CDockManager::saveState)
  bool restorePerspective(const QByteArray&); // false => caller falls back to resetToDefault()
  void resetToDefault();                      // every dock to its registered default area/state
  QWidget* focusedDock() const;               // nullptr when the worksheet has focus
signals:
  void dockFocusChanged(const QString& id, bool focused);
  void visibilityChanged(const QString& id, bool visible);
};
}
```

The ADS API (`CDockManager`, `CDockWidget`, `CAutoHideDockContainer`) is visible only inside `DockHost.cpp`. A second implementation, `QDockWidgetHost`, provides the same facade on `QMainWindow::addDockWidget`/`saveState`, with `AutoHide` mapped to `Closed` plus a side toolbar button; it is selected by `ESSES_DOCK_BACKEND=qdockwidget` at build time (macOS fallback, D-026).

### 3.2 Dock registry

| id | Title | Hotkey | Default area / state | Primary focus widget | Milestone |
|---|---|---|---|---|---|
| `data` | Data | F8 | Left, Docked | range tree | M1 |
| `channels` | Channels | C | Left (tabbed under Data), Docked | search line edit | M1 |
| `values` | Values | V (toggle) | Right, Docked | table | M1 |
| `trackmap` | Track Map | — | Right (tabbed under Values), AutoHide | map widget | M3 |
| `channelstatus` | Channel Status | CTRL+U | Bottom, Closed | table | M2 |
| `messages` | Messages | — | Bottom (tabbed with Channel Status), AutoHide | list | M1 |

i2 state names map as: Docked → `Docked`; Pop-up → `Floating`; Fly-out (tab shown, pin off) → `AutoHide`. The ADS auto-hide tab's pin icon toggles `AutoHide` ↔ `Docked`; dragging a title bar off the dock area produces `Floating`; double-clicking a floating title bar re-docks at the last docked position (ADS default behaviour, kept).

### 3.3 Focus model (i2 rule 1 of the UI behaviours)

1. A dock that has keyboard focus receives every key event first; its own widgets consume plain letters, digits, arrows, ENTER, SPACE, HOME/END, PAGE UP/DOWN. Function keys and modifier chords that the dock does not consume fall through to `KeymapBinder` in `global` context (§14.2), so F2, F4, F8, F9, CTRL+F, CTRL+0–9 work from inside any dock. Every non-letter data or zoom key that must work from a dock is therefore bound in **Global** context and acts on the active worksheet (§14.4: `data.toggle_domain` F9 and `zoom.full` F2 are Global; F3, SHIFT+F7 and the arrow keys stay in graph context because they act on the focused graph).
2. Single-letter and unmodified-arrow commands (`worksheet`, `component`, `graph`, `summary`, `track` contexts) act only when focus is inside `WorksheetCanvas`.
3. ESC inside a dock, or a click on the worksheet, moves focus to the last focused component (or the canvas when none); the dock keeps its state. ESC inside a modeless dialog closes it only if it has no pending edits; ESC in a modal dialog cancels.
4. `view.data_window` (F8) and `view.channels_window` (C) are `focus(id)`; `view.values_window` (V) is `toggle(id)`; CTRL+U is `focus("channelstatus")`.
5. When a dock in `AutoHide` state is focused by command it flies out and stays out until focus leaves it.

### 3.4 Perspectives ↔ workbooks and `.local/layout.json` (schema `layout-1`, D-019)

A perspective is the opaque byte string from `DockHost::savePerspective()`. One perspective is stored per workbook id plus one `_default` entry; switching workbooks restores that workbook's perspective if present, else keeps the current layout. Perspectives are per user and per project and are **not** part of the workbook document, so a shared workbook carries no personal dock state.

```json
{
  "$schema": "urn:esses:layout-1",
  "schemaVersion": 1,
  "appVersion": "0.1.0", "qtVersion": "6.7.2", "adsVersion": "4.3.1",
  "window": { "geometry": "<base64 QMainWindow::saveGeometry>", "maximised": true,
              "screen": { "name": "\\\\.\\DISPLAY1", "dpr": 1.5 } },
  "active": { "workbook": "b7c1…", "worksheet": "Overlay" },
  "perspectives": {
    "_default": { "ads": "<base64 DockHost::savePerspective>", "savedAt": "2026-10-09T18:22:04Z" },
    "b7c1…":    { "ads": "<base64>", "savedAt": "2026-10-09T18:30:11Z" }
  },
  "docks": { "trackmap": { "state": "AutoHide" }, "messages": { "state": "Closed" } },
  "dialogs": { "lapEditor": { "geometry": "<base64>" }, "mathsEditor": { "geometry": "<base64>", "splitter": [220, 640] } }
}
```

This example validates against 02's `layout-1` schema (02 §6.7, D-071); `active` is the last viewed workbook and worksheet. Rules: written atomically (temp + rename) on workbook switch, on `Reset Dock Windows`, on project close and on exit; never written by autosave; `appVersion`/`qtVersion`/`adsVersion` mismatch on load triggers `restorePerspective` and, on failure, `resetToDefault()` with an info row in Messages; a missing or invalid file is not an error. The `.local/` folder is excluded from `backup/`, from the v1.0 archive and from `esses-cli validate --project` unless `--local` is passed. Dialog geometries (`dialogs`) and splitter positions persist here as well (UI rule 15: sizes/positions remembered between sessions).

### 3.5 Reset Dock Windows

`view.reset_docks`: every dock to its registered default, floating windows re-docked, auto-hide tabs rebuilt, the window un-maximised to 1366×768 if it does not fit the current screen, perspective for the active workbook deleted from `layout.json`.

---

## 4. Workspace › Workbook › Worksheet › Component

Terminology: **Workspace** = the open project folder (D-019; exactly one open at a time). **Workbook** = one `workbooks/<name>.workbook.json` (`workbook-1`) with an `id` (UUID assigned at creation; a file without `id` gets one derived from `Hash128(file name)` on load) and ordered worksheets. **Worksheet** = `WorksheetDef { name, linkTo, domain, grid, components[] }`. **Component** = `ComponentSpec` with the header of `04_RENDERING_AND_COMPONENTS.md` §7.3 (`id, type, version, rect[l,t,r,b], z, caption{show,auto,text}, links{zoom,cursor,datum}, legend, activeChannel, body, x-i2`), which is canonical.

### 4.1 Navigation

1. Worksheet tabs: click, `view.worksheet_n` (keys 1–9 select tabs 1–9, key 0 selects tab 10), `view.prev_worksheet`/`view.next_worksheet` (F11/F12, wrap at the ends).
2. Workbooks: combo, `view.workbook_n` (CTRL+1–9, CTRL+0 = 10th), CTRL+F11/CTRL+F12 (wrap).
3. `view.select_worksheet` (F7) opens `SelectWorksheetDialog`: a two-level list (workbook › worksheet) with a type-to-filter line, UP/DOWN, ENTER selects workbook **and** worksheet, ESC cancels; the previously active entry is preselected.
4. Switching worksheets changes neither `DataSelectionModel` nor the shared view: zoom, cursor and datum are one workspace-level state in `WorkspaceViewModel` (i2 has one current zoom level, drawn in the `ZoomSummaryBar` above the worksheet tabs), so flipping from a driver sheet to a chassis sheet keeps the corner in view. Each worksheet keeps only its `domain` (initialised from `WorksheetDef.domain` when first shown; i2 remembers the time/distance mode per worksheet) and its offset-axis visibility; when the newly active worksheet's domain differs from the domain the shared zoom is expressed in, the F9 conversion (04 §3.4) is applied to the shared zoom on the switch. At session start zoom = Main range (W) and cursor = range start. This amends D-018's per-worksheet view state (proposed, DECISIONS.md Addendum A; whether i2 also keeps cursor/datum across worksheets is a D-005 (3) verification item).
5. Active workbook/worksheet are persisted in `.local/layout.json` (`active`); `WorksheetDef`/`WorkbookFile` `activeWorksheet` fields are used only when the layout file has no entry (first open of a shared workbook).
6. Tab reorder by drag is a `MoveWorksheetCommand` (undoable).

### 4.2 Layout Editor (CTRL+F7, MVP basics)

Requirements:

1. Tree of workbooks and their worksheets with the active item preselected.
2. Move Up / Move Down (worksheet within its workbook; workbook within the project order).
3. Copy To… / Move To… another workbook (worksheet, with its components; `linkTo`/`domain` preserved).
4. Rename worksheet/workbook (inline edit; duplicate names within a workbook rejected with the reason shown).
5. Delete worksheet/workbook (confirmation naming the number of components; last worksheet of a workbook cannot be deleted; last workbook of a project cannot be deleted).
6. Duplicate worksheet (same as the tab context-menu item; name `<name> (2)`).
7. New worksheet (name, `linkTo` from the profile's data views, domain) and New workbook (name; file name slugified, collision → suffix).
8. Import Workbook… (copies a `*.workbook.json` into `workbooks/`, validates `workbook-1`, runs migrations, lists unresolved channel names in Messages) and Export Workbook… (copies the document; never includes `.local` state).
9. Every change is one `QUndoCommand`; the dialog is modeless; `ProjectSession::documentsChanged` refreshes it.
10. Lock Layout (`layout.lock`, persisted in `project.json` `ui.layoutLocked`): disables move/resize/delete/paste in `WorksheetCanvas`; properties and channel edits remain allowed.

### 4.3 `WorksheetCanvas` — free fractional placement

1. Coordinates: every component rectangle is `[l,t,r,b]` in `0..1` fractions of the canvas content rect; the canvas converts to logical pixels on layout; i2 `RectGridPts` (grid units of `GridSize`) and `PosPC` (per-mille) convert losslessly on import (D-019). Overlap is allowed; z-order is the persisted `z` field of each component (04 §7.3: ascending paint order, highest on top, array order only breaks ties).
2. Snap grid: 1 % of width/height (`grid = [100, 100]` default; the value is per worksheet and editable in the Layout Editor between 20 and 200). Moves and resizes snap to the grid unless ALT is held; the minimum size is 4 × 4 grid cells.
3. Add: Component ▸ Add ▸ type, or right-click an empty canvas area ▸ Add ▸ type; the new component takes the largest empty rectangle ≥ 20 % × 20 % if one exists, else the full canvas, and receives focus. Pasting places the clipboard components at their stored rectangles, offset by one grid cell per successive paste into the same worksheet.
4. Move: drag the caption bar or the top-left handle, or SHIFT+drag anywhere on the component; arrow keys with ALT held nudge the focused component one grid cell (ALT+arrows are otherwise unbound in `component` context). Resize: drag any edge or corner (8 handles, 6 px hit zone at 1×).
5. Multi-select: CTRL+click toggles membership; a rubber band on empty canvas selects by intersection; move/resize applies to all selected; `component.select_all`. Selection is drawn as a thin accent border, focus as the thick scheme `cptFocusBorder`.
6. Maximise: double-click the caption or `component.maximise`; the component fills the canvas, other components stay instantiated but hidden; the same action or ESC restores; the maximised state is per session and not persisted.
7. Full screen (`view.fullscreen`): the main window goes full screen with docks hidden and a floating exit hint shown for 3 s; ESC or the command exits.
8. Splitters: CTRL+drag on a shared edge between two components moves that edge for every component touching it (i2 "worksheet splitters"); plain drag resizes one component.
9. Send to background (`component.send_to_background`): sets the component's `z` below every other component's (`min(z) − 1`, then the worksheet's `z` values are renormalised to `0…n−1` in the same undoable command; track components become backdrops); `component.bring_to_front` is the inverse (`max(z) + 1`). The `components[]` array is not reordered.
10. Cut/copy/paste/delete: clipboard MIME `application/x-esses-components+json` (the one type used by 04 §7.2 item 6) holding `{ "schemaVersion": 1, "components": [ComponentSpec…], "sourceDomain": "distance" }` plus `text/plain` with the same JSON; a pasted component keeps its own stored domain-related settings; delete asks no confirmation (undoable).
11. Layout lock honoured (§4.2.10). Every move/resize/add/delete/paste/reorder is a `QUndoCommand` with merge of consecutive drags of the same component (`id()` based).
12. Unknown component `type` (D-019) renders `PlaceholderComponent` with the type name, caption and an "unsupported in this version" label; it can be moved, resized, cut and pasted; its body is preserved verbatim.
13. Focus routing: TAB/SHIFT+TAB cycle components in `z` order, then reading order; a click focuses; focus is remembered per worksheet for the session; ESC from a dock returns to the remembered component.

### 4.4 `ComponentWidget` contract

The one declaration of `ComponentWidget` is in `04_RENDERING_AND_COMPONENTS.md` §7.1 (merged: rendering/data members plus the shell-facing members below, Qt string types throughout). The shell relies on these members:

| Member | Shell use |
|---|---|
| `static create(const ComponentSpec&, WorksheetContext&)` | factory by `type` string; unknown types → `PlaceholderComponent` (§4.3.12) |
| `spec()` / `applySpec(spec)` | properties dialog, undo/redo, clipboard |
| `specEditRequested(ComponentSpec, QString undoText)` (signal) | the canvas wraps every edit in one `QUndoCommand` |
| `focusRequested()` (signal) | focus routing (§4.3.13) |
| `QStringList channelNames()`, `std::optional<QString> activeChannel()` | Values window rule (§8), active-channel commands (§14.4) |
| `acceptsChannelDrop()`, `addChannel(name, newPanel)` | Channels window ENTER / CTRL+ENTER / drop (§7) |
| `contextCommands()` | Component menu and context menu (§2.2) |
| `bool exportData(ExportSink&, const ExportOptions&) const` | `component.export_data` |
| `QImage renderToImage(qreal dpr) const` | `component.print_clipboard` (current layers, never blocks; 04 §7.2 item 9) |
| `openProperties(int tab)` | F5 / CTRL+N (§12) |

`WorksheetContext` gives a component its `WorksheetViewModel&`, `DataSelectionModel&`, `MathsService&`, `RenderService&`, `Scheme&` and `CommandRegistry&`. A component never edits its own spec in place; it emits `specEditRequested` and receives `applySpec` after the command executes, so undo/redo and the properties dialog share one path.

### 4.5 `WorksheetViewModel` (QObject sketch)

One `WorkspaceViewModel` (per workspace, beside `DataSelectionModel`) owns the shared part of the view: `zoom` with its `zoomDomain` and `zoomFrac`, `cursorT`, `datumT` and datum visibility (04 §2.1 item 3). Each `WorksheetViewModel` owns only its `domain`, `offsetAxis` and size; its zoom/cursor/datum slots below forward to the `WorkspaceViewModel`, which converts the shared zoom into the active worksheet's domain (04 §3.4) and emits the change to every worksheet; `state()` composes the two into the `ViewState` copied to workers.

```cpp
class WorksheetViewModel : public QObject {
  Q_OBJECT
public:
  WorksheetViewModel(const esses::WorksheetDef&, WorkspaceViewModel&, DataSelectionModel&, DistanceService&, QObject* parent);
  const esses::ViewState& state() const;                 // Qt-free struct, copied to workers
  esses::Domain domain() const;
public slots:
  void setDomain(esses::Domain);                         // F9 rule (D-016): DistanceIndex when present, else fraction of Main range
  void toggleDomain();
  void zoomIn();  void zoomOut();                        // ×0.5 / ×2 about cursorT; clamp to >= 4 samples of the active trace
  void zoomFull();                                       // F2: whole Main outing, [outing.start−M.start, outing.end−M.start) (04 §8.6)
  void zoomToMainRange();                                // W: Main range, cursor pulled inside if outside
  void zoomToDatumMain();                                // Z: [min(datum,cursor), max(...)]
  void zoomTo(esses::XSpan);                             // rubber band, Outing graph rectangle, summary bar
  void extendZoom(double x);                             // SHIFT+click in ZoomSummaryBar: span := hull(span, x)
  void panBy(double fractionOfSpan);                     // SHIFT+LEFT/RIGHT ±0.10, F/B ±0.90, scrollbar drag; clamped to the outing
  void centreOnCursor();                                 // H; also every cursor move when autoCentre is on
  void setCursor(esses::Micros tMainLap, CursorSource);  // click, Values/Report jump, animation tick
  void stepCursor(int screenPixels);                     // LEFT/RIGHT ±1, CTRL ±10, in samples of the active trace
  void cursorHome(); void cursorEnd();
  void placeDatum(); void toggleDatum(); void swapDatum(); // X: std::swap(cursorT, datumT), emits Cursor|Datum
  void setOffsetAxis(bool on);                           // O; off zeroes every offset of the selected ranges
  void offsetRange(const esses::RangeRef&, int steps);   // CTRL+SHIFT / ALT+SHIFT arrows (±1, UP/DOWN ±10 offset steps, 04 §3.3)
  void dragOffset(const esses::RangeRef&, double delta); // sub-axis drag, continuous
  void onSelectionChanged(const esses::SelectionSnapshot&); // Selection bit; N/P keep zoom relative to range start,
                                                         // cursor/datum keep their lap distance in distance mode (04 §2.1)
  void setAnimating(bool); void setAnimationRate(double x); // A; QTimer at 30 Hz advancing cursorT by rate*dt
  void setSize(int widthPx);                             // Size bit from the canvas
signals:
  void changed(esses::ChangeMask);
  void cursorInfo(QString t, QString d, QString lap, QString dt, QString dd); // status bar
};
```

`ChangeMask` bits and the component passes they trigger are exactly D-018. Vertical zoom/pan (ALT+UP/DOWN, ALT+F2, SHIFT+UP/DOWN) are per component and per active group and live in `TimeDistanceGraph`, not in `ViewState`. Offsets are per range and per domain, applied by `DomainMapper`; amounts are shown in the Data window and persisted in the sidecar (`rangeOverrides.offsetUs/offsetM`, D-015) through an undoable command when the drag ends. Selecting a range that carries a stored offset auto-enables the offset axis on the active worksheet (D-016).

`ZoomSummaryBar` draws the Main outing extent with lap boundaries, the zoom span as a translucent rectangle and the cursor as a line; click = set cursor, drag = move span, SHIFT+click = `extendZoom`, double-click = `zoomFull`. It sits above the worksheet tab bar because it shows the one workspace-level zoom, read from the `WorkspaceViewModel` in the active worksheet's domain.

---

## 5. `DataSelectionModel`

### 5.1 State

One instance per workspace (D-017), owning `esses::SelectionSnapshot { RangeRef main; std::optional<RangeRef> overlay0; std::vector<RangeRef> overlays; bool overlaysHidden; bool singleColourMain; }` plus per-source filters. Every component, dock and report reads this snapshot; maths receives a `MathSelection` copy built from it by `MathsService` (03 §7.5, D-050), because `esses_maths` cannot see `esses_plotcore`; nothing else holds a selection.

### 5.2 Operations (exact semantics)

| Op | Key / gesture | Precondition | Effect |
|---|---|---|---|
| `setMain(r)` | ENTER on a range row, click in column **M** | `r` is a range of an open source | `main = r`. If `r` was Overlay 0 it leaves that slot; if it was Overlay *k* it is removed and overlays renumber. Exactly one Main always exists while any range exists. |
| `toggleOverlay0(r)` | CTRL+ENTER, click in column **O** | `r != main` (else no-op with a status-bar hint) | Radio: `overlay0 = r`, or cleared if `r` already was Overlay 0. If `r` was Overlay *k* it moves out of the list (renumber). |
| `toggleOverlay(r)` | ALT+ENTER, click in column **1** | `r != main` and `r != overlay0` | Appends `r` with number `overlays.size()+1`, or removes it; removal **renumbers compactly** in selection order (D-017, documented choice). |
| `toggleOverlaysHidden()` | F4 | — | Flips `overlaysHidden`; sets are unchanged; `visibleOverlays()` returns empty while hidden; variance, Δ columns and reports see no reference overlay. |
| `clearOverlays()` | SHIFT+F4 | — | `overlay0.reset(); overlays.clear(); overlaysHidden = false`. |
| `swapMainOverlay0()` | Q | `overlay0` set (else no-op) | `std::swap(main, *overlay0)`. Zoom is kept relative to range start, cursor/datum per 04 §2.1 item 1, by `WorkspaceViewModel`. |
| `nextRange()` / `prevRange()` | N / P | — | Main moves to the adjacent range in the Data window's **filtered, displayed order within the Main source**, unconditionally; if that range occupies Overlay 0 or a numbered overlay it leaves that slot exactly as in `setMain` (overlays renumber). No wrap; no-op at the ends. Zoom span is preserved relative to the range start (D-016 rule for N/P); cursor and datum keep their position in the worksheet's domain (04 §2.1 item 1). |
| `selectFastest()` | CTRL+F (Circuit) | Main source has laps | **Fastest-lap candidate rule** (used by CTRL+F, CTRL+ALT+F, the Fastest / Fastest 3 / Within 1 % filters and the Δ Fastest column): if the Main source has any trusted lap, candidates = its trusted laps of class `Flying`; otherwise candidates = its laps of class `Flying` or `Partial`, trusted or not. `Out`, `In` and `Pit` laps are never candidates (i2 release note TT 2485 prevents in/out laps from being fastest when no trusted laps exist; other untrusted laps stay eligible). Main = the candidate with minimum duration (ties: lowest number); with no candidate the command is a no-op with a Messages info row. |
| `selectAllFast()` | CTRL+ALT+F | as above | Main = fastest as above; `overlays` = every other candidate of the Main source whose duration ≤ fastest × (1 + `fastLapTolerancePct`/100), in lap order; `overlay0` untouched. `fastLapTolerancePct` default 1.0 (same threshold as the "Within 1 %" filter), per project. |
| `setSingleColourMain(b)` | J | — | Flips `singleColourMain`; Main traces use the scheme's `mainSingleColour`. |
| `setTrusted(r, b)` | Data window trust cell, Lap Editor | — | Undoable `SetRangeTrustCommand` writing `rangeOverrides` keyed `(groupPath, startMarker)` (D-014); fires `changed` with `Selection` so autoscale recomputes. |
| `closeSource(id)` | DELETE on a file row, Close File button, CTRL+A for all | — | Ranges of that source leave every slot; if Main was in it, Main = first displayed range of the next source (previous if none follows), or the fastest trusted lap when Options › General "select fastest lap on load" is on; overlays renumber; the Values/Track docks update. |
| `sourcePublished(src)` | `SourceLoader` | — | Appends the source to the Data window; if no Main exists Main = first range (or fastest per the option); otherwise selection is unchanged. |

Invariants (asserted in debug, tested in `tst_dataselection`): `main` is never in `overlays` nor equal to `overlay0`; `overlay0` is never in `overlays`; numbers are `1..n` contiguous in vector order; after `clearOverlays()` `overlaysHidden == false`.

### 5.3 Overlay numbering and colours (D-017)

1. Overlay *k* is `overlays[k-1]`; the number is shown in the Data window column **1** cell, in the graph range band and in the Data Key legend.
2. `referenceOverlay()` = `overlay0` if set, else `overlays[0]` if any, else none; variance, dual-source measurements, `Δ` columns and `variance_*` maths all use it (D-023).
3. Colour per trace: Main → the channel's palette colour (`ChannelDescriptor.colourIndex` into `channelPalette`), or `roles.mainSingleColour` when J is on; Overlay 0 → `roles.overlay0`; Overlays 1..n → channel colour transformed by `overlayStyle` (§16), default no transformation (plain channel colours, the observed i2 rule). The key colour of a *range* (used by the range band, Data Key, Track Position dots, variance traces, scatter/histogram overlay outlines) is: Main → `roles.cursorMain` (red), Overlay 0 → `roles.overlay0`, Overlay *k* → `overlayPalette[(k-1) mod 9]`.
4. Compact renumbering is a documented choice pending verification on i2 (D-017, confirm). The scheme's `overlayStyle` lets an observed difference in overlay 1..n styling be fixed as data.

### 5.4 Data window filters (per source, persisted)

Filter kinds: `All`, `Selected` (rows currently in any slot), `Fastest`, `Fastest3`, `WithinOnePercent` (duration ≤ fastest × 1.01; uses the same candidate rule as CTRL+F), `LapNumbers`. Lap-number syntax: `expr := term (',' term)* ; term := int | int '-' int`, whitespace ignored, `3-7` inclusive, descending ranges normalised, numbers compared against `Range::number`; a parse error keeps the previous filter and shows the error in the filter editor. Filters never hide the Main range, Overlay 0 or any numbered overlay (they stay visible with a filter icon). `project.json` stores `dataWindow.filters: { "<sourceId>": { "kind": "lapNumbers", "laps": "3,5-7" } , "default": { "kind": "all" } }` and `dataWindow.applyToAllSources: false`. `applyFilterToAllSources(spec)` (toolbar) copies the Main source's filter to every source and sets the flag; a later per-source change clears the flag.

### 5.5 Selection restore (D-017, D-019)

`project.json` holds `lastSelection: { main: RangeKey, overlay0?: RangeKey, overlays: [RangeKey…], overlaysHidden, singleColourMain }` with `RangeKey { source: { path, hash }, layer, groupPath, startMarker, name }`. When Options › General "load previous data with project" is on, `ProjectSession` reopens the MRU sources; after each publishes, keys are resolved by `(hash, layer, groupPath, startMarker)` then by `(path, layer, groupPath, name)`; unresolved keys are dropped with an info row; `singleColourMain` and filters are restored unconditionally. The snapshot is written on every explicit save and in every journal entry (D-019) so a crash loses at most 2 minutes of selection.

### 5.6 QObject sketch

```cpp
class DataSelectionModel : public QObject {
  Q_OBJECT
public:
  enum class Change : std::uint8_t { Main = 1, Overlay0 = 2, Overlays = 4, Hidden = 8, Colour = 16, Sources = 32, Filter = 64 };
  Q_DECLARE_FLAGS(Changes, Change)
  explicit DataSelectionModel(ProjectSession&, QObject* parent = nullptr);
  const esses::SelectionSnapshot& snapshot() const;
  std::optional<esses::RangeRef> referenceOverlay() const;
  std::vector<esses::RangeRef> visibleOverlays() const;        // empty while overlaysHidden
  int overlayNumber(const esses::RangeRef&) const;             // 0 = Overlay 0, 1..n, -1 = none
  esses::FilterSpec filter(esses::SourceId) const;
  std::vector<std::shared_ptr<const esses::DataSource>> sources() const;  // Data window order
public slots:
  void setMain(esses::RangeRef); void toggleOverlay0(esses::RangeRef); void toggleOverlay(esses::RangeRef);
  void toggleOverlaysHidden(); void clearOverlays(); void swapMainOverlay0();
  void nextRange(); void prevRange(); void selectFastest(); void selectAllFast();
  void setSingleColourMain(bool); void setTrusted(esses::RangeRef, bool);
  void setFilter(esses::SourceId, esses::FilterSpec); void applyFilterToAllSources();
  void sourcePublished(std::shared_ptr<const esses::DataSource>);   // from SourceLoader via Dispatcher
  void sourceReplaced(std::shared_ptr<const esses::DataSource>);    // sidecar edit -> new version, ranges re-resolved by id
  void closeSource(esses::SourceId); void closeAllSources();
  void restore(const esses::SelectionKeys&);                        // project load
signals:
  void changed(const esses::SelectionSnapshot&, Changes what);
  void filterChanged(esses::SourceId);
  void sourcesChanged();
};
```

Selection changes are not undoable (D-032). `sourceReplaced` re-resolves every `RangeRef` by `RangeId` so a beacon edit that rebuilds laps keeps Main/overlays on the same laps when their start marker survived, otherwise falls back to the same lap number, otherwise drops the slot with an info row.

---

## 6. Data window (`DataWindow`, F8)

Tree model `DataWindowModel` (`QAbstractItemModel` in `esses_qtmodel`):

1. Level 1: source row — `Venue · Driver · Session` (from details, profile `Details` template), tooltip = Long Comment, file name and importer id; a **stale** badge when the file changed on disk after it was opened (02 §1 behaviour 5); sources sorted by file details (date/time, then venue) with a stable manual order override by drag; a "Telemetry" group is reserved at the top (hidden in MVP).
2. Level 2: outing rows when the worksheet's `linkTo` view has more than one outing; otherwise ranges directly under the source.
3. Level 3: ranges of the active worksheet's `linkTo` view (`Laps`, `Outings`, later `Runs`, `Stages`) in start-time order.

Columns:

| Column | Content | Edit |
|---|---|---|
| **M** | red dot on Main | click = `setMain` |
| **O** | grey box on Overlay 0 | click = `toggleOverlay0` |
| **1** | overlay number *k* or empty | click = `toggleOverlay` |
| Name | range name (`Lap 7`, `Out Lap`, `In Lap`, `Partial`), italic when untrusted, filter icon when shown only because selected | — |
| Time | lap duration in the project lap-time format; `—` for Outings | — |
| Δ Fastest | duration − fastest lap of the same source under the §5.2 candidate rule (`+0.432`) | — |
| Offset | alignment offset in the current domain (`+1.25 m` / `−0.040 s`), blank when zero | — |
| Trusted | checkbox | toggles `setTrusted` (undoable) |
| Local maths | icon when the source's sidecar carries `localMaths` | — |

Keyboard inside the dock: UP/DOWN move the highlight; ENTER / CTRL+ENTER / ALT+ENTER as in §5.2; DELETE on a source row = `closeSource` with no prompt (sidecars write through after every committed sidecar command, coalesced within 500 ms, and any pending write is flushed on close, 02 §5.3.4); DELETE on a range row = no-op; ESC = return focus. The highlight follows Main when Main changes by N/P/CTRL+F (Main is "forced into view").

Context menu: Set Main / Toggle Overlay 0 / Toggle Overlay · Filters ▸ (All, Selected, Fastest, Fastest 3, Within 1 % of Fastest, Lap Numbers…) · Apply Filter To All Sources · Trusted · Clear Offsets For This Source · Lap Editor… · Details… · Reload File (`file.reload_source`, enabled on a stale source) · Close File · Reveal In File Manager.

**Changed files.** `MappedFile` keeps the bytes it mapped (02 §1 behaviour 5); `SourceLoader` calls `MappedFile::changedOnDisk()` for every open source whenever the application becomes active (`QGuiApplication::applicationStateChanged` → `Qt::ApplicationActive`), and the chunk providers check it before each decode batch. A changed file marks its source stale: no further chunk is decoded from it, the Data window shows the badge, and a Messages row (`Import`, Warning, "file changed on disk") carries a **Reload** action. Reload closes the source and reopens it through the open pipeline (§18 step 5); the sidecar is re-applied and the selection is restored by `RangeId` where the ranges still exist (§5.6 `sourceReplaced`).

---

## 7. Channels window (`ChannelsWindow`, C)

1. Tree model `ChannelsModel`: level 1 = source (one node per open source, plus a "Project" node for aliases/mappings/constants that are source-independent), level 2 = category (`Logged`, `Maths`, `Details`, `Alias`, `Mapped`, `Constants`), level 3 = the source's channel groups as delivered by the importer dictionary (flat when none), level 4 = channels (`name (unit)`, italic when `Status::Pending`, red when `Error`). Expand/collapse state is keyed by `(category, group name)` and persists across source changes and sessions (`layout.json` `docks.channels.expanded`).
2. Sort: logical case-insensitive (`QCollator` numeric mode) within a group (UI rule 14); names > 32 characters and enumerated channels are first-class.
3. Search box (focused by `view.channels_window`): i2 semantics — the query is split on whitespace into words; a channel matches when every query word is a case-insensitive **prefix of some word** of the channel name, where name words are split on whitespace, `_`, `-`, `/`, `(`, `)` and `[`, `]` (so bracketed text is searchable, e.g. `W S FL` → `Wheel Speed FL`, `temp (ecu)` → `Water Temp (ECU)`). Each query word must match a distinct name word in order. Options › General "loose name match" switches each word to a substring match. The tree filters live on every keystroke (`QSortFilterProxyModel` with a custom `filterAcceptsRow`); the first match is highlighted.
4. Keyboard: TAB from the search box moves into the list on the first match; UP/DOWN move; ENTER = `addChannel(name, newPanel=false)` on the focused component; CTRL+ENTER = `newPanel=true`; double-click / CTRL+double-click likewise; ESC returns focus to the worksheet (the search text is kept). If the focused component does not accept channels (`acceptsChannelDrop() == false`), ENTER shows a status-bar hint.
5. Drag and drop: MIME `application/x-esses-channels` = JSON `{ "channels": ["Corr Speed", "Throttle Pos"], "newPanel": false }` (CTRL held at drop → `newPanel: true`); components that accept drops show a drop indicator (insertion line in the key column for graphs, highlighted border otherwise).
6. Context menu on a channel: Add To Focused Component, Add As New Panel, Properties… (channel override editor, same page as the Channel Editor row), Edit Maths… (maths channels), Status…, Copy Name (`'Name' [unit]` ready for the Maths editor).
7. Live binding: the tree updates on `sourcesChanged`, `MathsService::statusChanged` and override edits without losing the expand state or the search text; updates are coalesced to ≤ 10 Hz so a busy maths queue never stalls the editor (the i2 1.1.4 "editor slowed by channels window" fix).

---

## 8. Values window (`ValuesWindow`, V)

Rule (parity-first §5.10): rows are the channels of the **focused component** (`ComponentWidget::channelNames()` in its display order); the header has an "All channels" toggle that lists every channel of the Main source instead (grouped as the Channels window). The choice persists per user (`layout.json` `docks.values.allChannels`). When no component is focused the last focused component's list stays.

Columns: Name · Main · Overlay 0 · Δ (Main − reference overlay) · Unit (inline-units option; when off the unit is appended to the value cell). With a datum on, the active channel's row expands into a measurement block: value@main, value@datum, Δ, min/max/avg between cursors, Δt, Δd.

Values come from `Resampler::valueAt(series, cursorT, interp)` for Main and through each overlay's `DomainMapper` for overlays; the display unit and DPS are the channel override's. Pending maths rows show `…` and queue `MathsService::require(key, visibleSampleRange, Priority::Background)` while scrolled into view (D-022); errored rows show the status text in red. Refresh on the `Cursor` bit is budgeted at ≤ 2 ms for 250 rows (outline §8): only visible rows are recomputed, text is cached per `(value, dps, unit)`.

Context menu per row: Properties… (override), Edit Maths…, Status…, Copy Value, Copy Row (`name<TAB>value<TAB>unit`), Add To Focused Component. Double-click a row with a stored min/max measurement moves the cursor there.

The `values_list` component embeds `ValuesModel` with its own channel list and `datum`/`delta` column options; `gauge_numeric`/`gauge_bar` read the same model one row at a time.

---

## 9. Track Map dock (`TrackMapWindow`)

Shows the Main source's track map (`trackmaps/<venue>.track.json`, auto-generated on open when absent and the required channels exist — D-027). Content is the `TrackPosition` widget (`track_position` component class) bound to the active worksheet's `ViewState`: Main dot in `roles.cursorMain`, overlay dots in key colours (independent positions only in time mode), zoom span highlighted, optional section labels. PLUS/MINUS/wheel zoom when focused, drag pans, double-click fits. Context menu: Fit, Show Sections, Show Section Labels, Track Editor…, Regenerate Track…. The transparent-window mode of i2 is not implemented (deviation §21). Default state `AutoHide` on the right.

---

## 10. Channel Status (`ChannelStatusWindow`, CTRL+U)

`ChannelStatusModel` lists every maths item, alias, mapping and built-in module output across open sources with: Source · Channel · Kind (`Expression`, `Filter`, `Alias`, `Mapping`, `Built-in`, `Constant`) · Status (`OK` / `Pending` / `Error: <text>` / `Warning: <text>` for dimension warnings, D-021) · Resolved to · Rate · Unit. The "Resolved to" cell renders the `ResolutionTrace` of D-013 as one line (`'Wheel Speed' → alias → 'Wheel Speed FL' (km/h)`; `'ECU Speed' → mapping → 'Vehicle Speed' [unit stamped km/h]`; `'Corr Dist' → built-in esses.corrected_speed_distance v1`), with the full step list in the tooltip and in a detail pane below the table. Double-click opens the Maths Editor at the item, the Alias editor or the Mapping panel respectively. Filter box, "errors only" toggle, Copy Table (TSV). Updates arrive through `MathsService::statusChanged(key, status)` coalesced to 10 Hz. `channel.status` (CTRL+SHIFT+S) focuses the dock on the active channel's row.

---

## 11. Messages / Import Log (`MessagesWindow`)

`MessagesModel` is an append-only list of `Message { timestamp, severity {Info, Warning, Error}, origin {Import, Maths, Budget, Project, System}, source (file or document), text, action? }` — the origin set, the 5,000-row session cap (oldest dropped, not persisted) and the no-modal-diagnostics rule are those of `01_ARCHITECTURE.md` §8.3 / P-7. Mapping of shell diagnostics to origins: chunk-cache and layer-memory warnings → `Budget`; layout restore failures, migrations, journal recovery, selection-restore drops, unit-override drops → `Project`; scheme and keymap validation problems, file-association and log-sink rows → `System`. The dock ships in M1 because the Import Log and budget warnings exist from M1 (XREF 07 §2.3). Rows: the `ImportLog` of every open (one group per file, collapsible: heuristics used, companions found, sentinels, unknown units, dictionary hits), `MappingPanel` outcomes, maths diagnostics not tied to an item, chunk-cache budget warnings (D-011), layout restore failures (§3.4), scheme/keymap validation problems, project migrations, selection-restore drops. Severity filter buttons, text filter, Clear, Copy, Save As… (`.log` text). An `action` is a command id with arguments rendered as a link (e.g. "Open Mapping panel", "Show Channel Status"). An Error row flashes the status-bar message area; Warnings do not. The dock auto-shows on the first Error of a session unless Options › General "do not auto-show Messages" is set.

---

## 12. Properties dialogs (`PropertiesDialog`, F5 / CTRL+N)

D-027: one **modeless tabbed `QDialog` per component instance** (not a dock), opened by `component.properties` (F5) or `channel.properties` (CTRL+N, opens on the Channels tab with the active channel selected). Requirements:

1. Buttons OK · Cancel · Apply; Apply/OK emit one `specEditRequested(newSpec, "Edit <type> properties")` → one `QUndoCommand`; Cancel restores the spec at open time (also one command if Apply was pressed before). Each Apply is separately undoable (CTRL+Z in the main window).
2. The dialog tracks its component: deleting the component closes the dialog; undo/redo while open refreshes the pages; switching worksheets keeps it open; at most one dialog per component (F5 again raises it).
3. Tabs by type (hand-built, i2 vocabulary): `time_distance`/`outing`: Channels (groups tree: add/remove/reorder/rename, scale mode and min/max with unit, scale lines, Lo/Hi edges, per-trace visible/unit/DPS) · Display (caption auto/text, trace style/width, grid, markers, bands + band group, overlap mode, key mode/values/stats, cursor style, variance show/height/autoscale, status strip, device events, links zoom/cursor/datum) · Data (exclude untrusted) · Colour Bands. `scatter`: Channels (X, Y list with delay, Z colour) · Display (style, size, lines, trend type/order/through-zero/digits, interval average, tile mode) · Colour Channel (`BandAxis`) · Data (gating, exclude untrusted, density threshold). `histogram`: Channels · Display · Colour Channel · Data (bins `BandAxis`, Y mode, exclude untrusted, gating). `channel_report`/`time_report`: Channels/Sections · Report (scope, heading hierarchy, stats, probe seconds, time format, layout, highlight, Δ vs Overlay 0; row group, include untrusted laps and bands for the time report) · Display. `track_position`: Display. `values_list`: Channels · Display (datum, delta, heading, autosize). `gauge_*`: Channels (one) · Display · Colour Bands.
4. Validation is inline (red field + reason); OK/Apply are disabled while any page is invalid.
5. Geometry persists per component type in `layout.json` `dialogs`.
6. Schema-generated property grids exist only for plugin components (post-v1.0, D-027); MVP tabs are hand-laid-out pages (i2 vocabulary) whose fields bind through one generic `FieldBinder`: a binding row is { JSON pointer into the body, editor kind (bool, enum, number with `{v, unit}`, colour, channel picker, `BandAxis`, `ColourBands`), label }, and the editors, validation and undo wiring are shared by every type. A page is therefore a layout plus a binding table, not bespoke code; only the `time_distance`/`outing` groups tree (and the scatter Y-trace list) are custom widgets. For 0.1 every field a page shows is a field of its type's body schema (04 §8.14–§17); an item-3 field or tab with no body field yet is omitted in 0.1, not greyed.
7. Acceptance is schema-driven (M3, test `properties_roundtrip`): for each component type, every property of its body schema (`schemas/components/<type>-<version>.schema.json`) must be reachable through a binding row (an unbound property fails the test), each one is mutated through its bound editor, Apply, save, reopen → identical body. This replaces a hand-listed "every field on every tab" (XREF 07 AC-M3-08).

---

## 13. Editors — requirement lists

All editors are modeless unless stated, remember geometry (§3.4), apply edits through `QUndoCommand`s, and refuse to close while a field is invalid (showing the reason). Numbers are edited in the channel's display unit and stored as `{v, unit}` (D-010). Scope for 0.1 (D-035 sizing): an MVP editor is a table, an edit and undo; sub-features marked *(v1.0)* below are specified for later and are not built in 0.1. Scheme and keymap customisation in 0.1 is editing the JSON files (validated by `esses-cli validate` and on load) plus the scheme selector.

### 13.1 Channel Editor (CTRL+E)
1. Table of every channel of every open source: Source · Name · Logged name · Category · Rate (`Rate::hz()`) · Type (`Dtype`/`SampleKind`) · Unit (combo of the quantity's units; transient units excluded) · Min · Max (in the display unit) · DPS · Colour (palette index swatch) · Interp (`Linear/Closest/Previous/Next`) · Override (dot when `channels/overrides.json` has a row).
2. Sortable by every column including rate; filter box with the Channels-window search semantics.
3. *(v1.0)* Multi-select edit: changing a cell with several rows selected applies to all (unit only when the quantities agree; otherwise the row is skipped and listed).
4. Writes `channels/overrides.json` (`overrides-1`), which stays a map keyed by channel name (02 §6.3); the editor presents that map as rows `{ channel, displayUnit, dps, colourIndex, scaleMin{v,unit}, scaleMax{v,unit}, interp }` and converts on load and save; "Clear override" per row and per selection.
5. `Corr Dist` unit is editable here and changes the distance axis units and the `d` status-bar field.
6. Unit overrides must apply to every open file carrying that channel (i2 rule 13), on both paths: **at edit time** an override that cannot apply to some open file is refused with the list of conflicting files; **at source open**, if an existing override cannot apply to the newly opened file, the override is removed as in i2 (an undoable `ClearOverrideCommand` so the user can restore it after closing that file) and a Messages row (`Project`, Warning) names the channel, the unit and the file.
7. Aliases… and Mappings… buttons open the two small editors: Aliases = name + ordered candidate list with per-item enable, circular references rejected; Mappings = logged name → analysis name + optional unit stamp with `unitlessOnly` (D-013).

### 13.2 Maths Editor (CTRL+M)
1. Left pane: global modules (`maths/*.maths.json`, project order, drag to reorder, enable checkbox, per-module condition editor `'Vehicle Id' == "GT3-07"`), then "Local maths" with one node per open source (sidecar `localMaths`).
2. Centre: item list of the selected module (enable checkbox, kind icon, name, unit, status glyph from `MathsService`); multi-select cut/copy/paste/delete/enable; paste preserves names; item comments.
3. Right: item page by kind — Expression (name, quantity → display unit/result unit, rate explicit Hz or Auto, DPS, colour, interp, enumeration, comment, expression text), Constant (name, value `{v,unit}`), Filter, Scale & Offset, 2D Table, 3D Table (v1.0), Built-in (schema'd settings page), Plugin (v1.0), Regression (v1.0).
4. Expression editor: monospace `QPlainTextEdit`; validity line updated on every keystroke from `Parser::parse` + `TypeChecker::check` (column spans underlined; dimension mismatches amber warnings, D-021); unit popup while typing inside `[…]`; Insert Channel (`'Name' [unit]`), Insert Range (double-quoted group path); hover text for the `^`/unary-minus rule (D-020). *(v1.0)* auto-complete popup (CTRL+SPACE, automatic after `'`), Insert Function with signature help.
5. Editing without any source open is allowed (unknown channels are warnings).
6. Modules save on editor close and on every Apply (D-019); an item with a syntax error cannot be left by closing the editor (i2 rule) — the editor refuses with the diagnostic; CTRL+Z inside the text field is text undo, module edits are project undo.
7. Import/Export module (`*.maths.json` copy; i2 XML import is v1.0).
8. Double-click from Channel Status / Channels window / CTRL+SHIFT+M opens the editor at the item; the item page shows the current status text.

### 13.3 Lap Editor
1. Table of the Main source's laps (or the source picked in a combo): Number · Class · Start · Duration · Trusted (checkbox) · Stretch (%, from `LapStretch` report, blank when outside tolerance) · Provenance of the start marker (`Logged`/`Companion`/`User`/`Edited`). Alignment offsets (O axis) are not shown here; they live in the Data window Offset column.
2. Insert beacon: by absolute time, or by lap time after a chosen lap, or at the current cursor (SHIFT+F7 equivalent); result = `Marker{prov=User}` in the sidecar; laps rebuild synchronously (D-014).
3. **Beacon offset (distance)**, per source, with Apply: a signed distance in the `Corr Dist` unit (positive = the beacon was further along the track than usual) that **moves the beacon**, so lap boundaries, lap times, sections, stretch and time reports change and the laps line up with earlier files (i2 Laps help). It is stored in the sidecar as a distance (a `MarkerEdit` op `offsetDistance { dM }` applied to every `BCN` of the source; XREF 02 §4.3) and resolved to a time per beacon, `t' = first time Corr Dist reaches CorrDist(t_logged) + dM`, by a worker reading the `Corr Dist` column; the result is posted to the GUI, where the ranges rebuild synchronously (D-014). Until it lands (or while `Corr Dist` is Pending) the table shows "pending" and the old laps stay; a change of the corrected-distance settings re-resolves it. One undoable command per Apply. This is unrelated to the O-axis alignment offset (`OffsetRangeCommand`, display-only, per lap), which never moves a lap boundary. Per-beacon time offsets remain available as `MarkerEdit{offset}` on a selected beacon (drag in the table or numeric entry).
4. Trusted flag editing (same command as the Data window).
5. *(v1.0)* Auto-insert by lap distance (enter track length; beacons placed every `length` metres of `Corr Dist` from the first beacon; preview before Apply) and by GPS gate (lat/lon prefilled from the cursor position; gate width in metres; preview; latitude/longitude are read through `MathsService` as the `esses.gps_corrections` chain head, so (0, 0) fixes are invalid, 03 §9.2).
6. Restore logged laps: deletes every `MarkerEdit` and user marker of the source (one command).
7. Ignored beacons (within the device ignore time) are listed greyed; device reset laps are shown as `Partial`.
8. Terminology follows the profile (`Lap`/`Run`/`Stage`, D-036).

### 13.4 Details editor (CTRL+D)
1. Grid: one column per open source, one row per detail (standard set from the profile's details definition plus custom details); double-click edits; typed cells (text / number with unit / date-time); empty strings allowed; numbers are not rounded on reopen.
2. Edits are `SetDetailCommand`s into the sidecar with provenance `User` and the original value kept (D-015); edited cells show a dot; "Restore original" per cell.
3. *(v1.0)* Add / remove custom details (per source). Renaming the `Venue` detail prompts to rename/regenerate the track map (0.1).
4. Column sort; Copy as TSV. *(v1.0)* column reorder by drag.

### 13.5 Track Editor
1. Source lap picker (Main by default); generation method radio: Speed + Lateral G (needs `Corr Speed` and a lateral-G channel) or GPS (needs lat/lon ≥ 6 dp, which `TrackGenerator` reads through `MathsService` as the `esses.gps_corrections` chain head, never the raw logged columns, so (0, 0) fixes are invalid, 03 §9.2; optional inner/outer lap pickers; smoothing option).
2. Generate → preview map with closure error / RMS fit shown; Accept writes `trackmaps/<venue>.track.json` (undoable; backup of the previous file in `backup/`).
3. Default sections (Turns/Straights, ≥ 10 m) generated at first acceptance; "Regenerate default sections" button.
4. Sections table: name, start, end (in `Corr Dist` units), category (`corner`/`straight`), speed class, user-defined flag; add from cursor span, edit numerically, delete; ≥ 10 m enforced; overlapping user sections allowed within a named section set.
5. *(v1.0)* Rotation, direction (CW/CCW) and "closed" flags editable (0.1 keeps the generated values).
6. Rename / delete track files; "Auto-generate on open" checkbox (project option).

### 13.6 Corrected Speed and Distance dialog
1. Method radio: Fastest Wheel (pick the two wheel channels), Alternate Channel List (ordered list, first existing wins), G Correction (speed + longitudinal-G channel).
2. Clamping: min/max in the speed display unit with "invalidate instead of clamp" checkboxes.
3. Distance unit follows the speed unit; override hint pointing to the Channel Editor.
4. Lap distance stretching: enable, tolerance % (default 5, per project), per-lap stretch report (read-only table shared with the Lap Editor).
5. Apply re-registers the built-in module `esses.corrected_speed_distance` with the new settings for every source and invalidates its cone (D-016, D-022); the dialog shows the module status line and the note that results are not bit-identical to other tools.
6. Settings persist in `project.json` under `builtinMaths["esses.corrected_speed_distance"]` as `{version, enabled, settings}` (03 §9.1, 02 §6.2), validated by `schemas/builtin-esses.corrected_speed_distance-1.schema.json`; per-source overrides go to the sidecar as a `builtin` item.

### 13.7 Options (Tools › Options)
Tabs and settings (persisted in `project.json` `options` unless marked *user*):

| Tab | Settings |
|---|---|
| General | Save project on exit (ask when off) · Autosave interval (minutes, default 2, 0 = off; journal, D-019) · Load previous data with project · Select fastest lap on load · Loose channel-name search · Auto-show Messages on error · Reduce CPU during animation · Language (*user*; MVP ships English only) |
| Analysis | Include untrusted data in autoscale · Include overlay data in autoscale · Time format (axis/lap times: `m:ss.fff`, `ss.fff`, `h:mm:ss.f`) · Lap-time display unit · Fast-lap tolerance % (§5.2) · Maths precision, persisted as `options.maths.precision` = `single` or `double` (the 03 §4.1 vocabulary used by `project-1` and `maths-1`; the UI may label the choices F32/F64; D-022) · Maths compat: unary minus above power (D-020) |
| Colours | Scheme selector (shipped + user schemes), Duplicate (writes an editable copy to the user scheme folder and reveals it), Import/Export (`*.scheme.json`), Show scheme selector on toolbar, Follow OS light/dark (*user*); *(v1.0)* Edit… (scheme editor, §13.9) |
| Fonts | Read-only list of the active scheme's font roles (§16); fonts are changed by editing the scheme file in 0.1; *(v1.0)* family/size/weight/italic editors |
| Keys | Read-only keymap table: command, context, current keys, default keys, status; Import/Export/Reveal `keymap.json` (*user*); *(v1.0)* record a new key, Reset row / Reset all |

### 13.8 Mapping panel (`MappingPanel`, D-028/D-029)
1. Appears as a page inside the Open flow (a bar above the worksheet area with the panel below, not a modal) when an importer reports unknown unit strings or channel names that no `UnitMappingProfile`/dictionary resolves.
2. Table: Vendor name · Vendor unit · Sample values · Proposed analysis name · Quantity · Unit · Rule (`nameOnly`/`unitOnly`/`nameAndUnit`) · Apply to future files (checkbox).
3. Buttons Scan (re-run resolution), Resolve (accept proposals), Save (writes `channels/unitmaps/<importer>.json`), Open Anyway (unknowns stay unitless), Cancel (abort the open).
4. The open completes only after Resolve/Open Anyway: the open pipeline's task A has ended with a `PendingResolution` (05 §2 step 4), and the panel's decision submits task B (`ImporterRegistry::resume`) with the `MappingDecision` by value (01 §4.1); Cancel drops the pending open. No worker waits for the panel. The outcome is logged to Messages.

### 13.9 Scheme editor (Options › Colours › Edit…) *(v1.0)*
In 0.1 schemes are edited as JSON (Options › Colours › Duplicate, then edit the file; it is validated on load, §16 rule 8). The editor below is v1.0.
1. Role list grouped (Window, Component, Data tables, Cursors/marks, Status) with colour swatches; double-click edits (`QColorDialog`); channel palette list (add/remove/reorder, minimum 10); overlay palette (9); `overlayStyle` editor; print variant toggle showing the overridden roles.
2. Live preview on the active worksheet (every edit emits `schemeChanged` → `Scheme` bit); OK saves to `~/.config/esses/schemes/<name>.scheme.json`; Cancel reverts; Save As creates a copy. Shipped schemes are read-only (Duplicate first).
3. Double-clicking the Data Key legend in a graph opens this editor at the relevant role.
4. Contrast check: roles pairs (foreground/background, grid/background, each palette colour/background) below 3:1 are flagged in the editor and in Messages on load.

---

## 14. `CommandRegistry`, `KeymapBinder` and `keymap.json`

### 14.1 Registry

```cpp
enum class CommandContext : std::uint8_t { Global, Worksheet, Component, Graph, Summary, Track, DataWindow, ChannelsWindow, ValuesWindow };
struct CommandSpec {
  QString id;                 // "zoom.in"
  QString title;              // "Zoom In"
  QString menuPath;           // "Component/Zoom"
  CommandContext context;
  QList<QKeySequence> defaultKeys;
  bool checkable = false;
  QString iconName;           // theme icon or empty
  std::function<bool()> enabled;   // evaluated on menu show / palette open
  std::function<bool()> checked;
  std::function<void(const QVariantList& args)> invoke;
};
class CommandRegistry : public QObject {
  Q_OBJECT
public:
  void add(CommandSpec);
  bool invoke(const QString& id, const QVariantList& args = {});
  const CommandSpec* find(const QString& id) const;
  QList<const CommandSpec*> all() const;
  QAction* action(const QString& id);               // one shared QAction per id (menus, toolbars, context menus)
signals:
  void invoked(const QString& id);                  // telemetry for tests; later scripting hook
};
```

Commands are registered by the owning layer at start-up: `esses_app` (file/view/layout), `esses_qtmodel` facades (data, tools), components (component/graph/zoom/pan/cursor/datum/offset/track), docks (data window/channels window/values window). A command with no handler in the current context is `enabled() == false`.

### 14.2 `KeymapBinder` — context resolution

An application-wide `QObject::eventFilter` on `QApplication` for `QEvent::ShortcutOverride`/`KeyPress`. On each key:

1. Build the key sequence (`QKeySequence::PortableText`, with multi-key chords unsupported in MVP).
2. Determine the focus chain: modal dialog active → no keymap handling (Qt dialog navigation only). Else focus inside a dock → `[dock-specific context, Global]`. Else focus inside the worksheet → `[type context of the focused component (Graph | Summary | Track), Component, Worksheet, Global]`.
3. For dock contexts, an unmodified letter/digit/arrow/ENTER/SPACE/HOME/END/PAGE key is first offered to the focused widget; it reaches the binder only if unconsumed. Function keys and chords with CTRL/ALT go to the binder first (so CTRL+F works inside the Data window while CTRL+ENTER stays with the tree).
4. For worksheet contexts, keys go to the binder first; a text-entry widget inside a component (rare: inline rename) is excluded by `QWidget::property("esses.textEntry")`.
5. The first context in the chain with a binding for the sequence wins; `invoke` is called; the event is accepted.
6. Numeric edit controls in modeless editors: unmodified keys stay with the control; function keys and chords go to the binder (the i2 release-note fix).
7. Conflicts: two bindings with the same sequence in the same context are rejected at load (Messages, keymap row disabled); the same sequence in different contexts is allowed (DELETE: `component.delete` vs `data.close_file`).

### 14.3 `keymap.json` (schema `keymap-1`)

Shipped default `data/keymaps/default.json`; user file `~/.config/esses/keymap.json` with `inherits: "default"`. Worksheet-specific key bindings (i2 `QuickSelectKeys`) are a post-MVP extension slot: `WorksheetDef.keys[]` with the same row shape overrides for that worksheet only. This section is the canonical `keymap-1` shape (02 §6.9 and 08 §6.5 refer here; XREF).

```json
{
  "$schema": "urn:esses:keymap-1",
  "schemaVersion": 1,
  "name": "default",
  "inherits": null,
  "bindings": [
    { "command": "view.data_window",      "keys": ["F8"],              "context": "global",      "status": "implemented", "milestone": "M1" },
    { "command": "data.toggle_domain",    "keys": ["F9"],              "context": "global",      "status": "implemented", "milestone": "M1",
      "doc": "Toggle the active worksheet between time and distance with zoom preserved" },
    { "command": "zoom.in",               "keys": ["Up"],              "context": "graph",       "status": "implemented", "milestone": "M1" },
    { "command": "zoom.in",               "keys": ["Up"],              "context": "summary",     "status": "implemented", "milestone": "M3" },
    { "command": "component.colour_band", "keys": ["Alt+0"], "args": [0], "context": "component", "status": "implemented", "milestone": "M3" },
    { "command": "data.select_main",      "keys": ["Return", "Enter"], "context": "data_window", "status": "implemented", "milestone": "M1" },
    { "command": "file.print",            "keys": ["Ctrl+P"],          "context": "global",      "status": "planned",     "milestone": "v1.0" },
    { "command": "file.unload_device",    "keys": ["Ctrl+F8"],         "context": "global",      "status": "reserved" }
  ],
  "unbind": []
}
```

A user file has the same shape with `"inherits": "default"`; its `unbind` rows (`{ "command": "file.print", "context": "global" }`) remove inherited bindings.

Row fields: `command` (snake_case id `<area>.<verb_noun>`, exactly as in §14.4; required), `keys` (required, may be empty for menu-only commands), `context` (required; the snake_case form of `CommandContext`, §14.1: `global`, `worksheet`, `component`, `graph`, `summary`, `track`, `data_window`, `channels_window`, `values_window`), `args` (optional), and three fields that the shipped default carries on every row and user files may omit (ignored if present): `status` ∈ `implemented | planned | reserved`, `milestone` ∈ `M1 … M4 | v1.0 | later` (required unless `reserved`), `doc` (optional one-line behaviour text for the palette tooltip and the generated docs). The §14.4 MVP column maps onto them: "yes" rows are `implemented` with the milestone of the batch that lands them (§22), "v1.0" rows `planned`/`v1.0`, "—" rows `reserved`.

Rules: `keys` are Qt portable-text sequences (`Ctrl+Shift+C`, `Alt+F2`, `Shift+F7`, `PgUp`, `Space`, `Return`); macOS maps `Ctrl` to Command and `Meta` to Control automatically via `QKeySequence`; a user file may only add, replace or `unbind` rows; `esses-cli validate` checks the schema, the command ids and the context names against the registry export (`esses --dump-commands`).

### 14.4 Complete default keymap (i2 hotkey table, D-033: one Qt Test per row)

Context abbreviations: **G** global, **W** worksheet, **C** component, **GR** graph (`time_distance`, `outing`), **S** summary (`scatter`, `histogram`; v1.0 `spectrum`, `mixture_map`, `suspension_histogram`), **T** track (`track_position`; v1.0 track report/rainbow/GPS), **DW** Data window, **CW** Channels window. "MVP" marks rows that ship in 0.1; others are registered, bound and greyed with a tooltip.

**Views and windows**

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `view.data_window` | F8 | G | Show and focus the Data dock (range tree). | yes |
| `view.channels_window` | C | W | Show and focus the Channels dock with the caret in the search box. | yes |
| `view.values_window` | V | W | Toggle the Values dock visibility; focus unchanged. | yes |
| `view.return_focus` | Esc | G | Dock focused → focus the remembered component; modeless dialog → close if clean; modal → cancel; maximised component → restore; full screen → exit. | yes |
| `view.select_worksheet` | F7 | G | Open `SelectWorksheetDialog` (workbook + worksheet). | yes |
| `layout.editor` | Ctrl+F7 | G | Open the Layout Editor. | yes |
| `view.worksheet_n` | 1…9, 0 | W | Select worksheet tab *n* (0 = tenth); no-op if absent. | yes |
| `view.workbook_n` | Ctrl+1…9, Ctrl+0 | G | Select workbook *n* (0 = tenth), restoring its perspective. | yes |
| `view.prev_worksheet` / `view.next_worksheet` | F11 / F12 | G | Previous / next worksheet tab, wrapping. | yes |
| `view.prev_workbook` / `view.next_workbook` | Ctrl+F11 / Ctrl+F12 | G | Previous / next workbook, wrapping. | yes |
| `view.channel_status` (alias of `tools.channel_status`) | Ctrl+U | G | Show and focus the Channel Status dock. | yes |
| `view.messages_window` | — | G | Toggle the Messages dock. | yes |
| `view.track_map_window` | — | G | Toggle the Track Map dock. | yes |
| `view.command_palette` | Ctrl+Shift+P | G | Open the command palette (§15). | yes |
| `view.reset_docks` | — | G | Reset dock windows (§3.5). | yes |
| `view.fullscreen` | — | G | Toggle full screen (§4.3.7). | yes |
| Dialog navigation (ALT+letter, TAB, SHIFT+TAB, ENTER, arrows, SPACE, CTRL+TAB, SHIFT+CTRL+TAB) | — | dialog | Qt standard; not in the keymap. | — |

**File and general**

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `file.open_log` | Ctrl+O | G | Open Log File dialog (`QFileDialog` multi-select + Details side pane probing the selected file, D-019); typed paths allowed; the last folder is remembered per user. | yes |
| `file.close_all` | Ctrl+A | G | Close every source; no prompt (sidecars write through and pending writes are flushed on close, 02 §5.3.4). | yes |
| `file.reload_source` | — | DW | Reload the highlighted stale source through the open pipeline (§6, Changed files); disabled unless the source is stale. Also the action of the Messages row. | yes |
| `file.save_project` | Ctrl+S | G | Atomic save of every dirty document + `backup/` copy + journal clear (D-019). | yes |
| `file.print` | Ctrl+P | G | Print worksheet (full page extent). Greyed in MVP; v1.0. | v1.0 |
| `help.contents` | F1 | G | Open the user documentation (local HTML or URL). | yes |
| `file.unload_device` | Ctrl+F8 | G | Reserved (vendor device unload); registered, never bound to an action; shows "not available" tooltip. | — |
| `file.import_usb` | Ctrl+I | G | Reserved as above. | — |
| `edit.undo` / `edit.redo` | Ctrl+Z / Ctrl+Y | G | `QUndoStack` undo/redo (D-032); text fields keep their own undo while focused. No Ctrl+Shift+Z redo alias: that chord is `channel.zero_at_cursor` in component context, which would win while a component has focus and turn "redo" into a sidecar write. | yes |
| `file.new_project`, `file.open_project`, `file.close_project`, `file.import_data`, `file.export_data` | — | G | Menu-only in the default keymap. | yes |

**Edit (components)**

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `component.cut` | Ctrl+X | W | Copy the selected components to the clipboard (§4.3.10) and delete them (one command). | yes |
| `component.copy` | Ctrl+C | W | Copy selected components. | yes |
| `component.paste` | Ctrl+V | W | Paste at stored rectangles with per-paste offset; pasted components keep their domain-related settings and manual scales. | yes |
| `component.delete` | Delete | W | Delete selected components (undoable, no confirmation). | yes |
| `component.select_all` | — | W | Select every component on the worksheet. | yes |

**Data**

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `data.toggle_domain` | F9 | G | Toggle the active worksheet between time and distance with zoom preserved (D-016); works from inside any dock (§3.3.1); the toolbar button reflects it. | yes |
| `data.overlays_toggle` | F4 | G | Hide/show overlays without deselecting (§5.2). | yes |
| `data.overlays_clear` | Shift+F4 | G | Clear Overlay 0 and all overlays. | yes |
| `data.single_colour_main` | J | W | Toggle single-colour Main. | yes |
| `data.next_range` / `data.prev_range` | N / P | W | Move Main to the adjacent displayed range (vacating its overlay slot if it had one), zoom preserved. | yes |
| `data.select_fastest` | Ctrl+F | G | Fastest lap of the Main source under the §5.2 candidate rule becomes Main (trusted flying laps if any, else flying/partial; never in/out/pit). | yes |
| `data.select_all_fast` | Ctrl+Alt+F | G | Fastest becomes Main; all other candidates within the fast-lap tolerance become overlays. | yes |
| `data.swap_main_overlay0` | Q | W | Swap Main and Overlay 0. | yes |
| `data.animate` | A | W | Start/stop cursor animation on the active worksheet (rate from Data › Animation Rate). | yes |
| `data.lap_stretch_toggle` | — | G | Toggle lap-distance stretching (D-016); toolbar. | yes |
| `data.filter_all_sources` | — | G | Apply the Main source's Data-window **lap filter** (§5.4) to every source (i2 Data-window toolbar button). It does not touch transient maths filters (`channel.filter`), which apply to one source (XREF 03 §4.2). | yes |
| `data.select_main` | Return | DW | Highlighted range becomes Main. | yes |
| `data.toggle_overlay0` | Ctrl+Return | DW | Toggle Overlay 0 on the highlighted range. | yes |
| `data.toggle_overlay` | Alt+Return | DW | Toggle the highlighted range as a numbered overlay. | yes |
| `data.close_file` | Delete | DW | Close the highlighted source. | yes |

**Tools**

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `tools.details` | Ctrl+D | G | Open the Details editor. | yes |
| `tools.channel_editor` | Ctrl+E | G | Open the Channel Editor. | yes |
| `tools.channel_status` | Ctrl+U | G | Show and focus Channel Status. | yes |
| `tools.maths_editor` | Ctrl+M | G | Open the Maths Editor (at the last item). | yes |
| `tools.setup_sheets` | Ctrl+T | G | Reserved; greyed with tooltip (v1.0+). | — |
| `tools.lap_editor`, `tools.track_editor`, `tools.corrected_speed`, `tools.aliases`, `tools.mappings`, `tools.units_list`, `tools.options` | — | G | Menu-only by default. | yes |

**Active channel** (target = the focused component's active channel, marked with the square)

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `channel.next_active` / `channel.prev_active` | PgDown / PgUp | C | Move the active-channel square to the next/previous trace of the component (wrapping within the component; graph: across groups in key order). | yes |
| `channel.hide` | Ctrl+H | C | Set the active trace `visible = false` (spec edit). | yes |
| `channel.remove` | Ctrl+Delete | C | Remove the active trace from the component (spec edit). | yes |
| `channel.edit_maths` | Ctrl+Shift+M | C | Open the Maths Editor at the item defining the active channel (i2: "if maths channel"); for a logged channel a no-op with a status-bar hint — it does not create a new expression (XREF 03 §11.10). | yes |
| `channel.status` | Ctrl+Shift+S | C | Show and focus the Channel Status dock with the active channel's row selected (i2: "if maths channel"; aliases and mappings have rows too); for a logged channel without a row, a status-bar hint. No separate status popup (XREF 03 §11.10). | yes |
| `channel.filter` | Ctrl+Shift+F | C | Filter dialog (type, window/order) → **transient** maths shadowing the channel for this session (D-015). | yes |
| `channel.scale_offset` | Ctrl+Shift+O | C | Scale/offset dialog → `localMaths` item in the sidecar, DPS preserved. | yes |
| `channel.zero_at_cursor` | Ctrl+Shift+Z | C | Offset = −value at cursor → `localMaths` item. | yes |
| `channel.properties` | Ctrl+N | C | Properties dialog on the Channels tab at the active channel. | yes |

**Components general**

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `component.properties` | F5 | C | Open/raise the component's properties dialog (§12). | yes |
| `component.focus_next` / `component.focus_prev` | Tab / Shift+Tab | W | Focus next/previous component in z-order. | yes |
| `component.legend` | L | C | Toggle the channel legend (key column / data legend). | yes |
| `component.colour_legend` | K | C | Toggle the colour-channel legend (scatter Z, histogram colour channel, track contour). | yes |
| `component.cycle_layout` | G | C | Cycle layout: graph panels ↔ overlapped; scatter overlapped → tiled → average; histogram overlapped ↔ tiled. | yes |
| `component.cycle_style` | S | C | Cycle style: graph line ↔ points; histogram bar → line → text; scatter points → points+trend → trend. | yes |
| `component.colour_band` | Alt+0 … Alt+9 (args `[n]`) | C | Toggle colour band *n* of the component's `BandAxis`/`ColourBands`. | yes |
| `component.maximise` | — | C | Maximise/restore the focused component (§4.3.6). | yes |
| `component.send_to_background` / `component.bring_to_front` | — | C | z-order moves. | yes |
| `component.export_data` | — | C | Export Data dialog (CSV; graph: channels of that graph only). | yes |
| `component.print_clipboard` | — | C | `renderToImage(screen DPR)`: paint the component's current layers into one image (04 §7.2 item 9; never blocks, the previous Static image while a rebuild is in flight) and put it on the clipboard; `QTableView` components use `QWidget::render`. | yes |

**Graph display**

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `graph.status_errors` | E | GR | Toggle the status & errors strip. | yes |
| `graph.device_events` | Shift+E | GR | Hide/show device events in the marker band. | yes |
| `graph.variance` | F3 | GR | Toggle the variance panel (drawn only while an overlay is visible, D-023). | yes |
| `graph.measurements` | M | GR | Toggle min/max/avg measurements over the visible span. | yes |
| `graph.copy_cursor_values` | Ctrl+Shift+C | GR | Copy `name<TAB>value<TAB>unit` rows for every configured channel at the Main cursor. | yes |
| `cursor.add_annotation` | — | GR | Add an annotation at the cursor (sidecar, Main-source absolute time). | yes |

**Graph zoom** (keys also apply to summary components, which recompute over the zoom)

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `zoom.in` / `zoom.out` | Up / Down | GR, S | Horizontal zoom ×0.5 / ×2 about the cursor (≥ 4 samples). | yes |
| `zoom.full` | F2 | G | Whole Main outing (`[outing.start − Main.start, outing.end − Main.start)` in the current domain, 04 §8.6) on the shared zoom; works from inside any dock (§3.3.1). | yes |
| `zoom.default_range` | W | GR, S | One Main range; cursor pulled inside if outside. | yes |
| `zoom.datum_main` | Z | GR | Zoom to [datum, main] (requires datum). | yes |
| `zoom.v_in` / `zoom.v_out` | Alt+Up / Alt+Down | GR | Vertical zoom of the active group ×0.5 / ×2 about the active channel's cursor value. | yes |
| `zoom.v_full` | Alt+F2 | GR | Vertical full for the active group. | yes |
| mouse `zoom.rubber_band` | double-click, move, click | GR | Horizontal rubber band; ALT held = vertical on the active group; CTRL held = box (both axes). | yes |
| mouse `zoom.full_axis` | double-click a scrollbar | GR | Full horizontal / vertical zoom for that axis. | yes |
| mouse `zoom.wheel` | wheel | GR, S, T | Horizontal zoom about the pointer (graph); box zoom about the pointer (scatter, track). | yes |
| mouse `zoom.extend` | Shift+click in the zoom summary bar | G | Extend the zoom span to the clicked position. | yes |
| mouse `zoom.outing_rect` | drag rectangle ends / bottom edge | GR (`outing`) | Set the linked zoom span. | yes |

**Graph pan**

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `pan.left` / `pan.right` | Shift+Left / Shift+Right | GR | Pan by 10 % of the span. | yes |
| `pan.up` / `pan.down` | Shift+Up / Shift+Down | GR | Vertical pan of the active group when vertically zoomed. | yes |
| `pan.back` / `pan.forward` | B / F | GR | Pan by 90 % of the span. | yes |
| `pan.centre_cursor` | H | GR | Centre the view on the cursor; Options "auto-centre" keeps it centred on every move. | yes |
| mouse `pan.drag_axis` | drag the x or y axis / scrollbars | GR | Pan; scrollbar offsets survive resize. | yes |

**Graph cursor**

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `cursor.left` / `cursor.right` | Left / Right | GR, S | One screen pixel's worth of samples of the active trace (scatter: next/previous Main point). | yes |
| `cursor.left_fast` / `cursor.right_fast` | Ctrl+Left / Ctrl+Right | GR, S | 10 × the single step. | yes |
| `cursor.home` / `cursor.end` | Home / End | GR | Cursor to the left / right edge of the visible span. | yes |
| `cursor.style` | Y | GR, S | Cycle cursor style line → crosshair → none-with-values. | yes |
| `cursor.insert_marker` | Shift+F7 | GR | Insert a BCN (Circuit) / run (Drag) / stage (Rally) marker at the cursor into the sidecar; laps rebuild (D-014). | yes |
| mouse `cursor.place` | click | GR | Place the cursor, snapped to the nearest Main sample of the active channel; clicks in the marker band never move the cursor. | yes |

**Datum cursor**

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `datum.toggle` | D | GR | Datum cursor off/on (keeps its position while off). | yes |
| `datum.place` | Space | GR | Place the datum at the main cursor. | yes |
| `datum.swap` | X | GR | Swap the positions of the datum and main cursors (`cursorT` ↔ `datumT`, i2 "swap datum and main cursor"; 04 §8.5 item 4); the arrow keys, which always move the main cursor, then adjust the other point. | yes |

**Alignment offsetting**

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `offset.axis_toggle` | O | GR | Offset axis on/off; turning off zeroes every offset of the selected ranges (undoable). | yes |
| `offset.main_left` / `offset.main_right` | Ctrl+Shift+Left / Ctrl+Shift+Right | GR | Offset the Main range by one **offset step** backward / forward: one sample period of the active channel in time mode, one 0.25 m `DistanceIndex` step in distance mode (zoom-independent; 04 §3.3 item 3, 07 AC-M3-09). | yes |
| `offset.main_down` / `offset.main_up` | Ctrl+Shift+Down / Ctrl+Shift+Up | GR | Offset the Main range by ten offset steps backward / forward (documented choice; i2 lists "arrow" only). | yes |
| `offset.overlay_left` / `offset.overlay_right` | Alt+Shift+Left / Alt+Shift+Right | GR | Offset the reference overlay ("ref lap") by one offset step (step of the active channel in that overlay's source). | yes |
| `offset.overlay_down` / `offset.overlay_up` | Alt+Shift+Down / Alt+Shift+Up | GR | Ten offset steps. | yes |
| mouse `offset.drag` | drag the Main or overlay sub-axis | GR | Offset by mouse; one command on release. | yes |

**Track and GPS**

| Command id | Default key | Ctx | Behaviour | MVP |
|---|---|---|---|---|
| `track.zoom_in` / `track.zoom_out` | Plus (also `=`) / Minus | T | Zoom the track view about its centre while focused. | yes |
| `track.fit` | — | T | Fit the whole track. | yes |

### 14.5 Tests (D-033)

`tests/widgets/tst_keymap.cpp` generates one Qt Test row per binding with `"status": "implemented"` in `data/keymaps/default.json` (`QT_QPA_PLATFORM=offscreen`): build a fixture project with two sources, set the focus context, send the key with `QTest::keySequence`, assert the documented post-condition (table-driven lambdas keyed by command id). `planned` rows are exempt until their `milestone`; `reserved` rows share one generic row asserting the command is registered, disabled and shows its tooltip. Coverage (`tst_keymap_coverage`, mirrored by `tools/check_keymap_coverage.py`) fails the build when an `implemented` row has no test lambda or when a row's `milestone` is at or before the current milestone but its status is not `implemented`. Test effort for ≈ 100 MVP rows is spread over M1–M3 with the batches of §22 (XREF 07 §6.3 budget). `tst_dataselection.cpp` covers every row of §5.2 and the invariants; `tst_worksheetviewmodel.cpp` covers F9 both ways with and without `DistanceIndex`, N/P zoom preservation, W, Z, animation stepping.

---

## 15. Command palette (`CommandPalette`, CTRL+SHIFT+P)

1. Frameless popup centred over the main window; a line edit and a list; opens with the full enabled command list sorted by title; typing filters with fuzzy subsequence matching on `title`, `id` and `menuPath` (prefix matches ranked first, then recently used).
2. Each row: title · menu path · current key (from the keymap) · context badge; disabled commands are hidden unless the query starts with `?`.
3. ENTER invokes in the context captured when the palette opened (focused component, highlighted Data-window range); ESC closes; UP/DOWN move; the last 20 invoked ids are stored per user.
4. Prefix `>` lists worksheets/workbooks (type to jump), `@` lists channels (ENTER adds to the focused component, CTRL+ENTER as a new panel), `#` lists ranges of the Main source (ENTER = Main, CTRL+ENTER = Overlay 0, ALT+ENTER = overlay).
5. Help › Keyboard Shortcuts opens the palette with the query `?` and the key column widened; *(v1.0)* "Record key" in the row context menu jumps to Options › Keys for that command.

---

## 16. Schemes (`scheme-1`)

Shipped: `data/schemes/light.scheme.json`, `data/schemes/dark.scheme.json` (our own; D-004; file names carry no codename so a D-003 rename touches only the `name` field; same names in 04 §6.7 and 08 §1). This section is the canonical `scheme-1` shape: role names, `overlayStyle` keys and font roles below are the ones 02 §6.9 and 04 use (XREF 02 §6.9). User schemes in `~/.config/esses/schemes/`. The active scheme name is per user (`settings.json`); "Follow OS light/dark" picks the shipped scheme matching `QStyleHints::colorScheme` unless the user chose a scheme explicitly.

```json
{
  "$schema": "urn:esses:scheme-1",
  "schemaVersion": 1,
  "name": "Esses Dark",
  "background": "dark",
  "roles": {
    "windowBackground": "#1e1f22", "cptFrame": "#3a3d41", "cptBackground": "#141517", "cptForeground": "#e6e6e6",
    "cptForeground2": "#9aa0a6", "cptFocusBorder": "#4c9aff", "cptCaptionBackground": "#2b2d31", "cptCaptionForeground": "#e6e6e6",
    "dataBackground": "#1e1f22", "dataForeground": "#e6e6e6", "dataForeground2": "#9aa0a6", "dataGrid": "#33363a",
    "dataHeadingBackground": "#2b2d31", "dataHeadingForeground": "#ffffff", "dataHighlightBackground": "#264f78",
    "dataHighlightForeground": "#ffffff", "dataError": "#ff6b6b", "dataUnknown": "#9aa0a6", "keyBackground": "#1a1b1e",
    "cursorMain": "#ff3b30", "cursorDatum": "#ffd60a", "cursorZoom": "#4c9aff", "rangeMark": "#6e7681",
    "sectionCorner": "#5a3d7a", "sectionStraight": "#2f5f3a", "markerBand": "#2b2d31", "annotationMark": "#ffb000",
    "deviceEvent": "#8e8e93", "varianceZero": "#6e7681", "statusOk": "#34c759", "statusWarn": "#ffb000", "statusError": "#ff3b30",
    "pendingHatch": "#3a3d41", "mainSingleColour": "#ffffff", "overlay0": "#bdbdbd", "trackLine": "#8e8e93"
  },
  "channelPalette": ["#ff5252","#40c4ff","#69f0ae","#ffd740","#ea80fc","#ffab40","#18ffff","#b2ff59","#ff80ab","#8c9eff","#ccff90","#ffe57f"],
  "overlayPalette": ["#4fc3f7","#aed581","#ffb74d","#ba68c8","#4db6ac","#f06292","#fff176","#90a4ae","#a1887f"],
  "overlayStyle": { "colourSource": "channel", "dash": "solid", "lightness": 0.0, "widthScale": 1.0 },
  "fonts": {
    "axis":        { "family": "Noto Sans", "pointSize": 8,  "weight": 400, "italic": false },
    "axisRotated": { "family": "Noto Sans", "pointSize": 8,  "weight": 400, "italic": false },
    "caption":     { "family": "Noto Sans", "pointSize": 9,  "weight": 600, "italic": false },
    "key":         { "family": "Noto Sans", "pointSize": 8,  "weight": 400, "italic": false },
    "keyStrikeout":{ "family": "Noto Sans", "pointSize": 8,  "weight": 400, "italic": false, "strikeOut": true },
    "small":       { "family": "Noto Sans", "pointSize": 7,  "weight": 400, "italic": false },
    "colourBands": { "family": "Noto Sans", "pointSize": 7,  "weight": 400, "italic": false },
    "trackLabel":  { "family": "Noto Sans", "pointSize": 8,  "weight": 600, "italic": false },
    "equation":    { "family": "Noto Sans", "pointSize": 8,  "weight": 400, "italic": true },
    "reportTable": { "family": "Noto Sans", "pointSize": 9,  "weight": 400, "italic": false },
    "reportHeading":{ "family": "Noto Sans", "pointSize": 9, "weight": 700, "italic": false },
    "dataHeader":  { "family": "Noto Sans", "pointSize": 9,  "weight": 600, "italic": false },
    "dataRange":   { "family": "Noto Sans", "pointSize": 9,  "weight": 400, "italic": false },
    "valuesHeader":{ "family": "Noto Sans", "pointSize": 9,  "weight": 600, "italic": false },
    "valuesItem":  { "family": "Noto Sans", "pointSize": 9,  "weight": 400, "italic": false },
    "gauge":       { "family": "Noto Sans", "pointSize": 14, "weight": 600, "italic": false }
  },
  "print": {
    "roles": { "cptBackground": "#ffffff", "cptForeground": "#000000", "dataGrid": "#c8c8c8", "windowBackground": "#ffffff" },
    "channelPalette": ["#c62828","#1565c0","#2e7d32","#f9a825","#6a1b9a","#ef6c00","#00838f","#558b2f","#ad1457","#283593"]
  }
}
```

Rules:

1. `background` selects which shipped palette family a user scheme derives from; the channel palette is separate for light and dark backgrounds (i2 rule 4) simply because each scheme file carries its own.
2. `channelPalette` has ≥ 10 entries; `ChannelDescriptor.colourIndex` indexes it modulo its length; alias descriptors carry their own index so alias colours are consistent across outings (D-013).
3. `overlayPalette` has exactly 9 entries (range key colours of Overlays 1..9, cycling).
4. `overlayStyle` applies to Overlays 1..n traces: `colourSource: "channel" | "overlayPalette"`, `dash: "solid" | "dash" | "dot"`, `lightness` in −1..1 (HSL lightness shift), `widthScale`. Default = plain channel colours (D-017). Overlay 0 traces always use `roles.overlay0`.
5. `print` overrides any subset of `roles`, `channelPalette`, `overlayPalette`, `fonts`; `OffscreenRenderer` uses the merged print variant for printing (v1.0); when Options › Colours "clipboard uses print variant" is on, Print to Clipboard submits one extra Static build with the print variant through `RenderService` and copies the image when it lands (never blocking the GUI thread).
6. Qt mapping: `windowBackground`/`dataForeground`/`dataHighlight*` map to `QPalette` roles (`Window`, `WindowText`, `Highlight`, `HighlightedText`, `Base`, `Text`); the rest are read by `SchemeFonts` and the components via `Scheme` (Qt-free document in `esses_project`). Fonts load through `QFont` with `pointSizeF`. The two shipped schemes use **Noto Sans** (OFL-1.1) for every font role; golden images load exactly that family from `tests/golden/fonts/` with system fonts disabled (D-025; 04 §6.7 item 2). That directory is outside the shipped `data/` tree and REUSE-annotated `OFL-1.1`, not covered by the `data/**` MIT annotation (08 §6.4/§10, D-127). The installer does not bundle Noto Sans in 0.1 (08 §6.4); where it is not installed the app falls back through Qt family substitution.
7. A scheme change emits `ChangeMask::Scheme` to every worksheet (full repaint) and restyles docks through `QApplication::setPalette`; OS dark-mode changes are observed via `QStyleHints::colorSchemeChanged`.
8. Scheme files validate against `scheme-1` on load; a missing role falls back to the shipped scheme of the same `background` with a Messages warning.
9. The `.mcs/.mfs` importer (v1.0) maps vendor entries by a table in `esses_project` onto these role names; it is not part of the MVP and no vendor names appear in our files.

---

## 17. HiDPI and multi-monitor

1. `QGuiApplication::setHighDpiScaleFactorRoundingPolicy(PassThrough)` before `QApplication` is constructed; all widget layout in logical pixels.
2. Plot layers are rendered at device pixels: `LayerCache` allocates the Static `QImage` of `size × dpr` with `setDevicePixelRatio(dpr)` and paints the Dynamic and Chrome lists directly (04 §6.2); `DrawList` line widths are logical × DPR; text through `QFontMetricsF` (D-025).
3. `ComponentWidget` listens to `QWindow::screenChanged` and `devicePixelRatioChanged`; a change invalidates all three layers (`Size|Scheme` mask) and re-lays out the key column.
4. Floating docks on a monitor with a different DPR render at that monitor's DPR (ADS floating widgets are top-level windows); the M4 HiDPI sweep includes the mixed-DPI floating case (D-034/outline M4).
5. Window geometry restore (§3.4) checks that the saved rectangle intersects an available screen by ≥ 25 %; otherwise it centres on the primary screen at 1366×768 or smaller.
6. Icons are SVG (`QIcon::fromTheme` with our own fallback set); the 1 % snap grid and the 6 px handles scale with DPR.
7. Golden images for every component are rendered at DPR 1 and 2 (D-033).

---

## 18. Session restore and `SettingsStore`

`SettingsStore` persists per-user state in `QStandardPaths::AppConfigLocation` (`~/.config/esses/` on Linux, `%APPDATA%/esses/` on Windows, `~/Library/Application Support/esses/` on macOS) as `settings.json` (schema `settings-1`, D-055; the document is defined in 02 §6.9): active scheme, follow-OS flag, keymap name, MRU projects (10) and log files (20), last Open dialog folder and pane state, importer overrides per `(extension, preamble fingerprint)`, toolbar/status bar visibility, palette history, language, window geometry for the no-project state, "do not show again" flags. Written atomically on change (coalesced 1 s).

Startup sequence (`esses_app`). `main()` constructs `QApplication` first, then the `esses::qt::AppRuntime` that owns the `TaskPool`, `ChunkCache`, `Dispatcher` and log sinks (01 §5.3 rule 11); nothing uses a pool, cache or dispatcher before it exists, and there are no function-local static singletons:

```cpp
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  esses::qt::AppRuntime rt;            // after QApplication
  /* steps 1–7 below */
  const int rc = app.exec();
  rt.shutdown();                       // cancelAll(""), join, remove MessagesSink, close Dispatcher, clear cache
  return rc;                           // rt, then app, destroyed after shutdown
}
```

1. Parse arguments (§19). If another instance is running and `--new-instance` is not given, forward the arguments to it (`QLocalSocket` on `esses-<user>`) and exit 0 (proposed; see §24).
2. Load `settings.json`, the active scheme and the keymap; register commands; build `MainWindow`; apply `QPalette`.
3. Open the project: `-w <project>`, else the MRU project when Options › General "load previous data with project" is on, else the empty state (menus enabled for New/Open Project). Replay the journal if non-empty (D-019) after asking Recover / Discard.
4. Restore `.local/layout.json`: geometry, perspective of the active workbook, dock states, active workbook/worksheet.
5. Open sources: every `-l <log>` argument (any importer) in order, then — if no `-l` was given and the option is on — the project's MRU data; each goes through the `SourceLoader` pipeline with the Mapping panel if needed.
6. Restore the selection snapshot (§5.5) once every requested source has published or failed; first paint happens as soon as the first source publishes (budget: 100 MB `.ld` → first paint ≤ 500 ms, outline §8).
7. Start the autosave timer and the scheme/OS watchers.

Exit: dirty + "save on exit" on → save, off → ask Save / Discard / Cancel (project documents only); write `layout.json` and `settings.json`; flush any pending sidecar write (sidecars are not project documents: they write through after each committed sidecar command, coalesced within 500 ms, and on source close, and are never prompted for, 02 §5.3.4); then, after `app.exec()` returns, `rt.shutdown()` runs the 01 §5.3 rule 11 order (`pool.cancelAll("")`, `pool.join()`, remove the Messages sink, `dispatcher.close()`, `cache.clear()`) before the runtime and `QApplication` are destroyed. A crash leaves the journal for recovery of project documents; sidecar edits older than the 500 ms coalescing window are already on disk.

---

## 19. File associations and command-line arguments

```
esses [-w <project-folder>] [-l <log-file>]... [--new-instance] [--scheme <name>] [--keymap <name>]
      [--offscreen] [--dump-commands] [--version] [<file>...]
```

1. `-w <project>` (also `--workspace`): the project folder or its `project.json`; i2-compatible spelling (D-034).
2. `-l <log>` (also `--log`): repeatable; any file the `ImporterRegistry` sniffs; relative paths resolve against the current directory.
3. Bare arguments: a folder containing `project.json` → `-w`; a file with a registered importer extension → `-l`; a `*.workbook.json` → import into the open project (Layout Editor rule §4.2.8).
4. `--dump-commands` prints the command table as JSON (used by `esses-cli validate` for keymaps); `--offscreen` sets `QT_QPA_PLATFORM=offscreen` for tests.
5. Windows: the Inno Setup installer registers `.ld` (and the portable zip ships a script) under ProgId `Esses.LogFile` with `"esses.exe" -l "%1"`; the Open-with verb for `.csv` is registered as a secondary handler only (never default). Linux: `esses.desktop` with `MimeType=application/x-esses-log;` and a `mime/packages/esses.xml` mapping `*.ld` to that type with a low priority glob; macOS: `CFBundleDocumentTypes` for `ld` with `LSHandlerRank Alternate`.
6. Double-clicking a second `.ld` while Esses runs adds the file to the running instance's open project (step 1 of §18); with `--new-instance` a second window opens.

---

## 20. Accessibility basics

1. Every command is reachable by keyboard (menus, the palette, Options › Keys); no function is mouse-only except axis/sub-axis drags that have keyboard equivalents listed in §14.4.
2. Every dock, component, dialog page and editable cell has an accessible name/description (`setAccessibleName`); components expose caption, active channel and cursor values as the description, updated at ≤ 5 Hz.
3. Focus is always visible: component focus border ≥ 2 logical px in `cptFocusBorder` at ≥ 3:1 contrast against `cptBackground` (scheme contrast lint, §13.9.4).
4. Colour is never the only carrier of meaning: M/O/1 columns and range-band labels distinguish slots; untrusted ranges are italic as well as greyed; status glyphs have text tooltips.
5. OS text scaling (`PassThrough` DPR, point-size fonts) and "reduce motion" (`QStyleHints`: no fly-out animation, animation starts paused) are respected.
6. Minimum window size 1366×768; dialogs lay out at 1024×700 with scroll areas.
7. Tooltips on toolbar buttons and status-bar fields show the current key binding.

---

## 21. Deliberate deviations from i2 (shell scope) and why

1. **Modeless tabbed property dialogs with Apply and undo** (D-027, D-032): no OK/Cancel round-trips, every edit reversible; i2's dialogs are modal and nothing in i2 is undoable.
2. **Global undo/redo (CTRL+Z/CTRL+Y)** across layout, channel, maths, details, marker and track edits; selection/cursor/zoom excluded so the stack stays about documents.
3. **Remappable keymap on top of unchanged i2 defaults** plus a **command palette**: discoverability without altering the muscle memory that users praise; the defaults in §14.4 are never changed by us.
4. **Dock layout is per user in `.local/layout.json`, not in the workbook**: a shared workbook never drags a colleague's screen arrangement along; i2 keeps dock state in the workspace file.
5. **JSON project/workbook/scheme/keymap files with schema validation and migrations**: addresses the "project-file corruption" complaint; users never hand-edit them.
6. **Write-ahead journal autosave** instead of rewriting project files every two minutes (D-019).
7. **Messages dock with Import Log, maths diagnostics and memory warnings** always available; i2 surfaces out-of-memory in View › Messages only.
8. **Inline Mapping panel** instead of a modal conversion wizard (D-029).
9. **Open Log File is a native file dialog with a Details side pane** in the MVP; the indexed four-pane dialog arrives in v1.0 (D-019).
10. **OS-native look, OS dark-mode following, SVG icons**; no attempt to copy i2 chrome, icons or fonts (D-004).
11. **No Pro/Standard licence gates**: workbooks, docking, unlimited overlays, datum cursor, CSV import are always on.
12. **Compact overlay renumbering on removal** (D-017, to verify); the scheme's `overlayStyle` makes any observed styling difference a data fix.
13. **No transparent Track Map window** (compositing-dependent, low value); the dock supports AutoHide and Floating instead.
14. **No Telemetry View, Data Sets or Range Key view** in the MVP (semantics undocumented; listed in §24).
15. **Single-instance forwarding of `-l` arguments** so double-clicking files adds them to the running session (proposed, §24).

---

## 22. Acceptance criteria

M1 (walking skeleton): `MainWindow` with Data, Channels, Values and Messages docks; `DockHost` on ADS with save/restore of `layout.json` round-tripping byte-identical across a restart; `DataSelectionModel` passing every §5.2 row; `WorksheetViewModel` passing F9/N/P/W tests; hotkey batch 1 (views, data, zoom, pan, cursor) with Qt Test rows green; `-w`/`-l` open a project and two logs to first paint ≤ 500 ms for a 100 MB `.ld`.

M2: Channels window search semantics tested with the ten examples in §7.3 and the Data-window filters (moved from M1, D-044); Channel Editor, Maths Editor (live diagnostics, unit popup, Insert Range, refuse-close-on-error), Channel Status with resolution traces, Details and Lap editors, Options dialog, two shipped schemes with live switching, `QUndoStack` covering every editor; project reopens with previous selection and single-colour state.

M3: every row of `keymap.json` whose `milestone` is ≤ M3 has `"status": "implemented"` and a passing Qt Test (`planned` v1.0 rows and `reserved` rows are covered by the generic row of §14.5); `tst_keymap_coverage` green; properties dialogs for all ten MVP components with Apply/undo and the schema-driven `properties_roundtrip` of §12 item 7; Layout Editor basics; `WorksheetCanvas` move/resize/multi-select/maximise/splitters/paste-offset tests; command palette with `>`/`@`/`#` modes; Track Map dock; Print to Clipboard produces the same image as `esses-render` at the same DPR.

M4: HiDPI sweep including mixed-DPI floating docks; layout restore across Qt/ADS version bumps falls back cleanly; file associations and single-instance forwarding verified on clean Windows 10 and 11 VMs (one with an old VC++ redistributable) and an Ubuntu 24.04 VM, with a runner smoke test on macOS (07 AC-M4-04; AppImage and DMG "builds green, not supported", D-116); the scripted "Saturday" walkthrough drives the shell purely through command ids.

---

## 23. `CLEANROOM.md` provenance lines (to add)

Each line names its source **category** (the Category column of the `CLEANROOM.md` table, XREF 08 §11): *help page*, *public release notes*, *vendor-shipped template* (a file installed by the vendor; read-only, attribute names only, never redistributed), *file written by the user's install* (D-005 (2); the consent date is recorded in the row), *observed on a licensed install* (D-005 (3)).

- Hotkey table, scope rules, focus model — *help page* ("Hot Keys", "Docking Windows (Pro)", "Components Overview") and *public release notes*; no help text reproduced, behaviours restated.
- Data window columns, filters and selection semantics, fastest-lap candidate rule — *help page* ("Data Window") and *public release notes* 1.0.19–1.1.4 (filters, lap-number filter, Shift+F4, Ctrl+Alt+F, selection restore, in/out laps never fastest).
- Channels window search semantics — *help page* ("Channels Overview") and *public release notes* (loose matching).
- Workbook/worksheet/Layout Editor behaviours — *help page* ("Workbooks (Pro)", "Layout Editor"); geometry conventions (`RectGridPts`, `PosPC`) — *vendor-shipped template* (workbook templates installed with i2), used only to define a lossless import.
- Dock states and perspective persistence — *help page* ("Docking Windows (Pro)"); the fact that i2 keeps dock state in the workspace file — *file written by the user's install* (`Workspace.i2wsp`, D-005 (2); D-005 (2) consent 2026-10-10, research use 2026-10-09, D-039); our `layout.json` shares no structure with it.
- Scheme model — *help page* ("Colour Schemes", "Options"); role and font role *counts* — *file written by the user's install* (`userprefs.xml`, D-005 (2); D-005 (2) consent 2026-10-10, research use 2026-10-09, D-039); all role names, palettes and fonts in `data/schemes/` are our own.
- Lap Editor beacon offset (distance), unit-override removal — *help page* ("Laps") and *public release notes*.
- Shared zoom level across worksheets, per-worksheet time/distance mode — *help page* ("Histogram", "Time/Distance Graph").
- Command line `-w`/`-l` and `.ld` double-click — *public release notes*.
- Overlay colour/numbering rules — *help page* ("Data Window"/"Hot Keys"); renumbering, overlay 1..n styling and cursor persistence across worksheets — *observed on a licensed install*, no internals inspected (D-005 (3), D-017; added once the M1 observation has run, recorded "chosen, not observed" until then).

---

## 24. Open items and proposed decisions (reported in `unresolved`)

1. Task text asked for "Properties as a live dock with undo"; D-027 fixes modeless tabbed dialogs with Apply and undo. This document follows D-027.
2. Proposed: a range may occupy only one selection slot; setting Main to a current overlay removes it from the overlay set (§5.2) — not documented for i2.
3. Proposed: N/P do not wrap, stay within the Main source and step to the adjacent displayed range even when it holds an overlay slot (the slot is vacated); CTRL+F uses the §5.2 candidate rule (trusted flying laps if any, else flying/partial laps; in/out/pit never), which corrects the "untrusted laps are never fastest" reading in D-017's context against release note TT 2485 (DECISIONS.md Addendum A); CTRL+ALT+F uses `fastLapTolerancePct` (default 1 %) — i2's "fast laps" definition is undocumented.
4. Proposed: `overlaysHidden` makes `referenceOverlay()` empty for variance/Δ/reports (not only for drawing).
5. Proposed: offset step = one sample period of the active channel (time) / 0.25 m (distance); CTRL+SHIFT/ALT+SHIFT + UP/DOWN = ten steps (i2 says only "arrow"); same in 04 §3.3 and 07 AC-M3-09.
6. Proposed: worksheet key 0 = tenth tab; CTRL+0 = tenth workbook.
7. Proposed: `WorkbookFile.id` (UUID) as the perspective key; derived from the file name when absent — the outline names "workbook id" without defining it.
8. Proposed schema id `settings-1` for the per-user `settings.json` (outline §5 lists "settings" without a schema). Adopted as D-055; outline §5 now lists `settings-1`, defined in 02 §6.9.
9. Proposed: single-instance forwarding of CLI arguments via `QLocalSocket`, `--new-instance` to opt out.
10. Proposed: active workbook/worksheet live in `.local/layout.json`, with the workbook's `activeWorksheet` as the fallback only.
11. Proposed: Values dock "All channels" toggle persists per user; i2's rule (focused component vs all) is an open observation (D-005 §3, DECISIONS open question Q-08).
12. Not in MVP and undocumented: Data Sets / Range Key view, Telemetry View, user-defined per-worksheet key bindings (slot reserved in `keymap-1` and `WorksheetDef.keys`).
13. Outline §4 lists `W` among `DataSelectionModel` slots; this document implements W in `WorksheetViewModel::zoomToMainRange` because it changes zoom and cursor, not selection (consistent with D-018; the outline sentence should be corrected).
14. Proposed (DECISIONS.md Addendum A, D-133, amends D-018): one workspace-level view (zoom, cursor, datum in `WorkspaceViewModel`), domain and offset-axis visibility per worksheet (§4.1.4); D-005 (3) verification item: does i2 keep the cursor/datum when switching worksheets?
15. Proposed: Lap Editor beacon offset is a per-source distance stored as a `MarkerEdit` op `offsetDistance {dM}` and resolved through `Corr Dist` (§13.3 item 3); 02 §4.3 behaviour 7a and the `sidecar-1` example now carry it.
16. Proposed: a unit override that conflicts with a newly opened file is removed as in i2, undoably, with a Messages row (§13.1 item 6).
17. Proposed: F9 and F2 are Global-context bindings acting on the active worksheet (§3.3.1).
18. Scoped down for 0.1 (D-035 sizing): editor sub-features marked *(v1.0)* in §13; Options › Fonts and Keys tabs are read-only, schemes and keymaps are edited as JSON; property pages bind through `FieldBinder` with a schema-driven round-trip test (§12 items 6–7). Replacing the hand-built pages by schema-generated grids would reverse D-027 and is not done here.
19. `keymap-1` rows carry `status`/`milestone`/`doc` (§14.3); 02 §6.9, 07 AC-M1-06/AC-M3-01 and 08 §6.5 use this shape and the snake_case ids.
