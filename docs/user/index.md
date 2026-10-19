# Esses user guide

Esses is a free, open-source application for analysing motorsport data logs: open the day's log files, pick laps
to compare, look at them in graphs, reports and gauges, and add your own maths channels. This guide covers
version 0.1, the first public alpha. It is short on purpose; every page lists the keys and menus it talks about.

Esses opens `.ld` log files (with their `.ldx` companions) and CSV exports, and reads maths expressions written in
the syntax of MoTeC i2. Esses is an independent project: it is not affiliated with, endorsed by or supported by
MoTeC, and it contains none of MoTeC's software, help text or files.

## Pages

| Page | What it covers |
|---|---|
| [Installing Esses](install.md) | The Windows installer and portable zip, the Linux AppImage and the macOS disk image, the C++ runtime, the command line |
| [Opening files](open-files.md) | `.ld` and `.ldx` files, CSV files, the Import Log, the Mapping panel, projects, files that change on disk, the memory budget warning |
| [Choosing laps](selection.md) | The Data window, Main and overlays, filters, trust, the fastest-lap keys |
| [Graphs and worksheets](graph.md) | Workbooks and worksheets, the components, time and distance, zoom, cursor and datum, high-DPI screens |
| [Maths](maths.md) | The expression language in brief, the Maths Editor, local and transient maths, Channel Status |
| [Troubleshooting](troubleshooting.md) | The Messages window, settings and how to reset them, portable mode, reporting a bug without sharing private data |
| [Replacing the Qt libraries](relinking.md) | How to run Esses with your own build of Qt (the LGPL re-linking right) |

## Finding your way around

- **F1** opens this guide (Help > Help Contents). An installed copy opens from the installation folder; otherwise
  the copy in the public repository opens in your browser.
- **Help > Keyboard Shortcuts** lists every command with its keys. **CTRL+SHIFT+P** opens the command palette:
  type part of a command's name and press ENTER to run it.
- The window has a worksheet area in the middle and dock windows around it: the **Data** window (F8), the
  **Channels** window (C), the **Values** window (V), **Channel Status** (CTRL+U), **Messages** and the **Track
  Map**. The View menu shows and hides them, and View > Reset Dock Windows puts them back where they started.
- Keys act where the keyboard focus is: worksheet keys need a worksheet component focused, graph keys a graph.
  ESC returns the focus from a dock window to the worksheet.

## What 0.1 does not do yet

0.1 is an alpha. It does not import workbooks, maths or settings files from other analysis software (you rebuild
your worksheets; a default circuit workbook is included), has no indexed file browser, no printing, no GPS
correction dialog and no Drag or Rally profiles. Only the Windows packages are supported; the Linux and macOS
packages are built by the same automated checks but are labelled "builds green, not supported".

Esses is MIT-licensed. The source, the issue tracker and this guide live at
<https://github.com/mitchdetailed/jubilant-octo-meme>.
