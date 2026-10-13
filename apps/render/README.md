# esses-render (esses_render)

Layer 4, Qt Gui offscreen (01 §2.1, 04 §6.7 item 5; AC-M1-05: the PNG equals the on-screen `LayerCache`
composite for the same `ViewState` within 0.5 %).

```
esses-render <workbook.json> <log...> --worksheet N -o out.png [--dpr 2] [--component K] [--selection fastest]
             [--size WxH] [--scheme light|dark|<file>] [--font-dir DIR] [--timeout S]
```

Links `esses_qtmodel` (SourceLoader, MathsService, DistanceService, DataSelectionModel, WorksheetViewModel,
RenderService::prepareInputs) and `esses_qtrender` (OffscreenRenderer); no Qt Widgets. Sets
`QT_QPA_PLATFORM=offscreen` when unset. No thread is marked NoBlock, so the tool may wait on its own futures
(04 §1 item 4).

Pipeline (the application's): load the workbook; the rules of its profile (`<data>/profiles/<profile>.profile.json`
-> `RuleRunner::fromRuleIds`, else `RuleRunner::circuitDefault()`) become the open pipeline's range rebuild; every
log is opened through `SourceLoader`; each published source goes to `MathsService::addSource`,
`DataSelectionModel::sourcePublished` and `DistanceService::prefetch`; `--selection fastest` runs
`selectFastest()` (CTRL+F); the tool waits until Corr Speed / Corr Dist are evaluated (or failed) and the
visible ranges' distance chains are ready (or the source has none, which renders the banner); a
`WorksheetViewModel` of the worksheet (its own domain) is zoomed to the Main range (W); the inputs come from
`RenderService::prepareInputs` and `OffscreenRenderer` renders them.

| Option | Default | Notes |
|---|---|---|
| `--worksheet N` | 0 | out of range: exit 2 |
| `--dpr R` | 1 | 0.5 to 8; fractional values allowed (04 §6.6) |
| `--component K` | all | renders component K at its laid-out size inside a `--size` worksheet |
| `--selection` | `fastest` | `fastest` or `default` (the selection a newly opened log gets) |
| `--size WxH` | 1600x900 | logical pixels of the worksheet |
| `--scheme` | `light` | `light`, `dark` (the shipped schemes) or a `*.scheme.json` |
| `--font-dir DIR` | `ESSES_TEST_FONT_DIR` | registers DIR's fonts and points `QT_QPA_FONTDIR` at it (goldens: `tests/golden/fonts`); otherwise, on Windows, `QT_QPA_FONTDIR` defaults to `%WINDIR%\Fonts` because the offscreen platform's FreeType database sees no system font |
| `--timeout S` | 120 | for opening the logs and the maths; on a maths timeout it renders what is ready |

Exit codes: 0 written (one line on stdout), 1 failure (missing data directory, workbook, scheme, log, timeout,
write error), 2 bad arguments. ctest: `render.help`, `render.usage_missing_output`, `render.usage_bad_dpr`,
`render.usage_bad_size`, `render.missing_workbook`. The end-to-end run on
`fixtures/motec.ld/synthetic-acc-dialect/input.ld` is `tests/ui/tst_render_parity` (AC-M1-05).
