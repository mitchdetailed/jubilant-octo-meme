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

AC-M2-10 (07 §12: "20 maths channels of `bench/workloads.md` on RW-2 ready after request <= 2 frames"; D-042,
D-139). `bench/maths_bench.cpp` builds RW-2 in memory: 30 min, 8 channels at 1 kHz (`Wheel Speed FL/FR/RL/RR` in
m/s, `Damper Pos FL/FR/RL/RR` in mm), 32 at 500 Hz (`Engine RPM`, `Throttle Pos`, `Brake Pres Front`, `Brake Pres
Rear`, `Steered Angle`, `G Force Lat/Long/Vert` and 24 `Ch500 <n>`), 60 at 100 Hz (`Gear`, `Ground Speed` in m/s,
`Oil Temp`, `Water Temp` and 56 `Ch100 <n>`) and 100 at 20 Hz (`Ch20 <n>`), smooth periodic F32 signals with a little
ripple, one outing and 90 s laps (`Outings:Laps`). The 20 expressions are one global module of a fresh `MathGraph`:

| Class | Output | Expression |
|---|---|---|
| Elementwise | Wheel Speed Front | `('Wheel Speed FL' + 'Wheel Speed FR') / 2` |
| Elementwise | Brake Bias | `'Brake Pres Front' / max('Brake Pres Front' + 'Brake Pres Rear', 1) * 100` |
| Elementwise | Combined G | `hypot('G Force Lat', 'G Force Long')` |
| Elementwise | Full Throttle | `choose('Throttle Pos' > 95, 1, 0)` |
| Elementwise | Rear Slip | `('Wheel Speed RL' + 'Wheel Speed RR') / max('Wheel Speed FL' + 'Wheel Speed FR', 1) - 1` |
| Windowed | Damper Vel FL | `derivative(smooth('Damper Pos FL', 5))` |
| Windowed | Ground Speed Smooth | `smooth('Ground Speed', 0.5)` |
| Windowed | Engine RPM Smooth | `smooth('Engine RPM', 25)` |
| Windowed | Steer Rate | `derivative('Steered Angle')` |
| IIR | G Lat Filtered | `filter_lp('G Force Lat', 10, 2)` |
| IIR | G Long Zero Phase | `filter_lp('G Force Long', 5, -2)` |
| IIR | Damper FR HP | `filter_hp('Damper Pos FR', 1, 2)` |
| IIR | G Vert Cheby | `filter_cheby_lp('G Force Vert', 20, 4, 0.5)` |
| Stateful | Distance | `integrate('Ground Speed')` |
| Stateful | Lap Max Oil Temp | `running_max('Oil Temp', 'Gear' > 0, range_change("Outings:Laps"))` |
| Stateful | Brake Latch | `flip_flop('Brake Pres Front' > 20, 'Brake Pres Front' < 2)` |
| Stateful | Throttle Shifted | `time_shift('Throttle Pos', 0.1)` |
| Stateful | Lap Max Speed | `stat_max('Ground Speed', 'Gear' > 0, range_change("Outings:Laps"))` |
| Range | On Lap | `range_is("Outings:Laps")` |
| Range | Lap Number | `range_change("Outings:Laps")` |

Rows of `esses-maths-bench` (one untimed run first, so RW-2's columns are resident as after an open; then `--runs`
runs, each on a fresh graph):

| Row | Measures | Measured | Limit |
|---|---|---|---|
| `maths.rw2_20_ready` | from the first `require()` of the 20 outputs to the last `Future` ready, on the default `TaskPool` (one task per node, parallel across nodes, D-139; each node streams its plan and idle workers seal its later chunks, D-191); the logical processor count and the workers are part of the row | **28.3 ms** (16 logical processors on 8 cores, 15 workers) | 33.3 ms (2 frames at 60 Hz): met (D-191) |
| `maths.rw2_20_define` | `setModules()` of the 20 items (parse, resolve, type check, compile; owning thread) | 0.60 ms | recorded |
| `maths.filter_lp_zero_phase_21600k` | `evaluateWhole` of `filter_lp('X', 5, -2)` over 21.6 M samples (6 h at 1 kHz), one core | 224.8 ms | recorded (150 ms from v1.0) |
| `analysis.marker_rebuild_60lap` | AC-M2-05: `DataSource::withSidecar` with the circuit rules after a user beacon inserted in lap 10 and after an offset edit of beacon 20, on a 60-lap outing of 200 channels, one core (both rebuilds per run) | 0.11 ms | 5 ms |

Measured on the reference machine above (2026-10-13, `win-msvc-bench` Release, after D-191), median of 15 (5 for
the filter, 30 rebuilds), with the maintainer's desktop applications open: three batches of 15 gave medians of
28.3, 28.5 and 29.0 ms for `maths.rw2_20_ready` (26.9-32.0 ms over all 45 runs; 25.3-26.6 ms in earlier batches with
less background load), against 124.4 and 127.0 ms for the build before D-191 run alternately with it (113-120 ms
when it was measured first). `--nodes` prints each output's time when required alone: 0.7-13.1 ms, 79.7 ms in all
(402 ms before D-191), the slowest `filter_hp('Damper Pos FR', 1, 2)` over 1.8 M samples (its two RC passes are one
sequential recursion each); `--workers 1` gives the serial cost of the set, 175 ms, so the set runs 6.2 times faster
on 15 workers of 8 cores. Where the serial time goes (per kernel, the store's seal of the output chunks, the chunk
writes): `docs/milestones/M2.md` §6 open issue 5. `--digest` prints a fingerprint of every result instead of timing
(two builds that compute the same results print the same lines).

```
build\<bench dir>\bin\esses-maths-bench --runs 7 --gate --json maths.json
build\<bench dir>\bin\esses-maths-bench --runs 1 --nodes
build\<bench dir>\bin\esses-maths-bench --runs 7 --workers 1
build\<bench dir>\bin\esses-maths-bench --digest > digest.txt
```

## M2 text-import rows (AC-M2-13)

`bench/formats_bench.cpp` (executable `esses-formats-bench`, Qt-free, `std::chrono`) writes on first use, into
`<build>/bench-data/`, two deterministic 300 MB files of 100 channels at 100 Hz (two decimals, smooth signals,
units from a fixed list): one in the legacy CSV layout (`motec.csv` profile: quoted key/value preamble, names and
units rows, every field quoted, CRLF; 335,362 rows) and one plain table (generic profile: names and units rows,
LF). Each run has a fresh chunk cache and task pool (all logical processors); the files are warm after an untimed
first run. `--smoke` uses 2 MB files and one run (ctest `bench.formats_bench.smoke`, label `bench-smoke`);
`--gate` exits 1 above a limit; `ESSES_BENCH_REPETITIONS` and `ESSES_BENCH_OUT` as for the other benches.

| Row | Workload | Measured | Limit |
|---|---|---|---|
| `formats.csv_probe_300mb_100col` | `ImporterRegistry::probe` of the legacy file (pass 1: sniff, preamble, header, row scan, classification, rate) | **166 ms** | 2,000 ms (05 §1.3 rule 2) |
| `formats.csv_first_data_300mb_100col` | `ImporterRegistry::open` (units to base, whole-outing ranges) of the legacy file, then every chunk of 8 channels resident: all their chunks prefetched at Visible, as a graph's first build asks, then waited for | **1,553 ms** | 10,000 ms (07 §12, first paint) |
| `formats.csv_generic_first_data_300mb_100col` | the same through the generic profile on the plain table (pass 1 includes the full scan of sparse "timestamped") | **2,429 ms** | 10,000 ms |

Measured on the reference machine (2026-10-12, `win-msvc-core-rel`, 16 logical processors, median of 5): the
legacy open takes about 110 ms, the generic open about 860 ms; the rest is the slab decode of all 100 columns (6
slabs decoded in parallel, siblings sealed in the same sweep). "First data" is the formats share of the 07 §12
first-paint budget; the graph's build and raster of eight traces adds about 90 ms (`open.first_paint_100mb_8ch`
below, 8 x 3 M samples). With one worker the legacy first data takes 3.8 s (about 80 MB/s of quoted text per
thread).

Since D-190 (2026-10-13, `win-msvc-bench`, the same machine, median of 5) both passes read the file through guarded
4 MiB copies instead of the mapping itself (a file truncated under the mapping fails a chunk instead of the process)
and keep copies of the sampled rows: probe **230.5 ms**, first data **1,648.9 ms** legacy and **2,756.3 ms** generic
(legacy open about 175 ms, generic open about 1,180 ms), all within their limits.

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
