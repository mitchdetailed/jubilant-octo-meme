# Maths

Maths channels are channels you define with an expression, such as a brake balance from two pressures. They
behave like logged channels everywhere: in graphs, reports, gauges, other maths and CSV exports. Esses evaluates
them in the background, several at a time; until a channel is ready, it is shown as pending (`…`).

## The expression language in brief

```
'Brake Pres Front'[bar] / ('Brake Pres Front'[bar] + 'Brake Pres Rear'[bar]) * 100
```

- **Channels** are written in single quotes: `'Ground Speed'`. Names are not case-sensitive. A constant, a
  numeric detail of the log or a text detail is referenced the same way.
- **Units** in square brackets right after a channel convert it before the calculation: `'Ground Speed'[km/h]`.
  Without a unit (or with `[]`) the value is used in Esses' storage unit, the SI base unit (m/s, kelvin), and the
  editor warns, because that is rarely what you want. Unitless channels need no unit.
- **Numbers**: `3` is a whole number and `3.0` or `3e0` a decimal. The difference matters in window arguments:
  `smooth('X'[bar], 5)` averages 5 samples, `smooth('X'[bar], 0.5)` averages over 0.5 seconds.
- **Text** in double quotes: `"Outings:Laps"` (a range path) or a text value.
- **Operators**, from the tightest binding: `^` (power); unary `-`, `!`, `~`, `NOT`; `*` and `/`; `+` and `-`;
  `<`, `<=`, `>`, `>=` (or `LT`, `LTE`, `GT`, `GTE`); `==`, `!=` (or `EQ`, `NEQ`); `&`; `|`; `&&` (`AND`);
  `||` (`OR`). The words are not case-sensitive. Note that `-2^2` is `-4`: the power binds tighter than the minus.
  A project option switches this for maths that expect `4`; the editor shows a note on every expression the
  switch would change.
- Dividing two whole numbers gives a decimal result; `int()` makes a whole number.
- **Comments**: `// to the end of the line` and `/* ... */`.

### Functions

Function names are not case-sensitive (the editor suggests the lower-case spelling). The 0.1 set:

| Kind | Functions |
|---|---|
| Arithmetic | `abs`, `sgn`, `sqr`, `sqrt`, `power`, `exp`, `exp10`, `ln`, `log`, `hypot`, `min`, `max`, `int`, `frac`, `round`, `round_up`, `round_down`, `remainder`, `choose`, `invalid` |
| Trigonometry | `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh` |
| Bits | `bit_and`, `bit_or`, `bit_xor`, `bit_not`, `bit_decode` |
| Over time | `smooth`, `derivative`, `integrate`, `integrate_dist`, `running_min`, `running_max`, `time_shift`, `previous_sample`, `edge_delay`, `time_valid`, `pulse_width`, `flip_flop` |
| Filters | `filter_lp`, `filter_hp`, `filter_cheby_lp`, `filter_cheby_hp`, `filter_cheby_bp`, `filter_cheby_bs` |
| Per range | `stat_min`, `stat_max`, `stat_mean`, `stat_std_dev`, `stat_start`, `stat_end`, `range_is`, `range_change` |
| Lap comparison | `variance_time`, `variance_dist` |
| Test data | `rand_val`, `rand_chan` (repeatable: the same expression gives the same values) |

The Maths Editor shows a function's help when you point at its name. `esses-cli maths functions` prints the
list, and
`esses-cli eval "<expression>" <log file>` evaluates an expression over a log from the command line.

## Where maths lives

| Kind | Applies to | Stored in | Made with |
|---|---|---|---|
| Project maths | Every open file (a module can have a condition, so it applies only where the condition holds) | The project's `maths` folder, in modules evaluated in project order | The Maths Editor |
| Local maths | One log file | The file's sidecar beside the log | The Maths Editor's Local maths, CTRL+SHIFT+O, CTRL+SHIFT+Z |
| Transient maths | One log file, until it is closed | Nowhere: never saved | CTRL+SHIFT+F |
| Built-in | Every file | Settings in the project | Tools > Corrected Speed and Distance… (Corr Speed, Corr Dist) |

When several definitions give the same channel name, they are calculated in a fixed order: the logged or
built-in channel, then local maths, then the project's modules from top to bottom, then a transient filter. Each
one can read the one before it (a maths item named like a logged channel can correct that channel), and what you
see is the last one. A second item of the same name in one module is ignored. Channel Status shows what each name
resolves to.

## The Maths Editor (CTRL+M)

**Tools > Maths Editor…** lists the modules and their items. Add an expression, a filter, a scale and offset item
or a constant; set its name, unit, decimal places and comment; enable, disable, cut, copy and paste items. Table
items read from a module file are kept and evaluated, but they cannot be edited yet.

While you type, the line under the expression says whether it is valid and why not, with the position of the
problem. Typing `[` after a channel lists the units that fit it. **Insert Channel** and **Insert Range** put a
quoted channel name or range path at the cursor. You can edit maths with no log open; channel names are then
checked when data arrives. The editor saves the project's maths when you close it (an item with an error keeps
it open so you can fix or revert it).

### From a channel in a graph (the active channel)

| Key | Does |
|---|---|
| CTRL+SHIFT+M | Open the Maths Editor at the item that defines the active channel |
| CTRL+SHIFT+F | Filter the channel for this session (a transient item that replaces it until the file closes) |
| CTRL+SHIFT+O | Scale and offset the channel (a local maths item of this file) |
| CTRL+SHIFT+Z | Offset the channel so that it reads zero at the cursor (a local maths item) |
| CTRL+SHIFT+S | Show the channel's row in Channel Status |

## Channel Status (CTRL+U)

The Channel Status window lists every maths item, alias, mapping and built-in channel of the open files with its
status: **OK**, **Pending** (defined, not needed yet), **Calculating** (with a percentage), **Warning** (it
evaluates, but something looks wrong, such as a unit that does not fit), **Error** (it cannot be evaluated: the
message says why and where) or **Disabled**. The **Resolved to** column shows what a name really reads, for
example an alias that falls through to its second candidate. Double-click a row to edit the item, alias or
mapping. "Errors only" and the filter box narrow the list; Copy Table copies it.

## Channel settings, aliases and mappings

- **Tools > Channel Editor…** (CTRL+E) sets each channel's display unit, limits, decimal places, colour and
  interpolation, for every open file.
- **Tools > Aliases…**: an alias is a name with an ordered list of candidate channels; the first one a file has
  is used. Use one to give the same name to a channel that different loggers name differently.
- **Tools > Mappings…**: a mapping renames a logged channel when files open (logged name to analysis name).
- **Tools > Units List…** lists the units Esses knows.
