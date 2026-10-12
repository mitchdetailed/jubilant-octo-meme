# esses (esses_app)

Layer 5, Qt Widgets (01 §2.1, 06 §§18-19). The GUI executable: `main()` only. Start-up order (01 §5.3 rule 11):
`QApplication` -> `qt::AppRuntime` (pool, cache, dispatcher; the GUI thread marked `ThreadRole::NoBlock`) ->
`widgets::registerBuiltinComponents()` -> `widgets::MainWindow`; after `exec()` the window is destroyed, then
`rt.shutdown()`, then the runtime and the application.

```
esses [-w <project folder>] [-l <log file>]... [--offscreen] [--dump-commands] [--version] [<file>...]
```

`-w`/`--workspace` takes the project folder or its `project.json`; `-l`/`--log` is repeatable and relative paths
resolve against the current directory; a bare argument is a project when it is a folder with `project.json` (or a
`project.json`), otherwise a log (06 §19 item 3). The MainWindow restores the session's dock layout on start
(`<config>/layout.json`, then the project's `.local/layout.json` after `-w`) and writes it on exit (06 §§3.4,
18 step 4). `--offscreen` sets `QT_QPA_PLATFORM=offscreen` before the application starts. `--dump-commands`
prints the `CommandRegistry` as JSON (the registry export that `esses-cli validate` checks keymaps against,
06 §14.3) without writing the layout. ctest `app.version` starts the executable offscreen.

M1-STUB: none here; the shell's behaviour lives in `esses_widgets` (`MainWindow`). File associations and the
single-instance forwarding (`--new-instance`), `--scheme` and `--keymap` arrive with 06 §§18-19 (M4).
