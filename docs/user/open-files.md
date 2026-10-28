# Opening files

## Log files

**File > Open Log File…** (CTRL+O) opens one or more log files. The Details pane beside the file list shows what
Esses reads from the highlighted file before you open it (or why it cannot), and the dialog remembers the last
folder. You can also double-click an associated `.ld` file, or start Esses with `-l <file>` (see
[Installing Esses](install.md#starting-esses-from-a-command-line)). Several files can be open at once; each is a
row in the Data window (F8). **File > Close All Log Files** (CTRL+A) closes them all; DELETE on a file's row in
the Data window closes that one.

Opening is quick even for large files: Esses reads the file's header first and shows the file straight away,
then reads the samples as graphs need them, in the background.

### `.ld` and `.ldx` files

`.ld` files are read by the importer `motec.ld`. When a `.ldx` file with the same name sits beside the `.ld`, its
beacon markers and details are merged in automatically: you open the `.ld`, never the `.ldx`. Laps are built from
the beacon markers: the out lap, the flying laps and the in lap of each outing, each with its lap time and a
trusted flag (see [Choosing laps](selection.md)).

### CSV files

- A CSV export in the legacy layout that the importer `motec.csv` reads (Esses' own Export Data writes it too) is
  recognised from its first line and opens with its details, units and sample rate.
- Any other delimited text file (`.csv`, `.txt`, `.tsv`, `.asc`, `.log`) opens with the generic text importer.
  It works out the delimiter, the decimal separator, the header row and an optional units row below it, the time
  column (a column named like `Time`, else the first column that only increases) and the sample rate. A column
  named like a lap counter gives laps. Text columns become enumerations.

**File > Import Data…** opens CSV and other text logs too. Every guess the importers make is listed in the
file's **Import Log** (below), so you can see why a file opened the way it did.

### The Import Log

Each open adds the file's Import Log to the **Messages** window (View > Messages): one row per note, warning or
error, with the file as its source. Typical rows say which companion files were found, which layout was
recognised, which units could not be matched and which samples were invalid. The window opens by itself on the
first error of a session (Tools > Options > General "Show the Messages window on an error").

### The Mapping panel: channels with unknown units

When a file has channels whose units (or names) Esses does not recognise, the open stops at the **Mapping panel**
above the worksheet area, with one row per such channel: its name and unit as written in the file, some sample
values, and the quantity, unit and analysis name you choose. For each row:

1. Pick the **Quantity** and **Unit** (for example Pressure, kPa), and optionally an **Analysis name** (the name
   the channel should have in Esses).
2. Choose the **Rule** for later files: match by name only, by unit only, or by name and unit. Tick **Apply to
   future files** to remember the answer.

Then press **Resolve** to open the file with your choices (one undoable step), **Save** to store the ticked
answers in the project's unit map so later files open without asking, **Scan** to look the rows up again,
**Open Anyway** to open the file with those channels unitless, or **Cancel** to not open it. Several waiting files
queue up; the panel says how many.

## Projects

A project is a folder that holds your workbooks, maths, channel settings and the files you last had open.
Without a project Esses uses the shipped defaults (the default circuit workbook); create a project to keep your
own worksheets, maths and channel settings.

- **File > New Project…** asks for an empty folder and a name; **File > Open Project…** opens one; `esses -w
  <folder>` opens one from the command line. **File > Close Project** closes it.
- **File > Save Project** (CTRL+S) saves every changed document. The previous copy of each file is kept in the
  project's `backup` folder (the last five).
- Changes are also written every two minutes to a journal inside the project (Tools > Options > General
  "Autosave interval"; 0 turns it off). If Esses stops without saving, the next open offers to recover them.
- With Tools > Options > General "Load previous data with the project" on, opening a project reopens its log
  files and restores the laps you had selected.
- CTRL+Z and CTRL+Y undo and redo edits to the project's documents (layout, channels, maths, details, laps). The
  selection, the cursor and the zoom are not undo steps.

Edits that belong to one log file (lap and beacon changes, trust flags, details, local maths) are stored beside
the log in a small `<log file>.sidecar.json`, so they follow the file into any project.

## When a file changes on disk

Esses notices when a file it has open is changed or replaced by another program (checked each time Esses becomes
the active application). The file is then marked **stale**: an amber badge on its row in the Data window, and a
row in Messages. Nothing more is read from the old copy. To use the new contents choose **File > Reload File**,
**Reload File** in the file's context menu in the Data window, or the Messages row's action. Reloading opens the
file again with its sidecar edits and keeps the same laps selected.

While a file is open in Esses, Windows lets another program replace the file (write a new copy and rename it
over the old one) but not overwrite it in place. If a program reports that it cannot write a log, close that
file in Esses first.

## The memory budget warning

Esses keeps decoded samples in memory up to a **memory budget**: by default a quarter of the computer's memory,
at least 1 GB, set in Tools > Options > General "Memory budget" (in GB). Before it loads a file, Esses estimates
the memory the file needs once every sample is loaded. If that estimate is above the budget, nothing is loaded
yet and a **Budget** warning appears in Messages with the estimate, the budget and what is in use:

- **Continue Opening**: double-click the row or press ENTER on it. The file opens normally.
- **Cancel Opening**: SHIFT+double-click, SHIFT+ENTER, or the row's context menu. The file is not opened.

In 0.1 the budget is a warning only: data that is loaded stays loaded. The status bar's **Performance** field
shows the memory in use against the budget (for example "Resident 412 MB of 4.0 GB"); click it to see the
details in Messages. If you work near the budget, close files you no longer need (DELETE in the Data window).
