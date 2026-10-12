# Reference workloads and the reference machine

Status: M0. This page is the home that `ARCHITECTURE_OUTLINE.md` §8 and `07_ROADMAP.md` §12 name for the
reference workloads RW-1 … RW-5, the reference machine, the budgets each milestone gates and (from M2) the 20
maths expressions of the readiness row. Budgets are absolute limits, checked as the median of at least 5 runs on
the reference machine (D-042); GitHub-hosted runners only run them as a non-blocking ±30 % regression alarm.

## Reference machine

The developer's machine, registered for M0 by hand (the self-hosted runner arrives with `bench/` sources in M1):

| Item | Value |
|---|---|
| CPU | AMD Ryzen 7 3800X, 8 cores, 16 logical processors |
| RAM | 64 GB |
| OS | Windows 11 Pro 10.0.26300 |
| Toolchain | MSVC 14.44 (Visual Studio 2022), Ninja, CMake 4.4, RelWithDebInfo (`win-msvc-core-rel`) |

## Reference workloads

Sized from `docs/design/background/design-panel/performance-first.md` §1.2. Rates are per channel.

| ID | Workload | Channels and rates | Samples | Raw F32 | Notes |
|---|---|---|---|---|---|
| RW-1 "Endurance" | 6 h log | 8 × 1 kHz, 32 × 500 Hz, 60 × 100 Hz, 150 × 20 Hz (250 ch) | 713 M | 2.85 GB | Larger than RAM with pyramids (≈ 4.8 GB); works through the chunk cache with eviction (gates from M5, D-043) |
| RW-2 "Sprint" | 30 min | 8 × 1 kHz, 32 × 500 Hz, 60 × 100 Hz, 100 × 20 Hz (200 ch) | 57.6 M | 230 MB | Fully resident: 230 MB raw + 154 MB pyramids |
| RW-3 "Sim" | 1 h ACC-style export | 80 × 60 Hz | 17 M | 69 MB | Must stay instant on an iGPU laptop |
| RW-4 "Overlay" | RW-2 with 10 laps × 12 channels on one worksheet of 10 components | 1,200 (lap, channel, component) polylines | — | — | The 60 fps linked-cursor target (M1) |
| RW-5 "Live" (later) | 200 channels appended at 20–1000 Hz for 6 h | append-only | — | — | Append-friendly pyramids, sliding windows |

## M0 workload: probe of the 500 MB synthetic `.ld` (AC-M0-12)

`esses-gen-fixtures --large <file>` writes it; `python -I tools/probe_timing.py --cli <rel>/esses-cli --gen
<rel>/esses-gen-fixtures --file <file>` reports the median of 5 warm runs of `esses-cli info --json --timing`.
Limit 200 ms (D-042). The measured median is recorded in `docs/milestones/M0.md`.

## Maths readiness set (M2)

The 20 maths expressions of AC-M2-10 (a fixed mix of Elementwise, Windowed, IIR, Stateful and Range kernels on
RW-2) are defined here when `bench/maths_bench` lands in M2.

## M1 budget rows (AC-M1-02, AC-M1-03)

`bench/plot_bench.cpp` and `bench/open_bench.cpp` (executables `esses-plot-bench`, `esses-open-bench`; plain
`QElapsedTimer` harnesses because Google Benchmark is not vendored yet, D-147) drive the application's data path
without the shell's docks: `AppRuntime` with the GUI thread marked NoBlock as in `esses`, `SourceLoader` with the
circuit rules, the built-in Corr Speed / Corr Dist, `DistanceService`, `DataSelectionModel`, one
`WorksheetViewModel`, `RenderService` and a `time_distance` graph in a top-level window on the offscreen platform at
DPR 1. Their logs are written on first use by `esses-gen-fixtures --bench` into `<build>/bench-data/`:
float channels at 1 kHz ("Ground Speed" first, so Corr Speed evaluates), triangle signals with deterministic noise
(an M4 column carries a real min/max spread). Each row is the median of the per-run medians of `--runs` runs
(default 5); `--gate` exits 1 above the limit; `--json` writes the rows; `--stages` adds the stage timings of
04 §6.4 (build, raster_static). ctest `bench.plot_bench.smoke` and `bench.open_bench.smoke` (label `bench`) run the
harnesses on small files and check nothing about the budget.

| Row | Workload | Measured | Limit |
|---|---|---|---|
| `plot.cursor_move` | 20 traces (four groups of five) of a 30 min, 20-channel log with 90 s laps, fastest lap Main, trace area 3,800 px at DPR 1: `WorksheetViewModel::setCursor`, the Dynamic rebuild on the GUI thread and a synchronous repaint of its damage (no Static build); 60 moves per run | **1.58 ms** | 4 ms |
| `plot.zoom_pan_step` | the same graph: x0.5 / x2 about the cursor, pan +-10 %, F2 and W (12 steps per run), from the command to the Static build delivered, stored and the widget repainted | **13.1 ms** | 16 ms |
| `open.first_paint_100mb_8ch` | a 100 MB `.ld` (8 channels at 1 kHz, 52 min, no beacons: Main = the whole outing), warm (an untimed open first; a fresh runtime per run, so the chunk cache is cold): `SourceLoader::open` to the first repaint showing all eight traces at full extent | **88.9 ms** | 500 ms |

Measured on the reference machine above (2026-10-11, `win-msvc-qt-rel` RelWithDebInfo, Qt 6.7.2): three
invocations of `esses-plot-bench --runs 5 --stages` gave cursor 1.581 / 1.580 / 1.583 ms and zoom/pan 13.08 /
13.06 / 13.38 ms (per-step p95 about 16 ms, max 19 ms); three of `esses-open-bench --runs 5` gave 91.4 / 88.4 /
88.9 ms (Static build delivered at about 88 ms, published at 3 ms). Zoom/pan step phases (medians): command to
Static build delivered 9.9 ms (GUI preparation 0.6 ms, worker build 5.6 ms of which decimation 2.7 ms, worker
raster in strips about 4.4 ms), storing 0.3 ms, full repaint of the 4,200 x 1,000 window 2.7 ms.

The CI gate builds with the `win-msvc-bench` preset (Release, `ESSES_BUILD_BENCH=ON`; ctest `bench.plot_bench`,
`bench.open_bench`, label `bench`, `--gate`, JSON with a Google-Benchmark-style `benchmarks` array in
`ESSES_BENCH_OUT`); in that configuration on the same machine: cursor 1.53 ms, zoom/pan 11.96 ms, first paint
75.2 ms.

Before the M1 integration the zoom/pan step took 27.9 ms (the worker build 10 ms with a serial trace emission,
then the Static raster on the GUI thread, 14 ms). Three remedies of 04 §6.5 item 3 brought it under the limit
without a GPU backend: per-trace parallel emission (`esses_plotcore`), the Static raster moved into the worker
build as 04 §6.3 step 2 specifies and painted in parallel vertical strips (`RasterBackend::rasteriseStatic`,
pixel-identical to one pass), and Static buffer reuse (`LayerCache::storeStatic` spare buffers).

```
build\m1-integ-rel\bin\esses-plot-bench --runs 5 --gate --json plot.json
build\m1-integ-rel\bin\esses-open-bench --runs 5 --gate --json open.json
```
