# Troubleshooting

## The Messages window

**View > Messages** shows everything Esses has to tell you that is not urgent enough for a dialog: the Import Log
of every file you open, maths problems, memory budget warnings, files that changed on disk, settings or layouts
that could not be read, and the program's own warnings and errors. Each row has a time, a kind (Import, Maths,
Budget, Project, System), the file or document it is about and the message.

- The **Errors**, **Warnings** and **Info** buttons show or hide each severity; the filter box searches the text,
  the source and the code of every row.
- A row with an action (Reload File, Continue Opening, and so on) shows it after the text: double-click the row or
  press ENTER to run it; SHIFT+double-click or SHIFT+ENTER runs a second action when there is one; the context
  menu offers both.
- **Copy** copies the visible rows; **Save As…** writes them to a text file; **Clear** empties the list. The list
  keeps the last 5,000 rows of the session and is not kept after Esses closes.
- The line above the list shows memory and background-task statistics; the status bar's Performance field shows
  the memory in use against the memory budget (see [Opening files](open-files.md#the-memory-budget-warning)).
- The window opens by itself on the first error of a session, unless Tools > Options > General "Show the Messages
  window on an error" is off.

Esses 0.1 writes no log file of its own: its warnings and errors go to the Messages window. Save them with
**Save As…** when you need to keep them. The command-line tool prints its errors to the terminal.

## Where Esses keeps its files

| What | Windows | Linux | macOS |
|---|---|---|---|
| Settings | `%APPDATA%\esses\` | `~/.config/esses/` | `~/Library/Application Support/esses/` |
| Cache | `%LOCALAPPDATA%\esses\cache\` | `~/.cache/esses/` | `~/Library/Caches/esses/` |
| New projects (suggested) | `Documents\esses\Projects\` | `~/Documents/esses/Projects/` | `~/Documents/esses/Projects/` |

The settings folder holds `settings.json` (the Options), `layout.json` (the window and dock layout), `keymap.json`
(your key bindings, when you changed any), `schemes\` (your own colour schemes) and `units.user.json` (units you
added). Projects are ordinary folders wherever you put them, and each log file's edits are in the `.sidecar.json`
beside it.

The environment variables `ESSES_CONFIG_DIR`, `ESSES_CACHE_DIR` and `ESSES_DATA_DIR` point Esses at other
folders (for example a clean settings folder for a test).

## Resetting settings

- **Docks in a muddle:** View > Reset Dock Windows puts every dock window back in its default place.
- **One kind of setting:** close Esses, then delete or rename that file in the settings folder: `layout.json`
  (window and docks), `keymap.json` (key bindings), `settings.json` (every option on Tools > Options). Esses
  starts with the defaults for what is missing.
- **Everything:** close Esses and rename the whole settings folder (for example to `esses.old`), so you can copy
  back anything you want to keep. Your projects and the sidecars beside your logs are not in that folder and are
  not touched.

A settings file that cannot be read is reported in Messages and the defaults are used; Esses never overwrites a
project document it could not read.

## Portable mode

When a file named `portable.marker` is next to the Esses program file, Esses keeps its settings in a `config`
folder and its cache in a `cache` folder beside the program instead of in your user profile. The portable zip
ships with the marker. To move a portable copy to another PC, copy the whole folder; delete the marker to make
that copy use the settings folder of your user profile again.

## Esses does not start (Windows)

- **It crashes at once, or Windows reports an error in `msvcp140.dll`:** the Visual C++ runtime on the PC is
  older than Esses needs. Run the Esses installer again, which installs a current runtime, or install the latest
  "Microsoft Visual C++ Redistributable for Visual Studio 2015-2022 (x64)" from Microsoft, or use the portable zip,
  which brings its own copy.
- **Windows protected your PC:** the 0.1 packages are unsigned; choose More info, then Run anyway.
- **It starts on a monitor that is no longer there:** close Esses and delete `layout.json` from the settings
  folder.

## Reporting a problem

Problems go to the issue tracker of the public repository,
<https://github.com/mitchdetailed/jubilant-octo-meme/issues>. Issues are public, so keep private data out of them:

1. **Say which version and system.** Help > About Esses, or run `esses --version` (or `esses-cli --version`),
   and give your operating system and display scale.
2. **Say what you did and what happened**, step by step, and what you expected instead.
3. **Add the Messages rows** that belong to the problem (select them, Copy, paste into the issue). Read them
   first: they can contain file and folder names, which can include your name, a team or a venue. Replace
   anything you do not want public with a placeholder such as `<file>`.
4. **Screenshots** help with drawing problems; crop away channel values or details you do not want to share.
5. **Do not attach log files, projects, sidecars or maths files** unless you own them and are happy to publish
   them. If a problem only happens with one file, say so and describe it instead: the logger or program that
   wrote it, its size, how many channels and laps it has, and what `esses-cli info <file>` prints after you
   remove the details (driver, vehicle, venue, comments) from that output.
6. **Security problems** are reported privately: the repository's Security tab has "Report a vulnerability".
