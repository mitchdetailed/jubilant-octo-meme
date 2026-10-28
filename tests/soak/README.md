# The Saturday soak (M4, AC-M4-03)

`tst_saturday.cpp` is the soak of 07 §7.1 ("Soak") and AC-M4-03 (D-213): the real shell, offscreen, with ten logs
open and the race engineer's day in a loop, failing on a crash, a `Dispatcher` thread violation, pending work that
never drains, unbounded growth, and (in the soak job) a leak with an Esses frame. It is ctest `soak.tst_saturday`
(labels `qt;soak`: the `saturday` function alone, left out of the normal runs with `ctest -LE soak`, D-216 (b)),
added from the top-level `CMakeLists.txt` with the Qt layers; `soak.guard` (label `qt`, in every run) runs the
guard's and the growth rule's own tests, and `soak.lsan_check.self_test` (label `lint`) checks the leak classifier
and `lsan.supp`.

| File | What |
|---|---|
| `tst_saturday.cpp` | The Qt Test: the guard's rules by hand (`growthRule`, `threadViolationMessages`), a recorded violation of each kind in-process (`guardRecordsDispatcherViolations`), the fatal path in a child process (`violationsAreFatal`), the soak (`saturday`), and the child itself (`selfTestChild`, skipped without `ESSES_SOAK_SELFTEST`) |
| `soak_data.*` | The ten logs, written at test time and never committed (once per build tree into `<build>/tests/soak/saturday-logs-v<n>-x<scale>`, on a pool of their own, then copied into the run's temporary folder): eight sessions of a circuit day as `.ld` + `.ldx` (two venues, out lap, flying laps, in lap, beacons in the `.ldx`, the header's details; 41 channels at 10 to 200 Hz, GPS at 20 Hz; about 1,000 s and 9 MB each at scale 1) and two CSV exports of another logger (16 channels and a lap counter at 20 Hz, one per CSV writer, about 3 MB each); two sessions also get a second version with one more lap for the reload rotation |
| `soak_driver.*` | The day, one cycle at a time (below) |
| `soak_guard.*` | What a thread violation is and what happens on one: every `ESSES_ASSERT` (the `Dispatcher`'s GUI-thread check, the GUI thread's `NoBlock` checks) through `setAssertHandler`, and Qt's thread-affinity warnings through the message handler; fatal in the soak (`soak: fatal <class>: <text>` on stderr, then `abort()`), recorded in the guard's own tests. Also the MSVC debug-CRT heap checks |
| `soak_metrics.*` | The process's resident set and heap (the sanitizer allocator, the MSVC debug CRT, glibc's `mallinfo2`, macOS's malloc zones, else Windows's private bytes) and the growth rule |
| `lsan.supp` | LeakSanitizer suppressions for the Qt, fontconfig and glib stacks, under the rule of its header |
| `lsan_check.py` | The soak job's verdict over the sanitizer logs, the Qt Test result and the exit status, and the rule check of `lsan.supp` (`--self-test`, `--check-suppressions`; standard library only, `python -I`) |

## One cycle

1. Cycles 1, 5, 9, ...: restart. Save the project, close the window (exit), open a new window on the project; the
   session restore reopens the previous data (the project's recent sources: every log opened in it, so at least
   those open at the exit) and the selection (Main as before).
2. Open what is not open, so ten logs are. One open goes through the memory-budget gate (the cache's budget set to
   1 MiB, below every log's own estimate, while the pool is idle): Continue on cycles 0, 3, 6, ...; Cancel and a
   second open on cycles 1, 4, 7, .... When everything is open (after a restart), the last log is closed first.
3. Settle, worksheet 0, its components drawn, settle: the cycle's sample.
4. Selection: Main, Overlay 0, an overlay and the CSV's outing, from the Data window's keys (even cycles) or the
   selection model (odd cycles); N, P, Q twice, J twice, F4 twice; a lap-number filter set and cleared; CTRL+F.
5. Every worksheet, by `selectWorksheet` or F12: every component drawn; on a time/distance graph UP, SHIFT+RIGHT,
   RIGHT, CTRL+RIGHT, D, SPACE, X, H, F3, M, END, HOME, W, F, B, F2 (F3, M, D twice), a wheel step each way, a click,
   F9 twice on even cycles; the reports filled and copied as TSV. Then the Track Map dock shown, fitted and drawn,
   and hidden; the Values and Channels docks toggled.
6. Edits: a maths module of two items (global maths) and its channel on the graph, evaluated and drawn; the
   expression changed and recalculated; a lap's trust off and on; the Maths Editor every third cycle. Undo to the
   cycle's start, redo all, the journal written by autosave (every 250 ms here), undo again, save (the journal
   cleared).
7. Reload: one `.ld` replaced on disk by a rename-over writer (its other version, or the same bytes as a newer
   file), the application activated (stale), the reload from the Messages row (even cycles) or File > Reload File
   with the source id (odd cycles).
8. Close three logs (the Data window's DELETE, the loader) or, on cycles 1, 6, 11, ..., Close All; clear Messages;
   settle.
At the end every log is closed and the window closed; the chunk cache must then hold no byte. Every wait is on a
state with a generous timeout (`ESSES_SOAK_SETTLE_SECONDS`, default 900), never a fixed time.

## What fails the soak

- A crash, an `ESSES_ASSERT` or a thread violation (fatal at once, with the offending thread's stack under ASan).
- A step that cannot finish: pending tasks or Dispatcher closures that never drain, an open, reload or restore
  that never completes.
- Growth, judged per cycle in the state of step 3 after a warm-up of a fifth of the run (at least two cycles): the
  smallest of the last third above the largest of the first third by more than max(absolute, relative) slack. Heap
  48 MiB / 10 %, resident set 192 MiB / 25 %, chunk cache 64 MiB / 10 %, widgets 40 / 5 %, the window's objects
  80 / 5 %, peak queued tasks and peak Dispatcher closures 2,000 / 200 %. Cycles that started with a restart are
  left out of the heap, resident and object series (a new window has not made its on-demand dialogs yet). Fewer
  than warm-up + 3 samples are reported, not judged (the 60 s ctest run).
- A chunk cache that still holds bytes or pins once every log is closed and the window is gone.
- In the soak job: an ASan or UBSan error, and a LeakSanitizer report with an Esses frame (`lsan_check.py`: a
  function in namespace `esses`, a source file under `libs/` or `apps/`, an unsymbolized frame of an `esses*`
  executable or library).

## Running it

```
ctest --test-dir build/<dir> -L soak --output-on-failure            # the soak, 60 s (two cycles at least)
ctest --test-dir build/<dir> -R "^soak\." --output-on-failure        # with the guard's tests and the self-test
build/<dir>/bin/esses_soak_tst_saturday saturday -o soak.txt,txt      # the soak alone
```
Environment: `ESSES_SOAK_SECONDS` (default 60), `ESSES_SOAK_MIN_CYCLES` (2), `ESSES_SOAK_SCALE` (session length,
1), `ESSES_SOAK_SETTLE_SECONDS` (900), `ESSES_SOAK_LOG_DIR` (writes `saturday-cycles.csv`, one row per cycle, and
`saturday-summary.txt`), `ESSES_SOAK_CRT_CHECK=<n>` (MSVC debug builds: a full heap check every n allocations,
1, 16, 128 or 1024, and `_CrtCheckMemory()` after every cycle). ctest gives the entry 1,800 s; a longer run calls
the executable directly. `initMain` raises Qt Test's per-function watchdog (`QTEST_FUNCTION_TIMEOUT`) above the
soak's duration. Every step prints `soak <s> s cycle <k>: <step>` on stderr (the Qt Test log is its own file on
Windows). A cycle takes about 12 s in a Clang Debug build against Qt's release libraries and about 135 s in an MSVC
Debug build against Qt's debug libraries (the reference machine, 16 workers), so ctest's run is about 70 s and 5.5
minutes there (the first writing of the logs included). That is why it is not in the normal runs: ci.yml's qt-app
runs `ctest -LE soak` (the guard's tests and the self-test stay in), and the soak itself is the soak job's.

The soak job (`.github/workflows/bench.yml`, job `soak`): ubuntu-24.04, the `linux-clang-asan` preset with the Qt
layers (Clang 18, Debug, ASan + UBSan, assertions), Qt 6.12.0 from ci.yml's aqtinstall commit; nightly for 30
minutes and on manual dispatch with `soak_minutes` (120 on the release candidate, `docs/milestones/M4.md` §6.5).
It runs `lsan_check.py --self-test`, then the leak pipeline's own check (`ESSES_SOAK_SELFTEST=leak`: an allocation
in `esses::soak::leakOnPurpose`, made inside a Qt timer's callback, must be reported through `lsan.supp` and
counted), then the soak, then `lsan_check.py` over the logs, which prints the first ASan or UBSan report in the job
log (whole, from its rule to its `SUMMARY:` line, up to 200 lines); the logs, `saturday-cycles.csv` and the summary
are the artefact `soak-logs`. A leak without an Esses frame is a warning naming its library: add it to `lsan.supp`
only under the rule of its header (the self-test refuses anything else).
