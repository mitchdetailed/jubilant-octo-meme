# esses (esses_app)

Layer 5, Qt Widgets (01 §2.1, 06 §§18-19). The GUI executable: the start of an instance. Start-up order (01 §5.3
rule 11): `QApplication` -> the command line -> single-instance forwarding -> `qt::AppRuntime` (pool, cache,
dispatcher; the GUI thread marked `ThreadRole::NoBlock`) -> `widgets::registerBuiltinComponents()` ->
`widgets::MainWindow` -> `--scheme`/`--keymap` -> `-w`/`-l`; after `exec()` the instance's channel closes, the
window is destroyed, then `rt.shutdown()`, then the runtime and the application.

```
esses [-w <project folder>] [-l <log file>]... [--new-instance] [--scheme <name>] [--keymap <name|file>]
      [--offscreen] [--dump-commands] [--version] [--help] [<file>...]
```

- **Files** (`src/launch_request.*`, 06 §19 items 1-3). `-w`/`--workspace` takes the project folder or its
  `project.json` (given twice: the last); `-l`/`--log` is repeatable; every path is made absolute against the
  launch's working directory. A bare argument is the project when it is a folder with `project.json` (or a
  `project.json`) and no project was given yet, otherwise a log. With `-w` the project's recovery prompt comes first
  (a pending journal, AC-M2-06) and the `-l` logs are then opened by the session restore, which otherwise reopens
  the project's previous data and restores the last selection (06 §18 steps 3-6, D-178); without `-w` (or when the
  project does not open) the logs open directly. The MainWindow restores the session's dock layout on start
  (`<config>/layout.json`, then the project's `.local/layout.json` after `-w`) and writes it on exit (06 §§3.4, 18
  step 4).
- **Single instance** (`src/instance_channel.*`, 06 §18 step 1 and §19 item 6; D-113 as built by D-212). A launch
  first hands its files to a running Esses of the same user and settings folder and exits 0; the running one opens
  them (a forwarded `-w` naming the project already open adds the logs to it, another project opens as File > Open
  Project would) and comes to the front. A launch without files just brings the running window forward. Otherwise
  the launch listens itself, so later launches reach it. The channel is the operating system's own local IPC, not
  Qt Network (which the application does not link, 01 §2.2 rule 4): a named pipe `\\.\pipe\<name>` on Windows
  (one instance, local clients only, a DACL that grants the user alone, the client at identification level, both
  sides checking that the other runs as the same user, `AllowSetForegroundWindow` for the running one), a
  Unix-domain socket in a private directory
  (`$XDG_RUNTIME_DIR`, else `$TMPDIR`, else `/tmp/esses-<uid>`, used only when it is ours and closed to group and
  others; the peer's uid checked; the client connects without blocking, within the launch's 20 s wait for an answer,
  `kForwardTimeoutMs`; a starting instance that finds a socket file there tries it for at most 1 s before taking the
  name over) on Linux and macOS; Qt Core's
  `QWinEventNotifier`/`QSocketNotifier` drive it, no thread. The name is `esses-<user>-<12 hex digits of SHA-256 of
  the settings folder and its salt>`: the salt is 32 random hex digits that the first launch writes to
  `instance.salt` in the settings folder (D-217 (h)), so another user cannot compute the name in advance, and a
  portable copy and the installed program, or a run with `ESSES_CONFIG_DIR`, never forward to each other;
  `ESSES_INSTANCE_NAME` replaces it (tests). The message is one line of JSON (`{"protocol": 1, "project": ...,
  "logs": [...]}`, absolute paths only, at most 1 MiB); the answer is `ok` or `error` (a launch whose request is
  refused, or that gets no answer in 20 s, starts a window of its own; it waits once: a launch whose first try found
  an instance that did not answer does not try again). The running instance emits a request only after its `ok`
  reached the client (a client that left gets nothing opened), and `RequestQueue` (`src/launch.*`) applies it when
  the event loop is back at its top level with no modal dialog or popup open (retried every 200 ms), one request at
  a time; a project switch the user cancels opens none of the request's logs (Info row `launch.cancelled`). A launch
  that can neither forward nor listen says so in Messages (Warning row `launch.instance`). `--new-instance` never
  forwards; `--version`, `--help` and `--dump-commands` neither forward nor listen. `--scheme` and `--keymap` are
  options of an instance's start and are not forwarded (a forwarding launch says so on stderr).
- **`ESSES_STARTUP_PROBE`** (bench only, D-217 (k)): set, `esses` prints `esses: ready` and quits once its window is
  shown and the event loop has run once; `esses-open-bench`'s `app.startup_empty_warm` times a start to that line.
- **`--scheme <name>`** (`src/launch.*`): a scheme of the scheme selector by its name, any case, or `light`/`dark`
  for the shipped ones, chosen as the toolbar's selector chooses it (it stays the active scheme). An unknown name is
  a Warning row in Messages (`launch.scheme`).
- **`--keymap <name|file>`**: a `keymap-1` file (an existing path, else `<config>/keymaps/<name>.json`, else
  `<data>/keymaps/<name>.json`) on top of the shipped default keymap, for this run only (the binder's bindings; the
  settings and the user's `keymap.json` are not touched). A problem is a Warning row (`launch.keymap`).
- **`--version`** prints `widgets::versionReport()` (D-212, AC-M4-09): `esses <version>` (the first line keeps its M1
  form), then `commit:`, `Qt: <runtime> (built with <build>)`, `build date:`, `dock library:` and `source:` (the
  tag's URL in a release build, else the commit's).
- `--offscreen` sets `QT_QPA_PLATFORM=offscreen` before the application starts. `--dump-commands` prints the
  `CommandRegistry` as JSON (the registry export that `esses-cli validate` checks keymaps against, 06 §14.3) without
  writing the layout.
- **Portable mode** is `esses::Paths` (D-055): `portable.marker` beside the executable keeps the settings, the
  layout, the cache and the logs beside it; the portable zip ships the marker (`tools/packaging/`). Help > About names
  the settings folder and says when the run is portable.

On Windows `esses` is a GUI-subsystem executable (08 §3.10): started from a terminal it has no console and no
standard handles, so `main()` first attaches to the parent's console (`AttachConsole(ATTACH_PARENT_PROCESS)`) and
reopens the streams that have no handle on it; redirected streams are kept, and started from Explorer nothing
happens (no console window). Unless the run only prints (`--version`, `--help`, `--dump-commands`, a parser
error), it detaches again before the GUI starts, so the terminal's Ctrl+C or closing it cannot end the
application.

Resources (D-212): the window icon (`resources/esses.qrc`, the PNGs of `tools/packaging/icons/`); on Windows
`resources/esses.rc.in` (the icon, which Explorer, the taskbar and the `.ld` association use, and the version
resource, FILEVERSION `major.minor.patch.<commits since the tag>`); on macOS the bundle's `Info.plist`
(`tools/packaging/macos/Info.plist.in`; identifier from the reversed `ESSES_ORG_DOMAIN`, the `.ld` document type)
and `esses.icns`. Install rules: `tools/packaging/CMakeLists.txt`.

Tests: ctest `app.version` (the executable starts offscreen and prints its version) and, on Windows,
`app.version_console` (`tests/console_probe.cpp` runs `esses --version` inside a pseudo console the way a shell does
and requires the version line there); `app.tst_launch_request` (the command line, the forwarded message, the
channel's names, `esses --version` against `versionReport()`), `app.tst_instance_channel` (the channel in process
with its client on a test thread: nobody listening, one listener per name, a stale socket file, a request, a
malformed, an oversized and a silent client; with the built esses: a second launch hands its files over and exits 0
without a window, `--new-instance` runs its own window, a forwarded file opens in a running window, two esses
processes), `app.tst_launch_options` (`--scheme`, `--keymap`, a forwarded request for the open project) and
`app.tst_portable` (settings, layout, cache and logs beside the program, in process and with a copy of the built
esses beside `portable.marker`) and `app.tst_demo_command_line` (D-216: a real `esses --new-instance -w <project> -l
<log>` opens the project, then the log, the command line of the demo of 07 §7.5). A child's `settings.json` is read
through `esses::readWholeFile` (`app_test_support.hpp` `fileText`), never a `QFile`, which on Windows would hold the
name against the child's atomic rename. The tests compile the application sources they exercise and find the built esses
through `ESSES_APP_EXE`.

M1-STUB: none here; the shell's behaviour lives in `esses_widgets` (`MainWindow`). The 06 §§18-19 deferrals
(single-instance forwarding, `--new-instance`, `--scheme`, `--keymap`) are built (M4, D-212); file associations are
the packages' (`tools/packaging/`).
