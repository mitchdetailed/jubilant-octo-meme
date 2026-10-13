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
- `images/time_distance/goldens.json`: where the PNGs were rendered (`generatedOn`, Qt version, platform).
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
Regenerate for review only (any OS): `ESSES_UPDATE_GOLDENS=1 <build>/bin/esses_golden_tst_golden_images`.