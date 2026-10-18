# Golden images (AC-M1-04)

Pixel goldens of the M1 worksheet cases, rendered with `esses_qtrender`'s `OffscreenRenderer` (04 §6.7, D-033,
D-127):

- `images/tst_golden_images.cpp`: the Qt Test executable (ctest `golden.tst_golden_images`, offscreen), registered
  by `CMakeLists.txt` through `esses_add_qt_test_dir`; other `.cpp` files in `images/` would be shared helpers.
- `images/time_distance/<case>@<dpr>x.png` with `<case>.json` (the component body, size, scheme, domain, zoom,
  cursor, datum, slots and their synthetic sources) for the eight time_distance cases of
  `libs/plotcore/tests/td_golden_cases.hpp` (single trace, 20 traces, like-units group, two panels, distance mode
  with an overlay, dark scheme, time overlays, the no-distance banner), each at DPR 1 and 2 (16 PNGs). The same
  cases are the `DrawList` JSON goldens of `drawlist/time_distance/` (plotcore, `FixedTextMetrics`); the pixel
  goldens use `QtTextMetrics` with the golden fonts.
- `images/time_distance/goldens.json`: where the PNGs were rendered (`generatedOn`, Qt version, platform) and, in
  `cases`, the provisional cases with where their PNGs came from (a case not listed takes `generatedOn`).
- The thirteen M3 time_distance cases of `libs/plotcore/tests/td_m3_cases.hpp` (AC-M3-02, D-201: variance in
  both domains and its banner, the datum key, measurements, the offset axis in both domains, the bands,
  annotations and device events, the status strip, overlapped mode, scale lines with the crosshair, the dark full
  anatomy) in the same `tst_golden_images.cpp` and directory: `DrawList` JSON goldens in `drawlist/time_distance/`
  (exact everywhere) and PNGs rendered on Windows (`cases`: `"windows"`, compared in report mode until replaced
  from the ubuntu artefact). The M1 `dark_scheme` case changed with the datum's key columns: its `DrawList` golden
  is regenerated and its ubuntu PNG is `"linux-stale"` until replaced. `ESSES_UPDATE_GOLDENS_CASES=<a,b,...>`
  rewrites only those cases (and lists them as rendered on this platform); `ESSES_UPDATE_GOLDEN_JSON=1` rewrites the
  case JSONs only.
- `images/tst_golden_plots.cpp` (M3, AC-M3-02, D-198): three cases each of `scatter`, `histogram`, `outing`,
  `gauge_numeric` and `gauge_bar` (`libs/plotcore/tests/plots_golden_cases.hpp`), rendered through the whole
  builders (the binners and TrendFit included) at DPR 1 and 2 into `images/<type>/<case>@<dpr>x.png` with
  `<case>.json` and one `goldens.json` per directory. The PNGs committed with that work are Windows renders
  (`"generatedOn": "windows"`, compared in report mode) until they are replaced from the ubuntu artefact
  (`<build>/tests/golden/artefacts/plots/`). The gauge cases are also `DrawList` JSON goldens
  (`drawlist/gauge_numeric`, `drawlist/gauge_bar`).
- `libs/widgets/tests/components/reports_track/tst_reports_track_goldens.cpp` (M3, AC-M3-02, D-199; a widgets
  test, ctest `widgets.tst_reports_track_goldens`, because the report tables render through `QWidget::render`):
  three cases each of `channel_report`, `time_report` and `track_position` at DPR 1 and 2 into
  `images/<type>/<case>@<dpr>x.png` with one `goldens.json` per directory (ubuntu renders, from
  `<build>/libs/widgets/artefacts/<type>/` of the CI artefact); exact goldens beside them: the report tables' contents in
  `tables/<type>/<case>.json` and the track map's `DrawList` JSON in
  `drawlist/track_position/widget_<case>@<dpr>x.json`.
- `libs/widgets/tests/components/tst_values_list_goldens.cpp` (M3, AC-M3-02, D-202; ctest
  `widgets.tst_values_list_goldens`): three `values_list` cases (`main_reference`, `main_only_fixed`,
  `corr_speed_delta`) at DPR 1 and 2, painted by `QWidget::render` like the report tables, into
  `images/values_list/<case>@<dpr>x.png` with one `goldens.json` (ubuntu renders, from
  `<build>/libs/widgets/artefacts/values_list/` of the CI artefact) and exact table goldens `tables/values_list/<case>.json`
  (headers, hidden columns, cell text), with cells also computed by hand from the synthetic source.
- M3 key colours (D-202): eleven `DrawList` goldens with overlays (the time_distance `bands_sections`,
  `dark_full_anatomy`, `offset_axis_*` and `variance_distance` cases, `scatter_overlay_trend`, `histogram_bars`,
  `track_fit`, `track_manual`, `numeric_background_fixed_font`, `bar_custom_origin_dark`) changed in colour only
  when the overlays took their range key colours (06 §5.3 item 3); their provisional PNGs were re-rendered.
- `fonts/`: the only fonts the renderer may use (Noto Sans, OFL-1.1, REUSE-annotated), registered with
  `SchemeFonts::registerApplicationFonts` before any `QtTextMetrics` exists.

Comparison: a pixel differs when any channel differs by more than 8/255; an image fails above 0.5 % differing
pixels; every render and each failing diff image is written to `<build>/tests/golden/artefacts/` (the CI
artefact). Pixel goldens are authoritative on ubuntu only (D-127, 08 §6.4): the comparison fails the test on Linux
when `goldens.json` says `"generatedOn": "linux"`, and anywhere with `ESSES_PIXEL_GOLDENS_STRICT=1`; elsewhere
(Windows and macOS, or Linux while the PNGs are provisional) each case is a render smoke test (device size, DPR
tag, ink, two renders identical) and the pixel difference is reported, not enforced. Every OS also checks
`<case>.json` against the case definition and runs the `DrawList` JSON goldens.

Provenance (M1): the committed PNGs come from the ubuntu `qt-app` artefact of CI run 38290822661 (Qt 6.12.0, offscreen, commit dc335d6) and `goldens.json` says `"generatedOn": "linux"`, so the ubuntu job enforces them; Windows and macOS run them as render smoke tests. They replaced provisional Windows renders, which differed by about 1 % of pixels (font rasterisation). To update them: take the renders from a later ubuntu run's `golden-diffs-ubuntu-24.04` artefact.

Provenance (M3): every M3 PNG (plots, report, track, values_list, zoom summary and the thirteen M3
`time_distance` cases, plus the M1 `dark_scheme` the M3 key columns changed) comes from the ubuntu `qt-app`
artefact of CI run 39104943947 (Qt 6.12.0, offscreen, commit 811f442); they replaced provisional Windows renders (D-203 (ae)).
Regenerate for review only (any OS): `ESSES_UPDATE_GOLDENS=1 <build>/bin/esses_golden_tst_golden_images`.