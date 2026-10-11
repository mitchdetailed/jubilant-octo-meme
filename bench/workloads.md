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
