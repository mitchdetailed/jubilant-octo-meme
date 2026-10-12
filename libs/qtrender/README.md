# esses_qtrender

Layer 4, Qt Gui (01 §2.1, 04 §6; D-025, D-033, D-058, D-127, D-137). The raster renderer behind the plot core's
`IRenderBackend` seam, the per-component layer cache, the offscreen renderer (goldens, `esses-render`), the
scheme fonts and thread-safe text metrics, and the golden pixel comparison. Built only with `ESSES_BUILD_QT=ON`.

Namespace `esses::qt` (01 §1, shared with `esses_qtmodel`); public headers in `include/esses/qtrender/`
(umbrella `qtrender.hpp`). Links `esses_plotcore` (plus `esses_project`, `esses_model`, `esses_store`,
`esses_units`, `esses_base` for the types it names) and Qt Gui. Never links `esses_qtmodel` (forbidden edge) or
Qt Widgets (`cmake/qt-free-check.cmake` rule 4). Compiled with `QT_NO_KEYWORDS` (PUBLIC): write `Q_SIGNALS`,
`Q_SLOTS`, `Q_EMIT`. Sources (`src/**`, `include/**`) are globbed with AUTOMOC; every `tests/**/tst_<name>.cpp` is
one Qt Test executable (`ctest -R qtrender.tst_`), run offscreen; `tests/qtrender_test_support.cpp` is compiled
into each.

| Header | Contents | Design |
|---|---|---|
| `scheme_fonts.hpp` | `SchemeFonts` (`font(TextStyleId, rotated)`, `role(name)`, `roleName`, `id`, `registerApplicationFonts`) | 04 §6.1 item 4, 06 §16 |
| `qt_text_metrics.hpp` | `QtTextMetrics : plot::ITextMetrics` (`FontDescription`, `create`) | 04 §5.4 |
| `raster_backend.hpp` | `RasterBackend : plot::IRenderBackend` (free-list, `image`, static `paintList`) | 04 §6.1 |
| `layer_cache.hpp` | `LayerCache` (`resize`, `invalidate`, `dirty`, `storeStatic`, `rasteriseStatic`, `setDynamic`, `setChrome`, `dynamicDamage`, `paint`, `composite`, `releaseStatic`, `bytes`, `deviceSize`) | 04 §6.2, D-137 |
| `offscreen_renderer.hpp` | `ComponentBuilders`, `OffscreenRenderer` (`InputsProvider`, `render`, `renderComponent`, `setBuilders`, `builtinBuilders`) | 04 §6.7 |
| `image_diff.hpp` | `ImageDiff`, `compareImages` (8/255 per channel, fail above 0.5 %) | 04 §6.7 item 4, 08 §6.4 |

Internal (`src/`): `font_cache.*` (the one QFont construction rule, the style -> role table, per-thread font and
`QStaticText` caches), `paint_internal.hpp` (text placement, command bounds and `paintStaticList`, shared by
`RasterBackend` and `LayerCache`), `span_raster.*` (the Static-layer line rasteriser).

## Behaviour notes

- **Coordinates and DPR.** `RasterBackend::paintList` maps a list's device pixels onto the painter's logical
  coordinates (`scale(1 / list.dpr)`), so one call paints a DPR-tagged `QImage` layer and a widget of that DPR 1:1
  in device pixels. Text is laid out with the logical font under the painter's DPR scale; `QtTextMetrics` returns
  the logical `QFontMetricsF` values times `dpr`, so builder measurements and rasterised runs agree at every DPR.
- **Commands.** Width <= 1 px strokes are aliased with each vertex snapped to the device pixel containing it
  (consecutive duplicates collapse); wider strokes are antialiased; polylines break at non-finite vertices.
  Through `paintList` (Dynamic and Chrome lists, any painter) strokes are QPainter polylines in slices of <= 4,096
  points (flat caps, round joins). `FilledRect`/axis-aligned `Quads` round their
  edges to device pixels. `Scissor` intersects with the painter's clip at entry (`w = 0` restores it); `Clear`
  uses `CompositionMode_Source` over the list's layer rectangle. Text anchors name a point of the box
  advance x (ascent + descent); `quarterTurns = 1` rotates -90 degrees about the anchor; a non-zero `background`
  fills that box first.
- **Static-layer line rasteriser.** `RasterBackend::draw` and `LayerCache::rasteriseStatic` (every Static layer,
  widget or offscreen) write solid strokes up to 8 device px straight into the `Format_ARGB32_Premultiplied`
  pixels (`span_raster.hpp`): Bresenham 1 px lines with merged vertical runs, and per-row (steep) or per-column
  (shallow) coverage runs for wider strokes; every other command still goes through QPainter, in list order.
  Reason (04 §6.5 item 3 remedies): QPainter strokes a dense M4 polyline as one path whose outline crosses every
  scanline of the trace. Measured on the development machine (RelWithDebInfo, noisy M4 data, 20 traces, Static
  raster only, clear included): QPainter 101 ms at 3,800 px DPR 1 and 6.8 s at 7,600 px DPR 2 (antialiased);
  this rasteriser 9 ms and 82 ms. A Dynamic repaint (cursor + 40 key values, damage-clipped, 3,800 x 1,000)
  takes 1.4 ms. `tst_raster_backend` checks that both paths draw the same picture (< 2 % pixels at a 48/255
  tolerance, identical ink bounds). Dashed strokes, strokes wider than 8 px, Points, Quads, rectangles and text
  always use QPainter.
- **Worker rasterisation in strips (M1 integration, AC-M1-02).** `RasterBackend::rasteriseStatic(list, fonts,
  clear, pool)` is the Static raster of the widgets' worker build (04 §6.3 step 2 "worker build + raster"): on a
  TaskPool worker it measures the columns every command reaches (in parallel slices: `commandBounds`, and whether
  a polyline is x-monotonic), then paints up to 32 vertical strips (twice the workers, edges on whole logical
  pixels and 16-pixel cache-line multiples) as sub-tasks joined with Rule W. Each strip paints a `QImage` over
  the whole shared buffer clipped to its columns, in exactly the coordinates of one pass, plus the direct
  rasteriser clipped to them; it paints, in list order, only the commands that reach it (a text run counts one
  line height either side of its box, since glyph ink can pass the advance box), and walks only the part of an
  opaque x-monotonic polyline that reaches it (binary search; translucent or gapped polylines are walked whole,
  clipped). The picture equals one `LayerCache::rasteriseStatic` pass pixel for pixel (`tst_static_strips`,
  tolerance 0, and `tests/ui/tst_render_parity`, where esses-render still paints in one pass). A strip is never
  a translated view of its columns: `QPainter::drawStaticText` truncates a run's device position to 26.6 fixed
  point toward zero, so a run starting left of a translated origin moved 1/64 px right and, where glyphs are
  positioned at subpixel offsets (FreeType with fontconfig's slight hinting on Linux, CoreText; not Windows'
  fully hinted FreeType), took another glyph image. `tst_static_strips` checks text across every strip edge
  with the application font's hinting preference set to none, which gives subpixel glyph positions on every
  OS. A non-integer DPR, a NoBlock thread or no pool paint one strip. Buffers: `LayerCache::storeStatic` hands
  the replaced image back when it has the same size (at most two process-wide spare buffers, 04 §6.5 item 3
  "QImage reuse"); `rasteriseStatic` takes one of the list's size before allocating. Measured on the reference
  machine (RelWithDebInfo, 20 noisy 1 kHz traces, 3,800 px trace area, DPR 1): 16.3 ms in one pass, about 4.4 ms
  in strips with reuse; the zoom/pan step of `bench/plot_bench` went from 27.9 to 13.0 ms (with the parallel
  emit of `esses_plotcore`).
- **LayerCache.** The Static image survives `resize`/`invalidate` (the previous frame stays on screen until the
  rebuild lands); `rasteriseStatic` reuses its buffer at constant size (and skips the initial fill when the list starts with a
  Clear of the layer). `dynamicDamage` is the union of the
  command bounds of both lists (strokes widened by w + 2 device px) in logical pixels: the old and new cursor
  strips plus the key-value column. `paint` clips to the exposed region, blits the Static image, then paints the
  Dynamic and Chrome lists; `composite()` is that paint into a DPR-tagged image (Print to Clipboard of the current
  layers, 04 §7.2 item 9, and `OffscreenRenderer`).
- **OffscreenRenderer.** Static builds go to the given pool, else `TaskPool::installed()`, else run inline;
  every Static build of a worksheet is submitted before the first wait. Each component is composed by a
  `LayerCache` exactly as a widget paints, so `esses-render` equals the on-screen composite (AC-M1-05).
- **Thread-local caches.** No `QFont` crosses threads (04 §5.4). The application thread's caches are cleared by a
  `qAddPostRoutine` hook so no `QFont` outlives the `QGuiApplication`.

## Fonts used by the tests (D-127)

`tests/golden/fonts/` holds **Noto Sans 2.015 Regular and Bold** (OFL-1.1, copyright 2022 The Noto Project
Authors), copied unmodified from the local Windows font directory (no download); `tests/golden/fonts/OFL.txt` and
`LICENSES/OFL-1.1.txt` carry the licence, `REUSE.toml` annotates the directory `OFL-1.1`, `NOTICES.md` lists it as a
test-only asset. Weight 600 roles resolve to Bold. The Qt Tests set `QT_QPA_FONTDIR` to that directory in
`initMain()` (on Windows the offscreen platform's FreeType font database then sees only Noto Sans) and register it
with `SchemeFonts::registerApplicationFonts`. Text is asserted geometrically (ink bounds against `QtTextMetrics`);
the pixel references in `tests/reference/` are geometry-only scenes and therefore valid on every OS.

## Tests

| Test | Covers |
|---|---|
| `tst_raster_backend` | layer life cycle and free-list; every command at DPR 1 and 2 (exact pixels); slices, gaps, dashes; text anchors, elision, background, rotation, DPR scaling against `QtTextMetrics`; compose order; straight-alpha readback; Static rasteriser == QPainter picture, single blend of translucent strokes, scissor clipping of huge coordinates; `geometry_scene@{1,1.5,2}x.png` references |
| `tst_static_strips` | `RasterBackend::rasteriseStatic` in strips == one `LayerCache::rasteriseStatic` pass (tolerance 0): geometry scene, split traces and text at DPR 1, 2 and 1.5, text across every strip edge at subpixel glyph positions, the eight `time_distance` golden cases; clear colour; one strip on a NoBlock thread; spare-buffer reuse |
| `tst_layer_cache` | the ChangeMask table of 04 §2.2 bit by bit (recomposition only on the right masks); resize; buffer reuse; damage = cursor strips + key column at DPR 1 and 2; `paint` == `RasterBackend::compose`; exposed-region clipping |
| `tst_text_metrics_concurrency` | 8 threads measuring while the GUI thread swaps schemes (200 snapshots); per-snapshot caches; metrics == `SchemeFonts` fonts |
| `tst_scheme_fonts` | style -> role table, QFont attributes, caption fallback, golden font registration |
| `tst_offscreen_renderer` | Static on the pool and Dynamic/Chrome on the caller; output == `LayerCache` composite (DPR 1, 2); determinism; worksheet layout by rect and z; placeholder and error rendering; the shipped `circuit-default` workbook with the light and dark schemes |
| `tst_image_diff` | the 8/255 and 0.5 % rule, size mismatch, straight-alpha comparison, diff image |

References: `tests/reference/<name>.png`; after a reviewed rendering change rerun with `ESSES_UPDATE_REFERENCES=1`.
Every rendered image (and `*.diff.png` on failure) is written to `<build>/libs/qtrender/artefacts/`.

## Deviations from the design text

- `OffscreenRenderer::render(workbook, worksheet, InputsProvider, size, dpr)` and `renderComponent(const
  plot::ComponentBuildInputs&, size, dpr)` replace 04 §6.7's `(WorkbookFile, worksheet, sources, selection, view,
  size, dpr, scheme)`: resolving maths outputs needs `esses_qtmodel`, which this library may not link; callers
  prepare the inputs with `qt::RenderService::prepareInputs`. Builders are chosen per component type
  (`setBuilders` overrides; `time_distance` -> `plot::TimeDistanceBuilder`; every other type renders a
  placeholder until its Qt-free builder exists, e.g. `values_list`).
- `RasterBackend` takes its `SchemeFonts` and `ITextMetrics` snapshot in the constructor; `image(handle)` exposes
  a layer as a `QImage`. A layer begun while another is open releases the open one.
- `LayerCache` takes the `SchemeFonts` it paints text with and adds `setFonts`, `rasteriseStatic` (the one-pass
  Static path of `OffscreenRenderer` and the widgets' fallback), `releaseStatic` (the 30 s hidden worksheet rule's
  release), `deviceSize` and accessors
  for tests. The Static image is kept, not dropped, on `resize` (04 §1 item 3 "keeps showing its previous Static
  layer").
- 04 §6.1 item 3 draws every `Polyline` with `drawPolyline`; Static layers use the direct rasteriser above for
  solid strokes (performance; the picture is the same). Translucent strokes may double-blend where one polyline
  crosses itself (QPainter blends a self-crossing path once).
- `QtTextMetrics` caches per (snapshot id, style) instead of (generation, style, dpr): the device value is the
  logical metric times `dpr`, and a process-unique id keeps two snapshots that share a generation apart.
- Added `image_diff.hpp` (the comparison rule of 04 §6.7 item 4) for the goldens (`tests/golden`) and the render
  parity test (`tests/ui/tst_render_parity`).
