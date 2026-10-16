# 03 — Maths engine (`esses_maths`) and built-in analysis modules

**Scope.** This document specifies the Qt-free target `esses_maths` (layer 2, depends on `esses_base`, `esses_units`, `esses_store`, `esses_model`, `esses_analysis`; the IIR filters are designed in-tree, D-169): the i2-source-compatible expression language (lexer, Pratt parser, AST), the type system with unit/dimension tracking, the complete intrinsic catalogue grouped by kernel class, compilation to `Plan`s of `Kernel`s evaluated in 4,096-sample blocks, `PlanProvider`/`MathInputGatherer`/`MathGraph`/`MathsScheduler`, version-keyed caching and invalidation, rate selection, the per-channel status model, global/local/transient maths, the `maths-1` module document with constants, enumerations, filters, scale/offset and 2D tables (3D tables at v1.0, D-027), the three built-in modules (`esses.corrected_speed_distance`, `esses.gps_corrections`, `esses.damper_velocity`), the `IMathFunction` extension interface with the C ABI (`esses_mathfn_v1`, v1.0) and Lua (v1.1), the Maths Editor UX contract, the i2 maths XML importer mapping (v1.0; the importer itself lives in `esses_project`, D-122), and the test strategy. Milestone staging (D-044): M1 ships only `MathGraph` (define/require/version), `PlanProvider`, `EvalContext` and the built-in `esses.corrected_speed_distance` module with `fastestWheel` and clamps; M2 ships `Parser`, `TypeChecker`, `esses-cli eval`, the full language, all intrinsics, modules/items, `MathGraph` shadowing/cycles/versions/`SelectionDependent`, `PlanProvider` into the chunk cache and the Maths Editor. Through 0.1 every node is evaluated over the whole layer extent by one task (§6.0); the chunk-parallel evaluator (§6.1–§6.2, §7.4) is the v1.0 path. The Qt facade `MathsService` and the editor widgets belong to `esses_qtmodel`/`esses_widgets` and are only constrained here.

**Related decisions.** D-007 (Qt-free line), D-008 (`Micros`, `Rate{num,den}`), D-009 (series kinds, dtypes, validity), D-010 (base units in memory, explicit unit objects on persisted numbers), D-011 (`Chunk`, `ChunkCache`, `IChunkProvider`), D-013 (name identity, `ChannelResolver`), D-014 (range groups, `RangeRef`), D-015 (sidecar `localMaths`), D-016 (corrected speed/distance module, `DistanceIndex`, stretch), D-017 (reference overlay), D-020 (grammar and precedence), D-021 (types, warning policy), D-022 (blocks, kernels, cache, shadowing), D-023 (variance), D-024 (modules, C ABI, Lua), D-027 (Maths Editor), D-028 (C ABI conventions), D-030 (scripting), D-031 (`TaskPool`, snapshots), D-032 (undo), D-033 (tests), D-035 (milestones).

---

## 1. Placement and public surface

| Item | Value |
|---|---|
| Target | `esses_maths` (static lib), namespace `esses::maths` |
| Depends on | `esses_base`, `esses_units`, `esses_store`, `esses_model`, `esses_analysis` (Iir1 was planned here; the IIR filters are designed in-tree instead, D-169) |
| Must not depend on | `esses_formats`, `esses_project`, `esses_plotcore`, anything Qt (`cmake/qt-free-check.cmake`) |
| Consumers | `esses_cli` (`eval`, `maths check`), `esses_qtmodel::MathsService` (receives the `MathModule`s that `esses_project` validates against `SchemaRegistry` and loads from `maths-1` documents through `MathModuleIo`, 01 §2.1; owns the `MathGraph`s, builds `MathSelection`), `esses_widgets::MathsEditor`, `ChannelStatusWindow`, `esses_script` and `esses_capi` (v1.1), and `esses_project`, which links `esses_maths` as a same-layer edge because its v1.0 `CfgMathsImporter` builds `MathModule`s and parses every script (01 §2.2 rule 1, §12 D-1; D-122). `esses_plotcore` does not link `esses_maths`: maths outputs reach plots as `Column`s through the shared `ChunkCache` |
| Public headers | `maths/Lexer.h`, `Parser.h`, `Ast.h`, `Types.h`, `TypeChecker.h`, `Scope.h`, `Plan.h`, `Kernel.h`, `Intrinsics.h`, `PlanProvider.h`, `MathInputGatherer.h`, `MathGraph.h`, `MathsScheduler.h`, `MathModule.h`, `MathModuleIo.h`, `MathItem.h`, `EvalContext.h`, `MathSelection.h`, `IMathFunction.h`, `FunctionRegistry.h`, `Builtins.h`, `MathsCompat.h`, `Diagnostics.h` |
| Threads | Every evaluation runs on the `TaskPool` (D-031) at the requesting priority; `MathGraph` mutation is single-threaded (GUI thread or CLI main thread); workers read only `std::shared_ptr<const DataSource>` snapshots and `ChunkHandle`-pinned chunks |
| Persistence | `maths/<module>.maths.json` (schema `maths-1`, read and written by `MathModuleIo`; `esses_project`, which links both libraries, validates against the schema through `esses_project::SchemaRegistry` and loads through `MathModuleIo`, 01 §2.1); sidecar `localMaths` (schema `sidecar-1`, item schema shared with `maths-1`); never the `.ldx` |

Request path (outline §2.3): `MathsService::require(ChannelKey, SampleRange, Priority)` → `MathGraph::require` → cache hit on `ChunkCache` or `MathsScheduler` task → `PlanProvider::materialise` → chunks sealed and pyramided → `Dispatcher::post(ChannelReady{key, version, chunkRange})`.

---

## 2. Language

### 2.1 Lexical structure

Source text is UTF-8, parsed in the C locale regardless of the host locale (i2 `DefaultLocale="C"`). Newlines are whitespace. Tokens:

| Token | Lexeme | Notes |
|---|---|---|
| `ChannelRef` | `'` … `'` | Any character except `'`; no escapes; name is NFKC case-folded for lookup, original spelling kept for messages. Resolves to a channel, constant, numeric detail, text detail or (later) setup-sheet value |
| `UnitSuffix` | `[` … `]` | Only valid immediately after `ChannelRef` (whitespace allowed between). Content is a unit symbol or alias from `UnitRegistry` (`km/h`, `m/s/s`, `kPa a`, `psi g`, `rpm`, …) or empty |
| `IntLit` | `[0-9]+` | Distinct AST node from `DecLit`; selects *points* in window arguments |
| `DecLit` | `[0-9]+ '.' [0-9]*`, `'.' [0-9]+`, or any number with an exponent `e[+-]?[0-9]+` | Selects *seconds* in window arguments; `1e3` is decimal |
| `StrLit` | `"` … `"` | No escapes. A text literal (`"Joe"`) or a range path (`"Outings:Laps"`); which one is decided by the type checker from the parameter type |
| `Ident` | `[A-Za-z_][A-Za-z0-9_]*('.'[A-Za-z_][A-Za-z0-9_]*)*` | Function names; dotted form for external functions (`mylib.rolling_rms`, i2 `ProgId.name` spelling) |
| Keyword operators | `AND OR NOT LT LTE GT GTE EQ NEQ` | Case-insensitive; `and`, `And` equal `AND` |
| Keyword constants | `rise fall` | Bare arguments of `edge_delay`; case-insensitive |
| Operators | `+ - * / ^ < <= > >= == != ! && \|\| ~ & \|` | |
| Punctuation | `( ) ,` | |
| Comments | `// … EOL`, `/* … */` (not nested) | Dropped by the lexer; positions preserved for diagnostics |

Function names are case-insensitive; a call whose spelling differs from the canonical lower-case name yields diagnostic `W-STYLE-CASE` (D-020). Channel names are case-insensitive (NFKC case-fold, D-013).

### 2.2 Grammar (EBNF)

```
expression := or_expr ;
or_expr    := and_expr  { ( "||" | OR  ) and_expr } ;
and_expr   := bor_expr  { ( "&&" | AND ) bor_expr } ;
bor_expr   := band_expr { "|" band_expr } ;
band_expr  := eq_expr   { "&" eq_expr } ;
eq_expr    := rel_expr  { ( "==" | "!=" | EQ | NEQ ) rel_expr } ;
rel_expr   := add_expr  { ( "<" | "<=" | ">" | ">=" | LT | LTE | GT | GTE ) add_expr } ;
add_expr   := mul_expr  { ( "+" | "-" ) mul_expr } ;
mul_expr   := unary     { ( "*" | "/" ) unary } ;
unary      := ( "-" | "+" | "!" | "~" | NOT ) unary | power ;
power      := postfix [ "^" power_rhs ] ;
power_rhs  := ( "-" | "+" ) power_rhs | power ;          (* right-associative; exponent may carry a sign *)
postfix    := primary ;
primary    := IntLit | DecLit | StrLit
            | ChannelRef [ UnitSuffix ]
            | Ident "(" [ expression { "," expression } ] ")"
            | ( rise | fall )
            | "(" expression ")" ;
```

### 2.3 Pratt binding powers (D-020)

| Level | Operators | Assoc | Left bp | Right bp |
|---|---|---|---|---|
| 11 | call `f(…)`, `[unit]` | postfix | 110 | — |
| 10 | `^` | right | 100 | 95 (parses `unary`, so `2^-3` is legal) |
| 9 | unary `- + ! ~ NOT` | prefix | — | 90 |
| 8 | `* /` | left | 80 | 81 |
| 7 | `+ -` | left | 70 | 71 |
| 6 | `< <= > >= LT LTE GT GTE` | left | 60 | 61 |
| 5 | `== != EQ NEQ` | left | 50 | 51 |
| 4 | `&` | left | 40 | 41 |
| 3 | `\|` | left | 30 | 31 |
| 2 | `&& AND` | left | 20 | 21 |
| 1 | `\|\| OR` | left | 10 | 11 |

Consequences: `-2^2 = -4`; `2^3^2 = 2^9`; `'G Force Lat'[m/s/s] / ('Corr Speed'[m/s]^2)` parses as the shipped i2 maths expects; `a & b == c` parses as `a & (b == c)` (C convention). **Compat switch** `MathsCompat::unaryMinusAbovePower` (project option `maths.compat.unaryMinusAbovePower`, default `false`) swaps levels 9 and 10 so `-2^2 = 4`; it is part of the definition hash (§7.2) so flipping it invalidates every expression that contains both a unary operator and `^`. The parser reports `I-COMPAT-POW` once per expression that is affected by the switch, so the editor can show the hover note required by D-020.

### 2.4 Literal and argument semantics

1. `IntLit` → `Scalar{integer=true}`; `DecLit` → `Scalar{integer=false}`. Integer ÷ integer is floating division (i2 1.1.1); `int()` is the only way to force integer type.
2. Window arguments (`smooth`, `derivative`, Filter item): `IntLit` = points; `DecLit` = seconds, converted at the result rate to `n = 2·⌊t·rate/2⌋ + 1` (odd, ≥ 1; an n of 1 from `t·rate < 2` is then widened to 2 by the §5.2 rule). A non-literal scalar argument (constant, detail): time dimension → seconds; unitless and integral → points; unitless and fractional → seconds (diagnostic `W-WINDOW-ARG`).
3. `StrLit` becomes `Text` unless the parameter type is `RangeRef`, in which case it is parsed as a colon path and validated against the source's `RangeGroupTree` at evaluation time (unknown path → status Error `E-RANGE-UNKNOWN`, listing available paths).
4. `rise`/`fall` are `Keyword` nodes accepted only by `edge_delay`.
5. `[unit]` after a reference converts the referenced base-unit values into `unit` before use (`Converter(base→unit)` from `esses_units`, affine, delta-aware); i2 requires a unit after every channel reference and defines it as the unit the value is converted to before calculation. `[]` and omission mean "as stored" (base unit) and type the node with the channel's own unit; on a reference whose quantity has a dimension (channel, numeric detail or constant that is not unitless) they raise `W-UNIT-MISSING` (Warning, amber; live in the editor, in Channel Status, and listed in the import report, §12), so an unannotated `'Brake Temp' > 100` (which compares kelvin) or `'Wheel Speed'` (m/s) is flagged instead of silently differing from i2; unitless references raise nothing. The base-unit reading itself is recorded in `docs/maths-compat.md`. A `[unit]` whose quantity family differs from the channel's is `W-UNIT-FAMILY` and no conversion is applied (numbers pass through, i2 behaviour).

### 2.5 Syntax diagnostics (must be reproduced verbatim as codes; text is ours)

`E-QUOTE-MISSING` (identifier that is not a function name, suggests `'…'`), `E-UNIT-BRACKET` (`'X' km/h` without brackets), `E-PAREN-UNBALANCED`, `E-CALL-UNCLOSED`, `E-ARITY` (lists accepted arities), `E-ARG-TYPE` (`derivative(1)`), `E-COMMA-MISSING`, `E-UNKNOWN-FUNCTION` (with nearest-name suggestion), `E-UNKNOWN-CHANNEL` (reported by the checker when a scope is available; without data loaded it is `I-UNRESOLVED`, which does not block saving — i2 1.0.20 "edit without data"), `E-UNIT-UNKNOWN`. Every diagnostic carries `{code, level, byteStart, byteEnd, message, hint}`.

---

## 3. AST and type system

### 3.1 AST

```cpp
namespace esses::maths {

using Span = esses::TextSpan;                            // {begin, end} byte offsets into the source (01 §6.1)

enum class NodeKind : std::uint8_t {
    IntLit, DecLit, StrLit, Keyword, ChannelRef, Unary, Binary, Call, Convert /*[unit]*/ };

enum class UnaryOp  : std::uint8_t { Neg, Plus, Not, BitNot };
enum class BinaryOp : std::uint8_t { Add, Sub, Mul, Div, Pow, Lt, Le, Gt, Ge, Eq, Ne,
                                     BitAnd, BitOr, And, Or };

struct Node {
    NodeKind kind; Span span;
    std::string text;                       // literal text, channel name (original), function name (canonical lower-case), keyword
    double      number = 0;                 // IntLit/DecLit value
    UnaryOp     uop{}; BinaryOp bop{};
    std::vector<std::uint32_t> kids;        // child indices in Ast::nodes
    std::optional<units::UnitId> unit;      // Convert: requested unit (nullopt = "[]")
    Type        type;                       // filled by TypeChecker
};

struct Ast {
    std::vector<Node> nodes; std::uint32_t root = 0;
    std::string source; Hash128 sourceHash;  // xxh3 of the normalised source (comments stripped, whitespace collapsed, names case-folded)
};

struct Diagnostic { std::string code; enum Level { Error, Warning, Info, Style } level;
                    Span span; std::string message, hint; };
using Diagnostics = std::vector<Diagnostic>;

class Parser { public: static Result<Ast> parse(std::string_view src, const MathsCompat&, Diagnostics&); };
std::string print(const Ast&);                            // canonical form; print(parse(x)) is idempotent
}
```

`Ast::sourceHash` ignores comments, whitespace and name case so cosmetic edits do not invalidate caches.

### 3.2 Types (D-021)

```cpp
enum class Kind : std::uint8_t { Scalar, Series, LapScalar, Bool, Text, RangeRef, Invalid };

struct Type {
    Kind kind;
    units::UnitId unit = units::kNone;      // registry unit when the value is in one unit; kNone for composites
    units::Dimension dim;                   // 7-vector [L,M,T,I,Θ,N,J]; Dimension::unknown() after fractional powers
    double scaleToBase = 1.0;               // multiplicative factor of the current numbers relative to SI base of `dim`; NaN when unknown
    std::optional<Rate> rate;               // Series/Bool/LapScalar only
    bool integer = false;                   // integer-typed (IntLit, int(), I32 channels, bit_* results)
    std::optional<std::string> lapGroup;    // LapScalar: range group path
};
```

Rules, inferred bottom-up by `TypeChecker::check(Ast&, const Scope&, Diagnostics&) → Type`:

1. `ChannelRef` → `Series` with the channel's unit (base) and rate; constants and numeric details → `Scalar` with their unit; text details → `Text`; `Invalid` for `invalid()`.
2. `Convert` on a `Series`/`Scalar` → same kind, `unit = requested`, `scaleToBase = unit.scalar`; on other kinds → `E-ARG-TYPE`.
3. `Series ∘ Scalar → Series`; `Series ∘ Series → Series` (rate reconciled per §7.1); `Scalar ∘ Scalar → Scalar` (constant-folded); `LapScalar ∘ X` → `LapScalar` if `X` is `Scalar`, else `Series` after lowering the `LapScalar` to a `Previous`-interpolated series over its ranges (D-021).
4. `+ -`: result `dim` = left `dim`; `W-DIM-MISMATCH` when dims differ and both known; `W-UNIT-SCALE` (Info) when dims match but `scaleToBase` differ by more than 1e-9 relative (`'A'[km/h] + 'B'[m/s]`) — no implicit conversion is ever inserted (i2 adds the numbers as written).
5. `* /`: dims add/subtract, `scaleToBase` multiplies/divides; a numeric literal factor other than ±1 beside an operand of known non-trivial dimension makes `scaleToBase` unknown (a conversion by hand, `/ 3.6`; D-185 (d)). `^`: exponent must be unitless (`W-DIM-MISMATCH` otherwise); with an `IntLit` exponent the dim is multiplied; otherwise `dim = unknown` when the base has a dimension.
6. Relational and equality → `Bool` (a Series of 0/1 when any operand is a Series, else a Scalar). i2 allows every relational operator on any data type including text: `Text` compares with `Text` under all six operators, `==`/`!=` exact and case-sensitive, `< <= > >=` lexicographic by UTF-8 code units; `Text` against a number is `E-ARG-TYPE` (readings recorded in `docs/maths-compat.md`).
7. `&& || !` → `Bool`; operands of any numeric kind (non-zero = true). `& | ~` require `integer` on every operand, else `E-INTEGER-REQUIRED` (the one hard type error, mirroring i2's "Integer/IntegerChannel operands only").
8. `choose(c, a, b)` unifies `a`/`b`: kinds unify to the wider of `Scalar < LapScalar < Series`; `Invalid` unifies with anything; dims per rule 4.
9. Function unit rules come from the catalogue (§5) or a plugin manifest (§10) using the vocabulary `same_as:<arg>`, `unitless`, `angle`, `sqrt:<arg>`, `per_time:<arg>`, `times_time:<arg>`, `times_length:<arg>`, `pow:<arg>,<arg>`, `unify:<args>`, `declared`.
10. **Declared result unit**: the item's `resultUnit` is compared with the inferred `dim`: mismatch → `W-RESULT-DIM` ("expression evaluates to L·T⁻¹, declared unit `G` is L·T⁻²"); when `scaleToBase` is known and differs from the declared unit's scalar → `I-RESULT-SCALE` with the suggested unit. **No warning ever blocks evaluation** (D-021). Strict mode (warnings as errors) is reserved for a later project flag and is not in scope.
11. The output descriptor gets `unit = resultUnit` (or unitless), `quantity` from the item, `dtype` per §7.4, `kind` `Enum` when an enumeration is attached, `interp` from the item (`Linear` default, `Previous` for enum/integer results).

### 3.3 Scope

```cpp
struct ScopeEntry { enum { Channel, Constant, DetailNumber, DetailText, Setup } what;
                    Type type; ChannelKey key; std::string resolvedName; model::ResolutionTrace trace; };
class Scope {                                    // built per (DataSource, layer) by MathGraph
public:
    std::optional<ScopeEntry> lookup(std::string_view nameCf, const ChannelKey& requester) const;
    const FunctionRegistry& functions() const;
    const units::UnitRegistry& units() const;
};
```

Lookup order for `'Name'` from an item in module `M` of source `S` follows `ChannelResolver` (D-013, 02 §4.6: Mappings → Aliases → canonical name → logged name), with the maths-only kinds placed in the name step: (1) project Mappings (rename) via `ChannelResolver`; (2) Aliases (D-013): when the name is an enabled alias, its candidates are tried in order, each through the whole resolver except the alias being resolved (02 §4.6 rule 17), so an alias may target a maths channel or another alias; the chain of aliases being expanded is kept and a revisit stops with `Error: alias cycle 'A' → 'B' → 'A'`; a candidate whose chain head has status Error, or that resolves to Missing, is skipped with `W-ALIAS-FALLBACK` (i2 1.1.4); (3) the shadowing chain head for that name in `S` as seen by the requester (§4.3); (4) a logged channel (canonical name, then logged name); (5) a numeric or text detail of `S`; (6) a constant: the constants of `M` itself first (for a local item `M` is the local module), then local constants of `S` (sidecar `localMaths`, imported `.ldx` constants), then constants of every other enabled global module whose `condition` holds for `S`, in project order (i2 constants are file-scoped; cross-module lookup was added later and honours the source enable condition); when two or more of those other modules define the name, the first in project order wins with `W-CONST-AMBIGUOUS`. A name that is both a channel and a constant resolves to the channel with `W-CONST-SHADOWED`. Every lookup records a `ResolutionTrace` (`'Wheel Speed' → alias → 'Wheel Speed FL' (km/h)`) for Channel Status.

---

## 4. Modules, items and resolution

### 4.1 Document model (`maths-1`)

```cpp
struct UnitValue { double v; units::UnitId unit; };                // persisted as {"v": 8000, "unit": "rpm"} (D-010)
struct MathConstant   { std::string name; UnitValue value; bool enabled = true; std::string comment; };
struct MathEnumeration{ std::string name; std::vector<std::pair<std::string,std::int64_t>> entries; };
struct DisplayDefaults{ units::UnitId unit; int dps = 2; int colourIndex = 0; };
struct RateSpec       { bool automatic = true; Rate rate{}; };      // "auto" | {num,den} | number (Hz)

enum class ItemKind : std::uint8_t { Expression, Filter, ScaleOffset, Table2D, Table3D, Builtin, Plugin, Regression };
struct MathItem {
    ItemKind kind; std::string name;        // output channel name (any char except '); Filter/ScaleOffset: the input channel name
    bool enabled = true; std::string comment;
    // Expression
    std::string expr; RateSpec rate; units::QuantityId quantity; units::UnitId resultUnit; DisplayDefaults display;
    store::Interp interp = store::Interp::Linear; std::optional<std::string> enumeration;
    struct { bool logged = true, telemetry = false; } applies;      // stored, telemetry unused before v1.1
    // Filter
    struct { std::optional<int> points; std::optional<double> seconds; } window;
    // ScaleOffset
    double scale = 1.0; UnitValue offset{0, units::kNone};
    // Tables (§4.4)
    TableSpec table;
    // Builtin / Plugin
    std::string moduleId; int moduleVersion = 1; JsonDoc settings;
    std::optional<std::string> precision;   // "single" | "double" (per item; module default applies otherwise)
    JsonDoc xI2;                            // JSON key "x-i2" (D-019): attributes imported from i2 and not understood (flags, params, …)
};
struct MathModule {
    std::string id; bool enabled = true; std::optional<std::string> condition;   // expression over details, Bool scalar
    std::string precision = "single";
    std::vector<MathConstant> constants; std::vector<MathEnumeration> enumerations; std::vector<MathItem> items;
    enum class Origin { Global, Local, Transient, Builtin } origin;
};
```

JSON example (`maths/Chassis.maths.json`):

```json
{ "$schema": "urn:esses:maths-1", "schemaVersion": 1,
  "id": "Chassis", "enabled": true,
  "condition": "'Vehicle Id' == \"GT3-07\"",
  "precision": "single",
  "constants": [
    { "name": "Gaussian Weight",   "value": { "v": 0.35, "unit": "" } } ],
  "enumerations": [
    { "name": "Balance", "entries": [ { "name": "Understeer", "value": 0 }, { "name": "Neutral", "value": 1 }, { "name": "Oversteer", "value": 2 } ] } ],
  "items": [
    { "kind": "expression", "name": "Yaw Rate Ref", "enabled": true,
      "expr": "'Corr Speed'[m/s] / 'Vehicle Wheelbase'[m] * tan('Steered Angle'[rad])  // Ackermann reference; wheelbase is the numeric detail",
      "rate": "auto", "quantity": "Angular Velocity", "resultUnit": "rad/s",
      "display": { "unit": "deg/s", "dps": 1, "colourIndex": 3 }, "interp": "Linear" },
    { "kind": "expression", "name": "Balance State",
      "expr": "choose('Oversteer'[deg] > 2, 2, choose('Oversteer'[deg] < -2, 0, 1))",
      "rate": { "num": 20, "den": 1 }, "quantity": "Unitless", "resultUnit": "", "enumeration": "Balance",
      "display": { "unit": "", "dps": 0, "colourIndex": 5 }, "interp": "Previous" },
    { "kind": "filter", "name": "G Force Long", "window": { "points": 6 },
      "display": { "unit": "G", "dps": 2, "colourIndex": 1 } },
    { "kind": "scale_offset", "name": "Brake Pres Front", "scale": 1.0, "offset": { "v": -0.3, "unit": "bar" } },
    { "kind": "table2d", "name": "Spring Force FL",
      "axis": { "expr": "'Damper Pos FL'[mm]", "unit": "mm", "breakpoints": [0, 10, 20, 30, 40] },
      "values": { "unit": "N", "data": [0, 850, 1700, 2600, 3600] },
      "interpolation": "linear", "outOfRange": "clamp",
      "quantity": "Force", "resultUnit": "N", "display": { "unit": "N", "dps": 0, "colourIndex": 7 } },
    { "kind": "table3d", "name": "Aero Balance",
      "x": { "expr": "'Ride Height Front'[mm]", "unit": "mm", "breakpoints": [20, 30, 40] },
      "y": { "expr": "'Ride Height Rear'[mm]",  "unit": "mm", "breakpoints": [40, 60, 80, 100] },
      "values": { "unit": "%", "rowMajor": true, "data": [[41,43,45,47],[40,42,44,46],[39,41,43,45]] },
      "interpolation": "linear", "outOfRange": "invalid",
      "quantity": "Ratio", "resultUnit": "%", "display": { "unit": "%", "dps": 1, "colourIndex": 2 } } ] }
```

`'Vehicle Wheelbase'[m]` in the example is the numeric detail of the source (02 §4.2 behaviour 2, as in i2's shipped Oversteer expression), not a module constant; the default circuit maths reads vehicle geometry from details in the same way. Every persisted number carries its unit object or sits in an array with a unit (`{unit, data}`), per D-010. `rate` accepts `"auto"`, `{num,den}` or a plain Hz number (normalised to `{hz,1}` on load).

### 4.2 Global, local and transient maths

| Scope | Lives in | Applies to | Lifetime | Created by |
|---|---|---|---|---|
| Global | `<Project>/maths/<module>.maths.json`, order from `project.json: mathsOrder` | every open source whose `condition` holds | project | Maths Editor, import |
| Local | `<log>.<ext>.sidecar.json: layers[].localMaths` (`sidecar-1`; same item/constant schema) | that source/layer only | file | Maths Editor "Local Maths", CTRL+SHIFT+O (scale/offset), CTRL+SHIFT+Z (zero at cursor: a `scale_offset` item with `offset = {v: −(displayed value at the cursor), unit: the channel's display unit}`, applied as a delta (§4.4), so the channel reads 0 in its display unit at the cursor: 0 °C, not −273.15 °C, for a temperature stored in K; 0 bar g for a gauge pressure; DPS preserved. Changing the display unit later keeps the physical delta, so a zeroed °C channel shown in °F reads 32 °F at that point) |
| Transient | memory only; module `id = "~transient"`, `origin = Transient` | the source whose channel label was used (the Data-window "apply filter to all sources" action, 06 §14.4 `data.filter_all_sources`, copies lap filters and does not touch maths) | until the source is closed/reloaded; never saved | CTRL+SHIFT+F (Filter from a channel label) |
| Builtin | registered by `MathGraph` on open (`origin = Builtin`); settings persisted in `project.json: builtinMaths` (and per-source overrides in the sidecar `localMaths` as `builtin` items) | every source | project | Corrected Speed/Distance dialog, Corrected GPS dialog (v1.0) |

### 4.3 Shadowing chain and order of calculation (D-022, i2 rules 1–5)

For every output name `N` in a `(source, layer)` the graph builds an ordered chain:

```
logged N  →  local self-maths on N (sidecar order)  →  global self-maths on N (module order; first item per module wins)  →  consumers
```

1. Distinct output names depend only on their references; list order is irrelevant.
2. A self-maths item (Filter, Scale & Offset, or an expression whose `name` equals a referenced channel) **reads the chain element before it** and becomes the new chain head for everyone after it. Hence local `rpm = smooth('rpm',2)` then global `rpm = 'rpm'*2` yields `smooth·2`.
3. A second item with the same output name in one module is ignored with status `Warning W-DUP-IGNORED` on the ignored item (not an Error).
4. Modules are applied in project order (top to bottom); local before global.
5. Any consumer (`'rpm2' = derivative('rpm')`) sees the chain head.
6. Mappings apply before the chain (rename); aliases are also checked before the chain (D-013, §3.3 step 2), but each candidate is looked up through the chain, so an alias may target a maths channel; an alias whose target is a maths channel with status Error, or is Missing, falls through to its next candidate (`W-ALIAS-FALLBACK`); alias-to-alias chains and cycle detection follow 02 §4.6 rule 17.
7. Cycles that are not self-maths steps (`A` references `B`, `B` references `A`) → every node on the cycle gets `Error E-CYCLE "A → B → A"`.
8. A module whose `condition` is false for a source contributes nothing to that source: its items show `Disabled (condition)` in Channel Status and its constants are unresolvable there.
9. `MathGraph::NodeId` is `(ChannelKey, chainIndex)`; the public `ChannelKey` resolves to the chain head. `ChannelKey { SourceId src; std::string layer; std::string nameCf; }`.

### 4.4 Non-expression items as plans

| Item | Equivalent plan | Notes |
|---|---|---|
| `filter` (simple Filter) | `smooth('N', points \| seconds)` on the chain input, output name `N` | Self-maths; result unit = input unit; DPS preserved from the input's override |
| `scale_offset` | `'N' * scale + offset_base` | `offset` converted from its unit to the input's base unit as a **delta** (temperature offsets are deltas, D-010 display families); result unit = input unit. As built (D-186, proposed, user must confirm): computed in the offset's unit when it is of the input's display family (`'N'[u] * scale + offset`, then to base), which equals this form for linear units and scales a temperature as displayed |
| `table2d` | `TableKernel{axis plan, breakpoints, values, mode, outOfRange}` | `axis.expr` is a full expression; breakpoints ascending (else `E-TABLE-AXIS`); ≥ 2 points |
| `table3d` | `TableKernel` bilinear over `x`, `y` | **v1.0** (D-027); before v1.0 the item loads with status `Error E-KIND-UNSUPPORTED` and is preserved. `values.data` is `len(x) × len(y)` row-major (`rowMajor` flag for imports) |
| `builtin` | expanded by the registered `IBuiltinModule` into N output plans (§9) | |
| `plugin` | multi-channel plugin module (post-v1.0) | MVP: status `Error E-PLUGIN-UNAVAILABLE`, preserved |
| `regression` | v1.0 | MVP: `Error E-KIND-UNSUPPORTED`, preserved |

Table lookup: binary search on the breakpoints (uniform-spacing fast path); `linear` interpolates, `next` takes the value at the first breakpoint ≥ x, `previous` at the last breakpoint ≤ x; `outOfRange: clamp` (default, the i2 reading we adopt) returns the edge value, `invalid` clears validity. Invalid axis sample → invalid output.

---

## 5. Intrinsic catalogue

Columns: **Class** is the kernel class (§6.1); **Extent** is `halo n` (chunk-independent with n samples of context each side), `ordered` (chunk-by-chunk with state carry), or `whole` (whole extent before any output); **Unit** uses the vocabulary of §3.2 rule 9. All kernels are NaN-/validity-aware: any invalid operand sample produces an invalid output sample unless stated otherwise. All trigonometry is in radians. Entries marked **[compat]** are readings of undocumented i2 behaviour; each has a row in `docs/maths-compat.md`, a `*.case.json` and, when D-005 permits, an oracle case.

### 5.1 Elementwise (class `Elementwise`, extent `halo 0`, fused into one `EwKernel`)

| Function | Signature | Semantics | Unit |
|---|---|---|---|
| `abs` | `abs(x)` | \|x\| | `same_as:x` |
| `sqr` | `sqr(x)` | x² | `pow:x,2` |
| `sqrt` | `sqrt(x)` | √x; negative → invalid | `sqrt:x` |
| `power` | `power(x, y)` | xʸ; y may be a Series | `pow:x,y` (dim unknown unless y is `IntLit`) |
| `hypot` | `hypot(x, y)` | √(x²+y²) | `unify:x,y` |
| `exp`, `exp10` | `exp(x)` | eˣ, 10ˣ | `unitless` (warn if x has a dim) |
| `ln`, `log` | `ln(x)` | natural log, log₁₀; x ≤ 0 → invalid | `unitless` |
| `int` | `int(x)` | truncate toward zero; result `integer=true` | `same_as:x` |
| `frac` | `frac(x)` | x − int(x) | `same_as:x` |
| `round` | `round(x)` | nearest, halves away from zero **[compat]** | `same_as:x` |
| `round_down`, `round_up` | | floor, ceil | `same_as:x` |
| `remainder` | `remainder(x, y)` | `std::fmod(x, y)`: sign of x, \|f\| < \|y\|; y = 0 → invalid | `same_as:x` |
| `sgn` | `sgn(x)` | −1 / 0 / +1 | `unitless` |
| `min`, `max` | `min(x, y)` | elementwise; if exactly one operand is invalid the other is returned **[compat]** | `unify:x,y` |
| `rand_val` | `rand_val()` | one value in [0,1), fixed per evaluation: `splitmix64(defHash ^ sourceId)` so results are reproducible | `unitless` |
| `rand_chan` | `rand_chan()` | per-sample value in [0,1): `splitmix64(seed, sampleIndex)`; chunk-order independent | `unitless` |
| `sin cos tan` | `sin(x)` | x in radians; a `[deg]` reference is converted first by the `Convert` node | arg `angle`, result `unitless` |
| `asin acos atan` | | result radians (`angle`); domain errors → invalid | `angle` |
| `atan2` | `atan2(y, x)` | four-quadrant | `angle` |
| `sinh cosh tanh asinh acosh atanh` | | hyperbolic | `unitless` |
| `bit_and`, `bit_or`, `bit_xor` | `bit_and(x, y)` | integer operands required; computed in 64-bit, result `integer` | `same_as:x` |
| `bit_not` | `bit_not(x)` | 32-bit two's-complement complement **[compat]** | `same_as:x` |
| `bit_decode` | `bit_decode(x, bit)`, `bit_decode(x, lowBit, count)` | `(x >> bit) & 1` or `(x >> lowBit) & ((1<<count)−1)` **[compat: arity and argument order are guesses]** | `unitless`, `integer` |
| `choose` | `choose(c, a, b)` | per sample: c ≠ 0 → a else b; c invalid → invalid; validity of the chosen branch only; nestable | `unify:a,b` |
| `invalid` | `invalid()` | all samples invalid | `Invalid` kind |
| operators | `+ - * / ^`, relational, `! && \|\|`, `~ & \|` | §3.2; `/` by zero → invalid; relational and equality on `Text` vs `Text`: `==`/`!=` exact, `< <= > >=` lexicographic **[compat]** | §3.2 |

### 5.2 Windowed (class `Windowed`, extent `halo ⌈n/2⌉`; on the v1.0 chunk path chunk-independent while the plan's cumulative halo (§6.2) ≤ 65,536, else `whole`)

| Function | Signature | Semantics | Unit |
|---|---|---|---|
| `smooth` | `smooth(x, n)`, `smooth(x, t)` | Unweighted moving average over n points at the result rate; odd n centred (`[i−h, i+h]`), even n biased one point earlier (`[i−n/2, i+n/2−1]`); seconds → odd n per §2.4; n ∈ {0, 1} → n = 2 (window `[i−1, i]`; i2 treats widths 0 and 1 as invalid and uses 2), including an n of 1 produced by the seconds conversion when `t·rate < 2`; `x` is resampled to the result rate with `Linear` whatever its `Interp` (i2 smooth help; §6.4 rule 6); invalid samples are excluded from the average, a window with no valid sample is invalid; at the extent edges the window is truncated to the available samples **[compat: edge and invalid handling]** | `same_as:x` |
| `derivative` | `derivative(x)`, `derivative(x, n)`, `derivative(x, t)` | Slope of the least-squares line through the window's valid points versus time; default n = 2 (current and previous sample, i.e. `(x[i] − x[i−1])·rate`); n = 0 or 1 → 2; odd/even window placement as `smooth`; fewer than 2 valid points → invalid **[compat: edge handling]** | `per_time:x` |
| `derivative` (w.r.t. channel) | `derivative(x, y)`, `derivative(x, y, n)` | y is a Series: least-squares slope of x versus y over the window (default 2 points); Δy = 0 → invalid **[compat: form inferred from release note 1.0.5]** | `dim(x) − dim(y)` |
| Filter item | §4.4 | `smooth` on the chain input (same window, n ∈ {0, 1} and `Linear` resampling rules) | `same_as` |

Windowed kernels compute in F64 accumulators (running sums with periodic re-summation every 4,096 samples to bound drift) and emit F32 unless the plan is F64.

### 5.3 IIR (class `IIR`; designed in-tree, D-169)

| Function | Signature | Semantics | Extent | Unit |
|---|---|---|---|---|
| `filter_lp`, `filter_hp` | `(x, fc)`, `(x, fc, order)` | First-order RC low/high pass, cutoff `fc` Hz at the result rate; `order ∈ {1,2,3}` = that many cascaded forward passes (default 1); `−1` = one backward pass (reverse phase delay); `−2` = forward then backward (zero phase). Other orders → `E-ARG-VALUE` | orders 1..3: `ordered`; −1, −2: `whole` | `same_as:x` |
| `filter_cheby_lp`, `filter_cheby_hp` | `(x, fc)`, `(x, fc, order)`, `(x, fc, order, ripple)` | Chebyshev type I (designed in-tree, D-169; readings `iir.*` of `docs/maths-compat.md`); `order ∈ {2,4,6}` forward, `{−4,−8,−12}` = forward+backward at half the order each way (zero phase); order omitted → 2 (i2 default); `ripple` in % of passband (default 0 → Butterworth) | positive: `ordered`; negative: `whole` | `same_as:x` |
| `filter_cheby_bp`, `filter_cheby_bs` | `(x, f_lo, f_hi)`, `(…, order)`, `(…, order, ripple)` | Band pass / band stop, centre `√(f_lo·f_hi)`, width `f_hi − f_lo`; order and ripple rules as above | as above | `same_as:x` |

Initial conditions **[compat]**: forward passes are primed with the first valid sample as steady state (no start-up transient); backward passes with the last valid sample. Invalid samples: the filter holds its state through a gap and the gap samples stay invalid; gaps longer than 1 s re-prime the filter at the next valid sample. `fc ≥ rate/2` → `E-ARG-VALUE` (Nyquist). Ordered forward evaluation (v1.0 chunk path, §6.0) carries the filter state from chunk k to k+1; the provider therefore evaluates chunks in order on one worker, seals each chunk as it completes so early chunks are visible before the whole extent finishes, and keeps a state checkpoint at every chunk boundary (§6.2) so an evicted chunk is rebuilt from its predecessor's checkpoint alone.

### 5.4 Stateful (class `Stateful`, extent `whole`, one worker, sequential)

Gating/reset conventions shared by `integrate*`, `stat_*`, `running_*`: `cond` (Bool, optional) gates accumulation (invalid cond = false); `reset` (any numeric, optional) starts a new segment whenever its integer part **increases** relative to the previous sample (so `range_change("Outings:Laps")` restarts per lap and a constant never resets). Segment boundaries also occur at the extent start.

| Function | Signature | Semantics | Unit |
|---|---|---|---|
| `integrate` | `integrate(x)`, `(x, cond)`, `(x, cond, reset)` | Σ over the segment of `x[i]·dt` (rectangle rule at the result rate, F64 accumulator) while `cond`; output holds while `cond` is false; invalid `x` contributes 0 and keeps the output valid **[compat]** | `times_time:x` |
| `integrate_dist` | `integrate_dist(x)`, `(x, cond)`, `(x, cond, reset)` | Σ `x[i]·ΔD[i]` where ΔD is the increment of the source's `Corr Dist` (base m, resampled `Linear`); requires the corrected-distance module → `E-NO-DISTANCE` otherwise **[compat: arities]** | `times_length:x` |
| `stat_min`, `stat_max`, `stat_mean`, `stat_std_dev` | `(x)`, `(x, cond)`, `(x, cond, reset)` | Two-pass per segment: the statistic over the segment's gated valid samples is written to **every** sample of the segment (segment-constant, i2 "constant over the window"); population std-dev; a segment with no gated sample → invalid. Ungated (no `reset`) = one segment = the math window (§7.6) → kind `Scalar`, `SelectionDependent`; with `reset` → kind `LapScalar` **[compat]** | `same_as:x` (`std_dev` too) |
| `stat_start`, `stat_end` | same arities | First / last gated valid sample of the segment, segment-constant | `same_as:x` |
| `running_min`, `running_max` | `(x)`, `(x, cond)`, `(x, cond, reset)` | Cumulative extreme to date within the segment (not two-pass); holds while gated off **[compat: names are guesses; `MathsCompat::functionAliases` lets the i2 importer rename once the real names are known]** | `same_as:x` |
| `flip_flop` | `flip_flop(set, reset)` | SR latch: output 1 from a rising edge of `set` until a rising edge of `reset`; both on the same sample → reset wins; initial 0 **[compat]** | `unitless` Bool |
| `edge_delay` | `edge_delay(x, t, rise)`, `edge_delay(x, t, fall)` | `rise`: output 1 only once x has been continuously 1 for ≥ t s; `fall`: output 0 only once x has been continuously 0 for ≥ t s; t converted to samples at the result rate (ceil) | `unitless` Bool |
| `time_valid` | `time_valid(x, t)` | ≡ `edge_delay(x, t, rise)` | `unitless` Bool |
| `time_shift` | `time_shift(x, t)` | y[i] = x[i − round(t·rate)]; positive t delays (moves data later); samples shifted in from outside the extent are invalid **[compat: sign]** | `same_as:x` |
| `pulse_width` | `pulse_width(x)` | Width in seconds of the most recently **completed** high pulse of Bool x, held until the next completes; invalid before the first completed pulse **[compat]** | time (`s`) |
| `previous_sample` | `previous_sample(x)`, `previous_sample(x, n)` | x[i−1] / x[i−n] at the result rate (n ≥ 1 `IntLit`); i < n → invalid. Self-reference (`'A' = previous_sample('A') + 1`) is a cycle in the MVP (`E-CYCLE`); recursive evaluation is deferred to the telemetry point evaluator (v1.1+) | `same_as:x` |

On the v1.0 chunk path the single-pass Stateful kernels (`integrate*`, `running_*`, `flip_flop`, `edge_delay`, `time_valid`, `pulse_width`, `time_shift`, `previous_sample`) run `Ordered` (sequential on one worker, as D-022 requires) with `saveState`/`loadState` checkpoints (§6.2); the two-pass `stat_*` stay `Whole`. Optional optimisation (not required for M2): `time_shift` and `previous_sample` may be reclassified `Windowed` with `halo = shift` when `shift ≤ 65,536`; outputs must be bit-identical to the sequential path (property test §14.2).

### 5.5 Range (class `Range`, extent `halo 0`, chunk-independent)

| Function | Signature | Semantics | Unit |
|---|---|---|---|
| `range_is` | `range_is("Group:Path")` | 1 while the sample time lies in `[start, end)` of any enabled range of the group, else 0; evaluated from `BlockCtx::ranges` (interval table, binary search per block boundary, then linear) | `unitless` Bool |
| `range_change` | `range_change("Group:Path")` | Integer count of range starts at or before the sample (0 before the first start); increments by 1 at every range start, so for contiguous groups it is the lap ordinal | `unitless`, `integer` |

Both depend on the version of the range group they name (`rangeVersion(groupPath)`, §7.2; marker edits rebuild ranges, D-014), not on selection, on other groups or on `DataSource::version`.

### 5.6 Variance (class `Variance`, extent `halo 0`, `SelectionDependent`, D-023)

| Function | Signature | Semantics | Unit |
|---|---|---|---|
| `variance_dist` | `variance_dist(x)` | For sample i of the Main range at time tᵢ: `x_main(tᵢ) − x_ref(t_ref)` where `t_ref = refDist.timeAt(mainDist.distAt(tᵢ))` (same distance from lap start, via the two `DistanceIndex` tables incl. alignment offsets and stretch); invalid outside the Main range, when no reference overlay exists, or when either lap has no `DistanceIndex` **[compat: "variance of a channel vs the reference"]** | `same_as:x` |
| `variance_time` | `variance_time(x)` | `x_main(tᵢ) − x_ref(ref.start + (tᵢ − main.start))` (same time from lap start, with alignment offsets) | `same_as:x` |

The reference overlay is `MathSelection::reference` (§7.5), which `MathsService` fills from `SelectionSnapshot::referenceOverlay()` (Overlay 0 if set, else the lowest-numbered overlay, D-017). With several overlays selected the functions still evaluate (i2 1.1.0 fixed their failure with multiple overlay selections) against that one reference and stay invalid outside the Main range (D-023); per-overlay variance traces are the F3 graph feature (D-023, 04), not these functions **[compat: whether i2 also fills the overlay ranges of a variance channel is unobserved; one oracle row in M2, permitted since U-03 was answered yes on 2026-10-10 (`OPEN_QUESTIONS.md` E-3)]**. When the reference lap is in another source, `x_ref` is read from that source's chain head for the same name through `EvalContext`.

### 5.7 Table (class `Table`, extent `halo 0`)

Not callable from expressions; produced by `table2d`/`table3d` items (§4.4). `TableKernel` evaluates the axis plan(s) as elementwise sub-plans, then interpolates.

---

## 6. Compilation and evaluation

### 6.0 Staging: 0.1 whole-node path, v1.0 chunk path (staging amendment to D-022, DECISIONS.md Addendum A)

D-001 adopts performance-first's interfaces "with the cheapest implementation that satisfies the MVP", and the MVP maths budgets (07 §12: elementwise 2.16 M samples ≤ 10 ms one core; `filter_lp` −2 on 21.6 M ≤ 150 ms; 20 maths channels on RW-2 ready ≤ 2 frames on 8 cores) do not need chunk-parallel evaluation. Through 0.1 (M1–M4):

1. Every plan runs as `ExtentMode::Whole`: one `TaskPool` task per node (key `…/all`, §7.4) evaluates the node over the whole layer extent into `std::vector` buffers with validity, then seals the output into 65,536-sample chunks through the ordinary chunk and pyramid builder into `ChunkCache` under the node's cache key (§7.2). `PlanProvider : IChunkProvider` is therefore not chunk-independent in 0.1.
2. Elementwise subtrees still run block by block (4,096 samples, `BlockCtx`), but `EwKernel` interprets the fused subtree with one tight auto-vectorised loop per operator over block temps; the register bytecode of §6.1 is the v1.0 implementation.
3. Windowed, IIR (all orders), Stateful, Range, Variance and Table stages run sequentially over whole-extent buffers; there are no per-chunk halos, no `Ordered` mode and no checkpoints.
4. Parallelism is across nodes only: independent nodes run on different workers; a node starts when its upstream nodes are sealed.
5. Every 0.1 maths output is one eviction group (§6.2): once eviction exists (M5, D-043) it is evicted all or none and recomputed by one task.
6. `Plan`, `Kernel`, `BlockCtx`, `ExtentMode`, `PlanProvider` and `EvalContext` are the 0.1 interfaces unchanged; the chunk-parallel path — register bytecode (§6.1), cumulative halos and stage buffers (§6.2), `Ordered` state carry with checkpoints (§5.3, §5.4, §6.2), per-chunk tasks (§7.4) and the chunk-order determinism property (§6.7, §14.2 #3) — is built at v1.0, when the v1.0 budgets (elementwise ≤ 3 ms, RW-1 under eviction) require it.

### 6.1 Plan, Kernel, BlockCtx (D-022)

```cpp
enum class KernelClass : std::uint8_t { Elementwise, Windowed, IIR, Stateful, Range, Variance, Table, External };
enum class ExtentMode  : std::uint8_t { Parallel /*chunk-independent*/, Ordered /*state carry, in order*/, Whole };

struct BlockCtx {
    SampleIdx first; std::uint32_t n;           // n ≤ 4096
    Micros t0; Rate rate;                       // of the result
    std::shared_ptr<const analysis::RangeTables> ranges;          // interval tables per group path (01 §5.3 rule 9)
    std::shared_ptr<const analysis::DistanceIndex> mainDist, refDist;   // may be null
    ArenaView temps;                            // bump allocator, reset per block
    CancelToken cancel;
};
struct Kernel {
    virtual ~Kernel() = default;
    virtual KernelClass cls() const = 0;
    virtual void run(BlockCtx&, std::span<const BlockView> in, BlockView out) = 0;   // NaN/validity-aware
    virtual void reset(const EvalContext&) {}   // Stateful/IIR: called at segment/extent start
    virtual std::uint32_t halo() const { return 0; }
    virtual ExtentMode extent() const { return ExtentMode::Parallel; }
};
struct BlockView { void* data; std::uint8_t* validity /*nullptr = all valid*/; store::Dtype dtype; std::uint32_t n; };

struct Plan {
    std::vector<Stage> stages;                  // topological; Stage{kernel, inputs(stage ids or input ids), outTemp}
    Type out; store::Dtype outDtype;
    ExtentMode extent; std::uint32_t halo;      // plan-level context per side: longest-path sum of stage halos (§6.2); Ordered/Whole if any stage is
    std::vector<PlanInput> inputs;              // {ChannelKey key; store::Interp interp; units::Converter conv; bool f64;}
    bool selectionDependent;
    Hash128 defHash;                            // §7.2
};
Result<Plan> compile(const Ast&, const Scope&, const MathItem&, const MathsCompat&, Diagnostics&);
```

Compilation: maximal elementwise subtrees (operators, §5.1 functions, `Convert`, constant folding, the final `resultUnit → base` conversion and dtype cast) fuse into one `EwKernel`; its v1.0 implementation runs a register bytecode (`Op{code, dst, srcA, srcB, imm}`) in a switch-free loop over 4,096-sample blocks (0.1: one loop per operator, §6.0); integer-typed registers are F64 lanes (exact to 2⁵³) so bitwise ops on 32-bit logged fields are exact. Non-elementwise functions become their own stage. Elementwise stages are what the budgets in §14.3 measure (2.16 M samples ≤ 10 ms MVP).

### 6.2 Stage buffers and halos (v1.0 chunk path; 0.1 per §6.0)

`PlanProvider` implements the v1.0 range form of the provider call recorded in 02 §3.2 (staged refinements), `materialise(ChunkRange, ChunkSink&, CancelToken)`, so one task can seal one chunk (`Parallel`) or many (`Ordered`, `Whole`).

**Cumulative halo.** Chained windowed stages need the sum of their halos, not the maximum. At compile time the extension of each stage is computed top-down: `ext(root) = 0`; for each input stage `u` of stage `s`, `ext(u) = max(ext(u), ext(s) + halo(s))`; `Plan::halo = max over leaf inputs of (ext(leaf) + halo(leaf))`, the longest-path sum, which is also how far each `MathInputGatherer` reads beyond the chunk. For chunk `[c0, c1) = [chunkNo·65,536, (chunkNo+1)·65,536)` stage `s` evaluates `[c0 − ext(s), c1 + ext(s)) ∩ [0, extent)`. Only the true layer extent counts as an edge for the truncation rules of §5.2; a chunk seam never does. Example: `smooth(smooth(x, 5), 5)` evaluates the inner stage on `[c0 − 2, c1 + 2)` and reads `x` on `[c0 − 4, c1 + 4)`. A plan whose `Plan::halo > 65,536` compiles `Whole`.

Each stage writes into a `StageBuffer` sized to its extended range (F32: ≤ 262 KB + 8·ext bytes); elementwise stages stream through their inputs in 4,096-sample `BlockCtx` steps (L1-resident with the temps arena); Windowed/IIR/Table/Range stages consume whole stage buffers once. The chunk's output is the central 65,536 samples; the chunk's `MinMaxPyramid` is built before sealing.

**Ordered.** `Ordered` plans run chunk 0, 1, 2 … on one worker with kernel state preserved between chunks. Every `Ordered` kernel implements `saveState()`/`loadState()`, and the provider keeps a checkpoint at every chunk boundary (IIR: ≤ 2·order doubles per section; `integrate*`: one double; `flip_flop`, `edge_delay`, `pulse_width`: a few bytes), held with the node (not in `ChunkCache`) and dropped with its version. An evicted chunk k is rebuilt from checkpoint k−1 plus input chunk k (and the upstream halo) instead of re-running chunks 0..k, so eviction cannot thrash an Ordered output.

**Whole.** `Whole` plans (zero-phase and backward IIR, two-pass `stat_*`, non-block-capable external functions, plans whose halo exceeds 65,536, and every 0.1 plan) evaluate the entire extent into stage buffers (memory ≈ stages × extent × 4 B; above 256 MB the provider spills stage outputs to temporary chunks in `ChunkCache`) and then emit every chunk. Their output chunks form one **eviction group** in `ChunkCache`: evicted all or none, with an LRU weight carrying the measured recompute cost, so a 21.6 M-sample zero-phase filter output is never thrown away for one chunk's worth of memory without that cost being weighed. 02 §3.2 behaviour 7 then reads: chunks `0..last` for `Ordered` (from the nearest checkpoint), the whole extent for `Whole` (XREF 02).

### 6.3 `ResampleView`

```cpp
class MathInputGatherer {                // one per PlanInput; wraps store::ResampleView (02 §3.3, owned by esses_store)
public:
    MathInputGatherer(const store::Column& src, const store::Series& series, Rate out, store::Interp, units::Converter, CancelToken);
    void fill(SampleIdx first, std::uint32_t n, BlockView out);   // pins source chunks via Column::require, gathers a
                                                                  // store::ResampleView (Resampler::block), converts into out
};
```

`esses_store` owns `ResampleView` and `Resampler::block` (02 §3.3); `MathInputGatherer` is the maths-side wrapper that adds pinning, the input's unit `Converter` and the `BlockView` output. `fill` pins its input chunks through `Column::require`, which materialises an absent chunk inline on the calling worker (01 §5.1 rule W (a)); when the input is itself a maths column, its `PlanProvider` therefore runs nested on the same thread, bounded by the depth of the acyclic maths DAG (rule W (c)). A worker never waits for a queued upstream task.

The gather rules below are implemented by `Resampler::block`/`ResampleView` in `esses_store` and stated here because maths compatibility depends on them. Uniform sources: output sample `i` (absolute index `first + i`) has time `t_i = out.timeAt(first + i, t_out0)` (02 §3.3, D-008 rounding); `j = in.indexAt(t_i, t_in0)` is the last input sample at or before `t_i` (the exact integer inverse of `Rate::timeAt`). `Previous` takes `j`; `Next` takes `j` when `in.timeAt(j, t_in0) == t_i`, else `j + 1`; `Closest` compares `t_i − t_j` with `t_{j+1} − t_i` (earlier on a tie, 02 §3.3 rule 13); `Linear` blends `j` and `j + 1` with `w = double(t_i − t_j) / double(t_{j+1} − t_j)`. Because output times come from `Rate::timeAt`, a block equals per-sample `Resampler::valueAt(timeAt(i))` (02 §3.3 rule 16). The implementation steps a 128-bit numerator per sample instead of dividing per sample; property test §14.2 #6. `TimestampedSeries` sources binary-search the I64 µs column per block start then walk. Samples outside the source extent, and any gather touching an invalid source sample, are invalid. Downsampling (explicit rate below the input rate) uses the same rule (point sampling, no anti-alias filter) **[compat]**. Nothing is materialised beyond the requested block.

### 6.4 Rate and extent selection

1. Explicit `rate` → result rate. `E-RATE-ZERO` for 0 (`"auto"` is the JSON spelling of i2 `SampleRate="0"`).
2. `auto`: highest `Rate` among Series inputs (a channel read only by a statistic included), compared exactly (`a.num·b.den` vs `b.num·a.den`). `TimestampedSeries` inputs contribute their descriptor's nominal rate (importer-provided; else `round(1/median interval)` Hz, as built `round(1/mean interval)` Hz because the owning thread never reads samples, D-185 (m)). Scalars/constants/details do not count.
3. No Series input (constants only, `rand_chan()`): result rate = the layer's highest logged rate; a pure `Scalar` result is materialised as a constant series at that rate (so constants appear as channels in the Channel Editor, i2 behaviour).
4. Extent: every maths output spans the layer extent `[layerStart, layerEnd)` at the result rate, `t0 = layerStart`; samples where an input has no data are invalid.
5. Window seconds → points use the result rate (§2.4).
6. Mixed-rate inputs are resampled per §6.3 using each channel's `Interp` (override from the Channel Editor wins over the descriptor), except the first argument of `smooth` and the input of a Filter item, which are always resampled `Linear` (i2 smooth help: points are first re-sampled to the maths rate by linear interpolation; recorded in `docs/maths-compat.md`). `I-RATE-MIXED` lists the inputs and their rates.

### 6.5 Precision and dtype

Buffers are F32 unless: the module/item `precision = "double"`, any input is F64 (GPS lat/lon, timestamps, `Corr Dist`), the plan contains an integrator, or the result dtype is F64. Accumulators (`smooth`, `stat_*`, `integrate*`, IIR state) are always F64. Output dtype: `I32` for a Bool result, an enumeration, or an integer-typed result that provably fits 32 bits (integer channels and constants through `+ -`, the unary and bitwise operators, `choose`, `min`, `max`, `abs`, `remainder`, the integer roundings and the order-preserving stateful functions); every other integer-typed result is `F64` (exact to 2^53, never saturated); an integer or Bool result converted from a declared non-base unit is real; `F64` under the rules above; else `F32` (D-185 (b)). Project default precision is open question Q-09 of DECISIONS.md.

### 6.6 Validity

Outputs use the chunk `Validity` modes of D-009: a block with no invalid sample is written without a bitmap; otherwise the provider sets bitmap bits. Float columns additionally carry NaN at invalid samples so NaN-aware consumers need no bitmap read. Rules: `invalid()` clears; operators and elementwise functions propagate (§5.1 exceptions: `choose`, `min/max`); windows and statistics skip invalid samples; range functions never produce invalid; tables per `outOfRange`.

### 6.7 Determinism

For a fixed `(definition, inputs, compat, precision)` the output is bit-identical regardless of worker count, block size and repeat runs (0.1, §6.0) and, on the v1.0 chunk path, also regardless of chunk evaluation order and of whether a chunk was rebuilt from a checkpoint; the chunk-path guarantee depends on the cumulative halo (§6.2). Tested (§14.2 #3): from M1, 4,096 vs 1,024 block sizes and repeat runs; at v1.0, random chunk order vs whole-extent, checkpoint rebuild vs sequential, and `smooth(smooth(x, 5), 5)` across samples 65,535/65,536. Random functions are counter-based (§5.1). Windowed sums re-sum per block to avoid order-dependent drift.

---

## 7. Graph, cache, scheduling and status

### 7.1 `MathGraph`

```cpp
class MathGraph {                                    // one per (DataSource, layer); mutated on the owning thread only
public:
    MathGraph(std::shared_ptr<const model::DataSource>, std::string layer, const FunctionRegistry&, const units::UnitRegistry&, MathsCompat);
    NodeId   define(const MathModule&, const MathItem&, Diagnostics&);          // (re)compiles; returns the node
    void     remove(NodeId); void setEnabled(NodeId, bool); void setModuleEnabled(std::string moduleId, bool);
    void     setModules(std::span<const MathModule> globalInOrder, const MathModule* local, const MathModule* transient);
    void     setSource(std::shared_ptr<const model::DataSource>);               // new snapshot → re-hash node versions from what each node reads (§7.2)
    void     setSelection(const MathSelection&);                                 // SelectionDependent nodes re-key (§7.5)
    Version  version(const ChannelKey&) const;
    Future<ColumnHandle> require(const ChannelKey&, SampleRange, Priority, CancelToken);
    MathStatus status(const ChannelKey&) const;
    std::vector<ChannelKey> consumersOf(const ChannelKey&) const;
    std::vector<ChannelKey> outputs() const;                                     // for Channel Editor/Status
    const ResolutionTrace& trace(const ChannelKey&) const;
};
```

### 7.2 Versions and cache keys (D-022)

```
defHash     = xxh3(Ast::sourceHash | item kind, rate, resultUnit, interp, precision, table data, settings JSON, compat switches, engineVersion)
version(n)  = xxh3(defHash(n), enabled(n), moduleEnabled, conditionResult, chainIndex,
                   sorted(version(upstream nodes)),
                   store.version(c) for exactly the logged columns c that n reads,
                   rangeVersion(g) for each range group path g that n reads      // only when the plan has Range or Variance
                                                                                 // kernels or a range-gated argument
                   xxh3(values of the details and constants n reads))
cacheKey    = (ChannelKey, version)                       for ordinary nodes
            = (ChannelKey, version, selectionKey)         for SelectionDependent nodes
selectionKey= MathSelection::key = xxh3(main RangeRef, reference RangeRef, alignment offsets and stretch of both, math window)
```

`DataSource::version` (which bumps on every sidecar commit: annotations, video links, detail edits, trust flags) is never part of a node version, and range versions are kept per group path (`Outings:Laps` separate from each `Track Sections` group; XREF 02 `RangeGroupTree`). An annotation therefore invalidates no maths output; a marker edit invalidates only nodes that read the affected groups; and `Corr Dist`, which reads no range group, is not invalidated when the track-section ranges built from its own `DistanceIndex` are rebuilt, which removes the `Corr Dist → DistanceIndex → sections → ranges → Corr Dist` rebuild loop. For the same reason `DistanceIndex` is keyed by `(Corr Dist column version, lap start/end marker ids and times, stretch settings)` rather than `source.version` (refinement of D-016 and of D-022's "source version" term, DECISIONS.md Addendum A; XREF 01 §7.1).

`PlanProvider : store::IChunkProvider` writes result chunks into the process-wide `ChunkCache` under the cache key; maths outputs are therefore plotted, pyramided and (from M5, D-043) evicted like logged data. An evicted chunk is recomputed on the next `require` without changing status: a `Parallel` chunk alone, an `Ordered` chunk from the nearest checkpoint, a `Whole` output (every 0.1 output) as its eviction group (§6.2). Editing an item changes its `defHash`, so every downstream `version` changes by construction and old chunks age out; sibling channels keep theirs. A selection change re-keys only `SelectionDependent` nodes (ungated `stat_*`, `variance_*`, and anything downstream of them, which inherits the flag); chunks for a previous selection stay cached, so toggling overlays back is free.

### 7.3 Status model

```cpp
enum class MathState : std::uint8_t { Pending, Evaluating, Ok, Warning, Error, Disabled };
struct MathStatus { MathState state; Diagnostics diags; std::string summary; ChannelKey resolvedInput; Version version; float progress; };
```

| State | Meaning | Channel Status text |
|---|---|---|
| `Pending` | defined, compiled, not yet demanded or invalidated | "Pending" |
| `Evaluating` | at least one chunk task running (`progress` = sealed chunks / total) | "Calculating 37 %" |
| `Ok` | all demanded chunks sealed, no diagnostics above Info | "OK" |
| `Warning` | evaluated, with Warning/Style diagnostics (dimension mismatch, duplicate ignored, compat guesses) | first warning message, "+n more" |
| `Error` | cannot evaluate: syntax, unknown channel/function, cycle, arity, Nyquist, missing distance, plugin unavailable | error message with position |
| `Disabled` | item or module disabled, or module `condition` false for this source | "Disabled" / "Disabled (condition)" |

`model::Channel::status` (outline: `Ok | Pending | Error`) is derived: `Ok` for `Ok`/`Warning`, `Pending` for `Pending`/`Evaluating`, `Error` for `Error`/`Disabled`. Transitions: define → Pending; `require` → Evaluating → Ok/Warning (or Error on a runtime failure such as `E-RANGE-UNKNOWN`); edit → Pending for the node's cone; `setSource` → Pending for nodes whose version changed (logged columns, range groups, details or constants they read, §7.2); `setSelection` → Pending for `SelectionDependent` nodes only. Status is written on the owning thread only (the GUI thread, or the CLI main thread; 01 §5.3 rule 10): a worker never touches `MathGraph`. It posts its completion (sealed chunk range, diagnostics) through `Dispatcher::post(context, …)`, keeps progress in atomics inside the `PlanSnapshot` it holds (read by the GUI on a timer for `progress`), and the posted GUI closure applies the transition and emits `StatusChanged{key}`, so the Channel Status window and the Maths Editor update without polling.

### 7.4 `MathsScheduler`

Qt-free; owned by `MathGraph`'s host (`MathsService` in the GUI, the CLI directly). `require(key, range, prio, cancel)`: (1) resolve the node and its topological upstream; (2) for each node whose chunks in `range` (plus `Plan::halo`) are missing from the cache, enqueue tasks at `prio` — 0.1 (§6.0): one task per node covering the extent; v1.0: `Parallel` plans one task per chunk, `Ordered`/`Whole` plans one task covering the extent (`Ordered` from the nearest checkpoint); (3) coalesce through `TaskPool` share-keys (D-031, 01 §5.4) that carry the chunk: `maths/<src>/<layer>/<channelKey>/<version>[/<selectionKey>]/c<chunkNo>` for a `Parallel` chunk task and `…/all` for a task that produces every chunk (`Ordered`, `Whole`, every 0.1 task). In code the key is a struct `MathTaskKey { Hash128 node /*xxh3 of the cache key*/; std::uint32_t chunk /*kAll = UINT32_MAX*/; }` with a precomputed hash; the string form is for logs only (XREF 01 §5.4). Ten components requesting the same chunk schedule it once, two different chunks of one node are two tasks, and a chunk request while the node's `/all` task is queued or running waits on that task; (4) order within a priority band by distance from the cursor chunk; (5) upstream tasks complete before dependents start: every task captures the `std::shared_ptr<const PlanSnapshot>` of the dependency cone taken at `require` time (01 §5.3 rule 9), an upstream input that is still absent is materialised inline through `Column::require` (01 §5.1 rule W), and continuations scheduled by a completing task are submitted from the GUI-posted completion closure, never from the worker (01 §5.3 rule 10); (6) `CancelToken` is checked per block; a cancelled chunk is simply not sealed. **Priorities** are the requester's, taken from the single table in 01 §5.2; `MathsScheduler` defines no bands of its own (D-016 fixes the `Corr Speed`/`Corr Dist` prefetch at `Background`). The GUI thread never evaluates; the CLI may wait synchronously.

### 7.5 `EvalContext`

```cpp
struct MathRangeGeom { model::RangeRef ref; Micros start, end, offsetUs; double offsetM; double stretch; };   // copied from model::Range
struct MathSelection {                       // maths-local selection copy; esses_maths (layer 2) cannot see plotcore::SelectionSnapshot (layer 3)
    MathRangeGeom main;
    std::optional<MathRangeGeom> reference;  // SelectionSnapshot::referenceOverlay() (D-017)
    std::vector<model::RangeRef> overlays;   // Overlay 0 then Overlays 1..n, for the per-source math window (§7.6)
    Hash128 key;                             // selectionKey (§7.2) over main, reference (offsets, stretch) and overlays, from which each source's math window follows; computed once by MathsService
};

struct EvalContext {
    std::shared_ptr<const model::DataSource> source; std::string layer;
    MathSelection selection;                                     // copied by value (D-031); built by MathsService from the SelectionSnapshot
    std::shared_ptr<const analysis::DistanceIndex> mainDist, refDist;   // may be null; never a raw pointer (01 §5.3 rule 9)
    Micros mathStartUs, mathEndUs;                               // math window
    Rate resultRate; SampleIdx resultFirst; std::uint64_t resultCount;
    std::shared_ptr<const analysis::RangeTables> ranges;
    std::shared_ptr<const units::UnitRegistry> units;            // process-lifetime registry, shared
    std::function<Result<MathInputGatherer>(const ChannelKey&, units::UnitId, Rate)> getChannel;   // for Variance and External
    CancelToken cancel;
};
```

`EvalContext` holds no C++ reference or raw pointer to GUI-lifetime state (01 §5.3 rule 9): shared inputs are `std::shared_ptr<const …>` snapshots (a rebuild publishes a new pointer and never mutates the old object), and the services a task calls back into (the `TaskPool` and the log sinks) are process-lifetime objects of the runtime (`TaskPool::instance()`, `Log`; 01 §5.3 rule 11), not members.

### 7.6 Math window

For source `S`: the Main range if Main is in `S`; else the earliest-numbered selected overlay in `S`; else the whole layer extent **[compat]**. Ungated `stat_*` compute over the window and broadcast over the whole extent; `variance_*` are invalid outside it. The window is part of `selectionKey`.

---

## 8. Channel descriptor of a maths output

`MathGraph` publishes a `model::ChannelDescriptor` per output: `name` = item name, `source = ChannelSource::Maths`, `unit = resultUnit` (base-stored values, §3.2 rule 11), `quantity`, `dtype` (§6.5), `kind` (`Enum` with the enumeration's `EnumTableId`, `Integer` for integer results, `Bool` for Bool results, else `Real`), `interp`, `dps`/`colourIndex` from `display` (overridable in `channels/overrides.json`), `statusText` from `MathStatus::summary`. `ChannelId` follows the derivation table of 02 §4.1 verbatim: `uuidFromHash(hash128({"maths", source content hash hex, layer name, canonical name, "module:" + module id}))` for global modules and `uuidFromHash(hash128({"maths", source content hash hex, layer name, canonical name, "local"}))` for local (sidecar) maths; built-in and transient modules use the global form with their module id (`"module:esses.corrected_speed_distance"`, `"module:~transient"`; XREF 02 §4.1). The source content hash is `SourceId` exactly as 02 §4.1 defines it. Self-maths outputs reuse the shadowed channel's descriptor with `source = Maths` and keep its overrides (so DPS is preserved for scale/offset, i2 rule).

---

## 9. Built-in modules (D-016, D-024)

```cpp
class IBuiltinModule {
public:
    virtual std::string id() const = 0; virtual int version() const = 0;
    virtual const JsonDoc& settingsSchema() const = 0;               // schemas/builtin-<id>-<version>.schema.json
    virtual Result<std::vector<BuiltinOutput>> expand(const JsonDoc& settings, const Scope&, Diagnostics&) = 0;
};
struct BuiltinOutput { std::string name; std::variant<std::string /*expression*/, std::unique_ptr<Kernel>> body;
                       units::QuantityId quantity; units::UnitId resultUnit; DisplayDefaults display; store::Interp interp; store::Dtype dtype; };
```

Each output is an ordinary node in the chain (shadowing rules apply), shows in Channel Status, and is cached by the settings hash. Algorithms live in `esses_analysis`; `esses_maths` wraps them (dependency rule: analysis never depends on maths).

### 9.1 `esses.corrected_speed_distance` (v1; registered for every source on open, prefetched at `Background`)

```json
{ "kind": "builtin", "module": "esses.corrected_speed_distance", "version": 1, "enabled": true,
  "settings": {
    "method": "fastestWheel",
    "fastestWheel":  { "channels": ["Wheel Speed FL", "Wheel Speed FR"] },
    "alternateList": { "channels": ["Ground Speed", "Wheel Speed FL", "Wheel Speed FR", "Wheel Speed Front", "Wheel Speed", "Wheel Speed RL", "Wheel Speed RR", "Wheel Speed Rear", "Drive Speed", "GPS Speed", "Speed"] },
    "gCorrection":   { "speedChannel": "Wheel Speed FL", "longGChannel": "G Force Long", "lockThreshold": { "v": 2.0, "unit": "m/s" }, "recoverSeconds": 0.5 },
    "clamp": { "min": { "v": 0, "unit": "km/h" }, "minInvalidate": false, "max": null, "maxInvalidate": false },
    "distance": { "source": "integrate", "loggedChannel": "Lap Distance", "integration": "trapezoid" },
    "outputs": { "speed": "Corr Speed", "distance": "Corr Dist" } } }
```

Behaviour (numbered, implemented by `analysis::CorrectedSpeed::compute` and `analysis::CorrectedDistance::integrate`):

1. `fastestWheel`: per sample `max(a, b)` of the two channels (resampled `Linear` to the higher rate); one invalid → the other; both invalid → invalid. Missing channel → fall back to `alternateList` with `W-CSD-FALLBACK`.
2. `alternateList`: the first channel of the list that exists in the layer (after mappings/aliases) is used as-is. The default list ends with `Speed` (D-154, 2026-10-11: the sim-exporter dialect logs its road speed as `SPEED`; lookups are case-insensitive and a channel whose unit is not a speed is skipped). None → outputs `Error E-CSD-NO-SPEED`; the graph shows the "no distance data" banner for that source (D-016).
3. `gCorrection`: `v_pred[i] = v_corr[i−1] + a_long[i]·dt`; if `v_wheel[i] < v_pred[i] − lockThreshold` (wheel lock) or `v_wheel[i] > v_pred[i] + lockThreshold` (wheel spin) the output follows `v_pred`; otherwise it blends back to `v_wheel` linearly over `recoverSeconds`. Stateful kernel, F64. Documented as a method whose numbers are not bit-identical to i2 (outline §9 risk).
4. `clamp`: below `min` → `min` or invalid (`minInvalidate`); above `max` → `max` or invalid. Clamp values are stored with units and shown in the channel's display unit.
5. `Corr Speed`: quantity Speed, result unit `m/s`, display unit inherited from the first source channel's display unit; dtype F32; `interp Linear`.
6. `Corr Dist`: `distance.source = "integrate"`: cumulative `∫ v dt` over the layer extent (trapezoid; `"rectangle"` option), F64, invalid speed samples contribute 0 (hold) **[compat]**; `"logged"`: builds a monotonic cumulative distance from `loggedChannel` (converted to m) by unwrapping its resets, because a logged `Lap Distance` restarts near 0 at each beacon while `DistanceIndex` uses `CorrDist(t) − CorrDist(R.start)` and the outing graph uses cumulative distance: wherever `d[i] < d[i−1] − max(10 m, d[i−1]/2)` the previous sample's value is added to a running offset (`CorrDist[i] = offset + d[i]`); smaller decreases (noise) pass through, and `DistanceIndex` uses the first time each distance is reached anyway. An already-cumulative source never triggers the rule **[compat: reset threshold]**. Quantity Length, result unit `m`, display unit follows the speed unit (`km/h → km`, `mph → miles`, else `m`), overridable in the Channel Editor, which also changes the distance axis units.
7. Lap stretching is **not** applied to the channel; `DistanceIndex` holds the per-lap `stretch` scalar (D-016). The Corrected Speed/Distance dialog edits these settings and stores them in `project.json: builtinMaths["esses.corrected_speed_distance"]`; per-source overrides go to the sidecar as a `builtin` item.

### 9.2 `esses.gps_corrections` (v1; zero-fix step always on from M2; correction steps and the Corrected GPS dialog at v1.0, D-027)

```json
{ "kind": "builtin", "module": "esses.gps_corrections", "version": 1, "enabled": false,
  "settings": {
    "latitude": "GPS Latitude", "longitude": "GPS Longitude", "speed": "GPS Speed",
    "additionalChannels": ["GPS Heading", "GPS Altitude"],
    "statusChannel": { "name": "GPS Sats Used", "validWhen": ">= 4" },
    "duplicateRemoval": { "enabled": true },
    "timeShift": { "percentOfUpdatePeriod": 80 },
    "outputs": "shadow" } }
```

The module is registered for every source on open (like §9.1) whenever the `latitude`/`longitude` names resolve (through mappings and aliases). Step 1 runs whatever `enabled` says, from M2, because GPS track generation and the Track Editor GPS generation is an MVP feature (06 §13.5), the GPS-gate beacon insert follows at v1.0 (06 §13.3) and loggers write (0, 0) before the first fix; steps 2–4 run only when `enabled` is true, which needs settings from the v1.0 dialog or the v1.0 importer. Missing lat/lon → status `Disabled (no GPS channels)`, never an error (i2 1.0.21). Consumers — `TrackGenerator` ("Generate from GPS"), the Lap Editor GPS-gate auto-insert, `gps_track` and KML export — receive lat/lon as resolved by `MathsService`, i.e. the chain head, so (0, 0) fixes are invalid for all of them (XREF 04, 06).

1. Validity (always on, not a setting): samples where lat and lon are both exactly 0 are invalid on both channels (i2 marks both invalid to avoid map-scaling problems and excludes them from track maps and KML). When `enabled`, samples where the status expression is false invalidate `speed` (and lat/lon when `statusChannel.applyToPosition`).
2. Duplicate removal: runs of identical consecutive lat/lon/speed samples (receiver slower than the logging rate) are replaced by linear interpolation between the distinct neighbours on lat, lon and speed; the GPS update period is estimated as the median run length × dt.
3. Time shift: all listed channels are shifted earlier by `percentOfUpdatePeriod` % of the estimated update period (or `seconds` when given) using `time_shift` semantics with linear resampling.
4. `outputs: "shadow"` emits the corrected channels under the input names (self-maths; consumers see corrected data, i2 behaviour); `"prefix": "Corr "` emits `Corr GPS Latitude` etc. Outputs are F64 for lat/lon.

### 9.3 `esses.damper_velocity` (v1; ships at v1.0 with `suspension_histogram`, 07 §10.1)

```json
{ "kind": "builtin", "module": "esses.damper_velocity", "version": 1, "enabled": true,
  "settings": {
    "positions": [ { "channel": "Damper Pos FL", "output": "Damper Vel FL" }, { "channel": "Damper Pos FR", "output": "Damper Vel FR" },
                   { "channel": "Damper Pos RL", "output": "Damper Vel RL" }, { "channel": "Damper Pos RR", "output": "Damper Vel RR" } ],
    "filter": { "kind": "smooth", "seconds": 0.02 },
    "derivative": { "points": 3 },
    "bumpSign": "positive" } }
```

Expands to expression outputs `derivative(smooth('Damper Pos FL'[m], 0.02), 3)` (or `filter_lp` when `filter.kind = "filter_lp"` with `cutoffHz`), quantity Speed, result unit `m/s`, display `mm/s`, dps 0. `bumpSign` is stored for the suspension histogram (v1.0) and does not change the channel. The Velocity Calculation tab of that component can disable the module per component and use user maths instead.

---

## 10. Extension functions: `IMathFunction`, C ABI, Lua

### 10.1 Registry and interface (D-024)

```cpp
struct ArgSpec   { std::string name; enum { Scalar, Series, Text, RangeRef, Keyword } kind; std::string dim /*"any"|"time"|"length"|…*/; bool optional; };
struct FunctionManifest {
    std::string name;                     // "mylib.rolling_rms" (dotted for external), canonical lower-case
    std::string description;
    std::vector<ArgSpec> args; std::vector<std::uint8_t> arities;
    struct { ArgSpec::Kind kind; std::string unitRule; } returns;       // §3.2 rule 9 vocabulary
    KernelClass cls; bool blockCapable; bool telemetry; std::string provenance;  // "intrinsic" | plugin id | "lua:<file>"
};
class IMathFunction {
public:
    virtual ~IMathFunction() = default;
    virtual const FunctionManifest& manifest() const = 0;
    virtual Result<Value> evaluate(EvalContext&, std::span<const Value> args) = 0;   // whole math window, or per block when blockCapable
};
using Value = std::variant<double, SeriesValue /*BlockView + rate + t0 + unit*/, std::string, RangePath, Keyword, InvalidValue>;

class FunctionRegistry {
public:
    void addIntrinsic(const FunctionManifest&, IntrinsicId);
    void addExternal(std::shared_ptr<IMathFunction>);          // name collision with an intrinsic → refused (E-FN-COLLISION)
    const FunctionManifest* find(std::string_view nameCf) const;
    std::vector<const FunctionManifest*> all() const;          // editor auto-complete
};
```

Intrinsics are registered with the same manifest shape so the editor, the type checker and the documentation generator (`esses-cli maths functions --json`) treat built-in and external functions alike. An external function becomes an `ExternalKernel` stage: `blockCapable` → class `Elementwise`-like with `halo 0` on the parallel path; otherwise `Whole`, called once with arrays covering the math window (i2 `MathStartTime/MathEndTime` contract) and cached like any node.

### 10.2 C ABI `esses_mathfn_v1` (v1.0, frozen with D-028; in the single header `sdk/esses_abi.h`, 05 §10.1 rule 1)

Rules (D-028, 05 §10.1): C11, `struct_size` first in every struct, append-only fields, every field fixed-width (no C `enum` type in a field or parameter), UTF-8 `{ptr,len}` strings, descriptor strings plugin-static and out-parameter strings via `host->alloc`, host-owned sample buffers, no C++/Qt types. Everything below lives in `sdk/esses_abi.h` after the importer types and reuses `esses_str`, `esses_status` (mirrors `ErrorCode` 1:1 by value, D-051), `esses_dtype` (`ESSES_DT_*`, `uint32_t`), `esses_host_v1` (`alloc`/`free`/`log`) and `esses_call_v1` (cancellation, progress) exactly as 05 §10.2 declares them; none is redeclared here.

```c
/* sdk/esses_abi.h, after the importer types of 05 §10.2 */
typedef struct esses_rate { uint32_t num, den; } esses_rate;
enum { ESSES_V_SCALAR = 1, ESSES_V_SERIES = 2, ESSES_V_TEXT = 3, ESSES_V_INVALID = 4 };   /* values of esses_math_value_v1.kind */

typedef struct esses_math_array_v1 {          /* host-owned */
    uint32_t struct_size; uint32_t dtype;      /* esses_dtype */
    void* samples;
    uint8_t* valid_bits;                       /* 05 §10.1 rule 5: bit i%8 of byte i/8 (LSB first), 1 = valid; NULL = all valid */
    int64_t count; int64_t first_index;
    esses_rate rate; int64_t t0_us;
} esses_math_array_v1;

typedef struct esses_math_value_v1 {
    uint32_t struct_size; uint32_t kind;       /* ESSES_V_* */
    double scalar; esses_str text; esses_math_array_v1* series; esses_str unit;
} esses_math_value_v1;

typedef struct esses_math_ctx_v1 {
    uint32_t struct_size;
    const esses_host_v1* host;                 /* process-wide alloc/free/log (05 §10.2); no private copies here */
    void* eval;                                /* host-private evaluation handle passed back to the two callbacks below */
    int64_t math_start_us, math_end_us;        /* math window (§7.6) */
    esses_rate result_rate; int64_t result_first_index; int64_t result_count;
    esses_math_array_v1* (*create_result)(void* eval, uint32_t dtype);                 /* pre-sized to result_count */
    esses_status (*get_channel)(void* eval, esses_str name, esses_str unit, esses_math_array_v1** out);
} esses_math_ctx_v1;

#define ESSES_FN_BLOCK_CAPABLE 0x1u
#define ESSES_FN_TELEMETRY     0x2u
typedef struct esses_mathfn_v1 {               /* descriptor strings are plugin-static (05 §10.1 rule 4) */
    uint32_t struct_size;
    esses_str name;                            /* "mylib.rolling_rms" */
    esses_str manifest_json;                   /* schema mathfn-manifest-1: args, arities, returns.unitRule, description */
    uint32_t flags;
    void* (*create_state)(void);  void (*destroy_state)(void* state);   /* called once per worker thread */
    esses_status (*evaluate)(void* state, const esses_math_ctx_v1* ctx, const esses_call_v1* call,  /* call: cancellation, progress */
                             const esses_math_value_v1* argv, uint32_t argc,
                             esses_math_value_v1* out, esses_str* err);                          /* err via host->alloc */
} esses_mathfn_v1;
/* esses_plugin_desc_v1 (05 §10.2) carries: uint32_t mathfn_count; const struct esses_mathfn_v1* const* mathfns; */
```

Concurrency: the host calls `create_state` once per worker thread that evaluates the function and never calls `evaluate` concurrently on one state (05 §10.1 rule 8), so a block-capable function on the parallel path runs on several states at once, never on one.

`manifest_json` example: `{"description":"RMS over a sliding window","args":[{"name":"x","kind":"series","dim":"any"},{"name":"window_s","kind":"scalar","dim":"time"}],"arities":[2],"returns":{"kind":"series","unitRule":"same_as:x"},"telemetry":false}`. It is validated against `schemas/mathfn-manifest-1.schema.json` at load; the unit rule participates in type inference. `CApiMathFunction : IMathFunction` adapts the vtable and passes chunk validity bitmaps through unchanged (no byte-mask copy); a function that returns anything but `ESSES_OK` puts the item in `Error` with the `err` text in Channel Status. Status values mirror `ErrorCode` 1:1 by value (`ESSES_E_INVALID` for a bad argument, not `ESSES_E_ARG`; 05 §10.2, D-051); a value the host does not know maps to `Internal`. Plugins are loaded from `plugin.json` manifests (D-028), never from a registry; a plugin function whose name collides with an intrinsic is refused.

### 10.3 Lua (v1.1, D-030)

`esses_script` registers Lua functions through the same interface:

```lua
esses.math.define {
  name = "mylib.rolling_rms", description = "RMS over a sliding window",
  args = { { name = "x", kind = "series", dim = "any" }, { name = "window_s", kind = "scalar", dim = "time" } },
  returns = { kind = "series", unitRule = "same_as:x" },
  evaluate = function(ctx, x, window_s) local out = ctx:create_result(); ... return out end }
```

`ctx` exposes the same fields as `esses_math_ctx_v1`; arrays are userdata views with `samples[i]`, `valid[i]`, `rate`, `t0_us`, `count`, `sample_time(i)`, `sample_index(t_us)`. Lua functions run on the single script worker (`Whole` extent, never `blockCapable`), results are cached chunks, so Lua stays off the hot path. Sandbox per D-030.

---

## 11. Maths Editor UX contract (`esses_widgets::MathsEditor`, CTRL+M; D-027, parity-first §7.4)

1. Modeless `QDialog` (D-027) with three panes: **modules** (global modules in project order with enable check boxes, "Local Maths" node per open source, "Transient" node when present; Add / Import / Export / Delete / Move up / Move down), **items** (table: enable, kind icon, name, rate, result unit, status column from `MathStatus`, comment; multi-select cut/copy/paste/delete/enable; drag reorder), **item editor** (kind-specific form).
2. Expression editor: monospace, multi-line, comment highlighting; **live diagnostics line** under the editor, re-run on every keystroke after a 150 ms debounce by `Parser::parse` + `TypeChecker::check` against the Scope of the current Main source (or a data-less Scope when nothing is open); Errors in red with caret position, Warnings in amber, Info in grey; the full list opens in a popover.
3. An item with an Error cannot be applied; the dialog refuses to close while the edited item has an Error and offers Revert (i2 "cannot close an invalid expression"). Warnings never block.
4. Auto-complete popup after `'` with the Channels-window search semantics (multi-word prefix, case-insensitive, underscore = separator, loose-match option) over channels, constants and details of the Main source plus outputs of all modules; selecting inserts `'Name'[displayUnit]` (unit omitted for unitless). Function auto-complete on identifier characters from `FunctionRegistry::all()` with signature hints and the manifest description.
5. **`[unit]` popup**: when the caret is inside `[` `]` following a channel reference, list the units of that channel's quantity (display family, D-010) with the symbol spelled as the registry's text form; ENTER inserts; ESC closes.
6. **Insert Range** button: tree of the Main source's `RangeGroupTree` (`Outings`, `Outings:Laps`, `Outings:Laps:Track Sections:Default:Corners`, …); inserts the double-quoted path at the caret.
7. Hover help over operators shows the precedence table and the current `unaryMinusAbovePower` setting; hovering a function shows its manifest.
8. Kind-specific forms: Expression (name, rate explicit/Auto, quantity → gates result/display unit lists, result unit, display unit, DPS, colour, interpolate, enumeration, Logged/Telemetry check boxes, comment); Filter (channel, points or seconds); Scale & Offset (channel, scale, offset with unit); 2D Table, and 3D Table from v1.0 (D-027) (grid with right-click Cut/Copy/Paste, axis expression with the same editor, interpolation, out-of-range, breakpoints validated ascending); Builtin (schema-generated settings page from `settingsSchema()`, e.g. the Corrected Speed/Distance page is the same form as the Tools dialog); Constant (name, value, unit, comment); Enumeration (name/value grid, duplicate values rejected).
9. Persistence: Apply (or item switch) writes through a `QUndoCommand` (D-032) to the `MathModule` document; the module file is saved on dialog close (`maths/<module>.maths.json`, atomic write) and local maths to the sidecar; transient modules are never saved.
10. Entry points: CTRL+M (dialog), CTRL+SHIFT+M (open at the active graph channel's item if it is a maths channel; on a logged channel it does nothing beyond a status-bar hint, as in i2, and never creates an expression), double-click in Channel Status (CTRL+U) opens at the item, CTRL+SHIFT+S focuses the Channel Status dock on the active channel's row (06 §10; no popup), CTRL+SHIFT+F / O / Z create transient/local items per §4.2 without opening the dialog.
11. Status feedback: the items table shows `Pending/Calculating/OK/Warning/Error/Disabled` live via `StatusChanged`; a "Recalculate" action invalidates the selected items' cones.
12. Editing is allowed with no data loaded; unresolved references are `I-UNRESOLVED` until a source is open.

Qt Test rows (D-033): one per hotkey above, plus "cannot close with error", "[unit] popup lists quantity units", "Insert Range inserts quoted path", "warning does not block Apply", "transient filter disappears on reload", "zero at cursor writes sidecar with DPS preserved", "zero at cursor on a °C channel reads 0 °C at the cursor", "a dimensioned reference without `[unit]` shows `W-UNIT-MISSING` in amber".

---

## 12. i2 maths XML importer mapping (v1.0, D-024; `esses::project::CfgMathsImporter` in `esses_project`, `libs/project/src/cfgimport/`, parsing with pugixml as a private dependency; it builds `MathModule`s and parses every script through the same-layer edge `esses_project → esses_maths`, 01 §2.2 rule 1, §12 D-1, D-122; neutral class name per 00 §8 rule 1)

The JSON bag key is `x-i2` (D-019); `xI2` is only the C++ member that holds it (§4.1). Input: `<Maths …>` files from the user's own workspace (D-005), optionally wrapped in `<DataBlock>` (older Rally template). Scripts are parsed in the C locale.

| i2 element / attribute | `maths-1` field | Notes |
|---|---|---|
| `Maths/@Id` | `module.id` | file name on collision |
| `Maths/@Locale`, `@DefaultLocale` | — | numbers always C locale |
| `Maths/@Flags`, item `@Flags` | `x-i2.flags` | undocumented bit sets, preserved |
| `MathConstants/MathConstant{Name, Value, Unit, Active}` | `constants[]` `{name, value{v, unit}, enabled}` | unit via `UnitRegistry::find` alias table; unknown unit → `W-IMPORT-UNIT`, kept as text in `x-i2` |
| `MathEnumerations/MathEnumeration{Name}/Enum{Name, Value}` | `enumerations[]` | duplicate values → import report |
| `MathExpression{Id}` | `items[] kind=expression, name` | |
| `@Active="0"` (absent = enabled) | `enabled` | |
| `@Script` | `expr` | XML-unescaped, multi-line kept; parsed immediately; function-name case → `W-STYLE-CASE`; unknown functions (e.g. `map`, `lap` seen in user scripts) → item status `Error E-UNKNOWN-FUNCTION`, item preserved |
| `@SampleRate` | `rate` | `0` → `"auto"`, else `{hz,1}` |
| `@Unit` | `resultUnit` | declared result unit |
| `@DisplayUnit`, `@DisplayDPS`, `@DisplayColorIndex` | `display{unit, dps, colourIndex}` | |
| `@Interpolate` | `interp` | `1` → Linear, `0` → Closest (i2's up-sampling flag is Linear vs Closest) **[compat]**; raw value kept in `x-i2.interpolate`; one oracle row once a user's i2 export is available |
| `@EnumerationName` | `enumeration` | empty → none |
| `@Comment` (if present) | `comment` | |
| `MathFilter{Id, Samples, …}` | `kind=filter, name, window.points` | display attributes as above |
| `MathScaleOffset` (documented, not observed) | `kind=scale_offset` | attribute names resolved at import time from the user's files; unknown → `x-i2` |
| `MathTable2D{Id, Interpolation, Rate}` `XDim{Count, Id, Units, Data}` `Values{Units, Data}` | `kind=table2d; axis{expr: "'<Id>'[Units]", unit, breakpoints}; values{unit, data}; interpolation; rate` | `Interpolation="1"` → linear; other values → `next`/`previous` by observation, else flagged `W-IMPORT-TABLE-MODE`; whitespace float lists → arrays; `Count` validated |
| `MathTable3D` (expected `XDim`, `YDim`, `Values`) | `kind=table3d` | `Count = nx·ny` row-major assumed, flagged |
| `MathPlugin ProgId="MoTeC.CircuitMaths.*" Params SpeedClamp*` | `kind=builtin module=esses.corrected_speed_distance` | `Params` decoded best-effort: fastest-wheel pair, alternate list `[a;b;…]`, G-correction channel, distance source, stretch flag and tolerance (`1;5` → 5 %); `SpeedClampMin/Max`, `SpeedMin/Max`, `*Invalidate` → `clamp`; raw string kept in `x-i2.params`; every inferred field listed in the import report |
| `MathPlugin ProgId="MoTeC.GPSMaths.*" Params` | `kind=builtin module=esses.gps_corrections` | lat/lon/speed names, additional channel list, status channel, time-shift fraction (`0.8` → 80 %); raw kept |
| `MathPlugin ProgId="MoTeC.SuspensionMaths.*"` | `kind=builtin module=esses.damper_velocity` | channel pairs inferred; raw kept |
| other `MathPlugin ProgId` | `kind=plugin` with `x-i2.progId/params` | status `Error E-PLUGIN-UNAVAILABLE` |
| `MathRegression` (1.1.4; undocumented) | `kind=regression` | preserved verbatim |
| `.ldx` local maths / constants | sidecar `localMaths` | same mapping; written to the sidecar on the user's confirmation (D-015: `.ldx` is never written) |

The importer writes an import report (per item: imported / imported-with-guesses / preserved-unsupported, plus every reference flagged `W-UNIT-MISSING`, §2.4) to the Messages dock and never drops an item. `MathsCompat::functionAliases` (`docs/maths-compat.md`) maps alternative spellings discovered later (running min/max, `bit_decode`) onto our canonical names.

---

## 13. CLI

`esses-cli eval "<expr>" <log…> [--layer Normal] [--rate auto|<Hz>] [--unit <u>] [--range "Outings:Laps/3"] [--main <range> --ref <range>] [--compat unaryMinusAbovePower=1] [--precision double] [--json|--csv] [-o file]` — parses, type-checks (diagnostics on stderr, JSON with `--json`), evaluates on the `TaskPool` with synchronous wait, prints `{t_us, value, valid}` rows. `esses-cli maths check <module.maths.json> <log>` prints per-item status; `esses-cli maths functions --json` dumps the registry manifests (documentation source). `esses-cli validate` covers `maths-1` and `mathfn-manifest-1`.

---

## 14. Test strategy and acceptance (D-033)

### 14.1 Case files (`tests/maths/*.case.json`, schema `maths-case-1`)

```json
{ "$schema": "urn:esses:maths-case-1", "schemaVersion": 1,
  "id": "smooth_even_window_bias", "expression": "smooth('X'[], 4)",
  "provenance": { "kind": "help", "ref": "Maths Functions (Pro): smooth", "note": "even window [i-2, i+1], truncated edges, invalid samples excluded" },
  "rate": "auto", "compat": {}, "precision": "single",
  "inputs": { "X": { "rate": { "num": 10, "den": 1 }, "unit": "", "values": [0, 1, 2, 3, 4, 5, 6, 7], "valid": [1,1,1,1,0,1,1,1] } },
  "ranges": { "Outings:Laps": [ { "startUs": 0, "endUs": 400000 }, { "startUs": 400000, "endUs": 800000 } ] },
  "selection": { "main": "Outings:Laps/1", "overlay0": "Outings:Laps/2" },
  "expected": { "rate": { "num": 10, "den": 1 }, "unit": "", "values": [0.5, 1.0, 1.5, 2.0, 3.3333333, 4.6666667, 6.0, 6.0], "valid": [1,1,1,1,1,1,1,1],
                "tolerance": { "abs": 1e-6, "rel": 0 }, "diagnostics": [ { "code": "W-DIM-MISMATCH", "count": 0 } ] } }
```

Fields: `$schema` and `schemaVersion` (D-072), `id` (unique, also the file stem), `provenance {kind: help | release-notes | derived | oracle | public-maths, ref, note}` (the `CLEANROOM.md` source of the expected semantics; `oracle` cases live only in `fixtures/private`, D-005), inputs, ranges, selection, and `expected` with `tolerance {abs, rel}` (a value passes when `|got − want| ≤ abs + rel·|want|`). The example's window is `[i−2, i+1]`, truncated at the extent edges, with the invalid input sample 4 excluded: index 4 averages samples 2, 3, 5 (3.333…), indices 6 and 7 average 5, 6, 7 (6.0). This reading is the `docs/maths-compat.md` row `smooth.edges` (truncated window, edge outputs valid). The corpus also holds `tests/maths/range_is_track_sections_default_corners.case.json`, which evaluates `range_is("Outings:Laps:Track Sections:Default:Corners")` with the help's exact path against the per-category child group of 02 §4.5.

≥ 150 cases at M2 (acceptance, outline §7): every function and arity, both window parities and the seconds→odd rule, `smooth(x, 0)`, `smooth(x, 1)` and `smooth(x, t)` with `t·rate < 2` (all ≡ width 2), `Linear` resampling of a `Closest`/`Previous` input to `smooth`, edge truncation, invalid propagation, every filter order incl. zero-phase and the omitted Chebyshev order (≡ 2), `range_is/range_change`, `stat_*` gated/reset/ungated, variance with offsets and stretch and with several overlays selected (evaluates against the reference), tables (2D × linear/next/previous × clamp/invalid; 3D cases join at v1.0), scale/offset with temperature deltas, zero at cursor on a °C and on a `bar g` channel, enumerations, constants with unit conversion, own-module-first constant lookup and `W-CONST-AMBIGUOUS`, alias-before-name resolution and alias fallback, text-detail comparison under all six relational operators, `W-UNIT-MISSING`, `Corr Dist` from a resetting logged `Lap Distance`, precedence (`-2^2` with both compat settings, `2^3^2`, `a & b == c`), word aliases, comments, scientific notation, shadowing chains (local→global, duplicate ignored), cycle detection, module condition, each i2-documented syntax error. Expected arrays of windowed, stateful, IIR and resampling cases are generated by a brute-force reference evaluator (`tests/maths/reference/`: per-sample F64 loops written from §5's text, independent of the kernels) and reviewed, never computed by hand. At v1.0 the corpus adds the seam case `smooth(smooth(x, 5), 5)`, chunked vs whole across samples 65,535/65,536.

### 14.2 Property tests (Catch2, `esses_maths`)

1. `print(parse(x))` round-trips and `parse(print(parse(x)))` is structurally equal for the public parser corpus (our default circuit module, hand-written i2-syntax cases and fuzz-generated expressions, D-040) and random expressions; the private corpus (ACC workspace and vendor template maths) runs the same check where `fixtures/private` is checked out.
2. Precedence oracle: random expression trees rendered with minimal parentheses re-parse to the same tree under both compat settings.
3. Determinism (§6.7): from M1, 4,096 vs 1,024 block sizes and repeat runs give bit-identical output. At v1.0 (chunk path), for random plans and NaN-sprinkled inputs, per-chunk evaluation in random order and the rebuild of evicted `Ordered` chunks from checkpoints equal whole-extent evaluation bit-for-bit, including chained windows across a chunk seam.
4. Window identities: `smooth` of a constant is the constant; `derivative` of `a·t + b` is `a` (interior samples); `integrate` of a constant is linear; `smooth(x, n)` equals the brute-force per-sample mean.
5. IIR: `filter_lp(x, fc, −2)` of a reversed signal equals the reversed output (zero phase); forward order 1 equals a scalar reference RC recursion.
6. Resampler: monotone index maps, exact `Rate` arithmetic (no drift at 33.333 Hz over 24 h), `Previous`/`Next` boundary semantics; a block equals per-sample `Resampler::valueAt(timeAt(i))` for `{512,25}→{1000,1}` and `{30,1}→{100,1}` under all four `Interp`s (§6.3).
7. Graph: `version` changes iff the definition, enable state, condition, an upstream version, or a logged column, range group, detail or constant the node reads changed; an annotation (any sidecar commit that touches none of those) changes no version; a track-section rebuild leaves `Corr Dist`'s version unchanged; `SelectionDependent` propagates downstream; re-keying restores cached chunks; cycles report the path.
8. Shadowing: random chains of local/global self-maths evaluate to the composed function in i2 order.
9. Determinism of `rand_chan` across chunk orders.
10. Validity: output bitmap equals NaN positions for float results.
11. Scheduler keys (§7.4): two different `chunkNo` of one node produce two tasks; ten requests for one chunk produce one; a chunk request while the node's `/all` task is queued or running produces none.

### 14.3 Oracles, fixtures, fuzz, benchmarks

- Oracle cases (D-005 (1), consent given 2026-10-10; logs from MoTeC devices only, D-038): i2 CSV exports of maths channels for the user's MoTeC-device logs become `expected.channels/<name>.csv.gz` in the private fixture submodule; compared at the exported DPS ± 1 ulp of the display unit; each ambiguous semantic listed in `docs/maths-compat.md` gains one oracle row.
- Parser fixtures (D-040): vendor-shipped template maths files and the ACC workspace maths are private fixtures in `fixtures/private` (never redistributed); they parse 100 % with zero Errors, and their public golden JSON (`esses-cli maths check --json`) records counts only (items, OK/warning/error), never names or expressions.
- Fuzz: libFuzzer on `Parser::parse` and `TypeChecker::check` (60 s per PR, 30 min nightly); no crash, no exception escaping, bounded memory.
- Benchmarks (Google Benchmark, JSON baselines, outline §8): elementwise 2.16 M samples one core ≤ 10 ms (MVP) / ≤ 3 ms (v1.0); `filter_lp` order −2 on 21.6 M samples ≤ 150 ms (v1.0); parse+check ≤ 1 ms per expression; RW-2 + 20 maths channels resident ≤ 1 GB; the 20-maths-channel readiness row as defined in `bench/workloads.md` (its 20 expressions and the reference machine's CPU, core count, RAM and OS), measured as the median of ≥ 5 runs on the reference machine (D-042).
- TSan on `esses_maths` tests in the `sanitize` job (D-031).

### 14.4 Milestone acceptance (from outline §7)

- **M1** (D-044): `MathGraph` define/require/version, `PlanProvider`, `EvalContext` and the built-in `esses.corrected_speed_distance` module (`fastestWheel` with clamps, `Corr Dist` integration) only; it populates distance mode on the `synthetic-acc-dialect` fixture (and on a real ACC `.ld` where the private submodule is present). `Parser`, `TypeChecker`, `EwKernel`, `esses-cli eval` and the elementwise budget move to M2.
- **M2**: `Parser`, `TypeChecker`, `esses-cli eval` (a 3.6 M-sample elementwise expression < 100 ms), full language and catalogue; ≥ 150 cases green; the user's real i2 modules (pasted or imported in v1.0) every item OK or with an explained status; shadowing/cycle tests; Channel Status precise for every item; Maths Editor behaviours under §11 covered by Qt Test rows.

---

## 15. CLEANROOM provenance lines (to be appended to `CLEANROOM.md`)

- Expression lexical rules, operators and word aliases: i2 help "Maths Expressions (Pro)", "Maths Operators (Pro)"; observed in the user's own maths XML (*file written by the user's install*, D-005 (2); D-005 (2) consent 2026-10-10, research use 2026-10-09, D-039).
- Function names, arities and window semantics: help "Maths Functions (Pro)"; release notes 1.0.5–1.1.4 for `atan2`, `integrate_dist`, `variance_*`, `pulse_width`, `previous_sample`, running min/max, `bit_decode`, `power` with channel exponent.
- Order of calculation and shadowing: help "Maths Order of Calculation".
- Filter, Scale & Offset, Tables, Constants, Channel Status, transient vs local maths: help pages of the same names; "Basics – Channel Status".
- Corrected speed methods and clamping, distance stretching, Corrected GPS options: help "Corrected Speed and Distance" (Circuit/Drag/Rally), "GPS Calculations"; release notes 1.1.4.
- External function/plugin contract shape: shipped SDK sample source files (VB.NET, C++ ATL) read for interface shape only; no code reused.
- Maths XML attribute semantics: *vendor-shipped template* profile XML files, and the user's own workspace files (*file written by the user's install*, D-005 (2); D-005 (2) consent 2026-10-10, research use 2026-10-09, D-039); no file content redistributed.
- Precedence of `^` versus unary minus, `stat_*` segment semantics, `edge_delay`/`pulse_width`/`time_shift` conventions, table out-of-range, IIR priming: **chosen readings**, not observed; recorded in `docs/maths-compat.md`.

---

## 16. Open points raised by this document (proposed decisions, smallest consistent choice taken)

1. `MathState` adds `Evaluating`, `Warning` and `Disabled` to the outline's `Channel::Status {Ok, Pending, Error}`; the mapping in §7.3 keeps the outline type unchanged.
2. `Type` gains `scaleToBase` and `Dimension::unknown()` (after fractional powers) so unit-scale mismatches can be reported as Info without rejecting anything (D-021 unchanged).
3. `ExtentMode::Ordered` (chunk-by-chunk with state carry and per-boundary checkpoints, §6.2) is introduced between D-022's chunk-independent and whole-extent paths for forward IIR passes and, on the v1.0 chunk path, the single-pass Stateful kernels (still sequential on one worker as D-022 requires); D-022's "forward orders chunk-independent with state carry" is read as this mode. It exists only from v1.0 (§6.0).
4. Math window for a source that does not hold Main: earliest selected overlay in that source, else the layer extent (§7.6).
5. `stat_*` are segment-constant (two-pass) and `running_min/max` are cumulative; names of the running functions are guesses behind `MathsCompat::functionAliases`.
6. `esses.gps_corrections` is registered for every source with GPS lat/lon from M2 with only its zero-fix validity step active (always on, i2 core behaviour); its correction steps stay disabled until settings exist, which the v1.0 dialog or importer provides (§9.2; XREF 07 §10.1 item 6, 01 §10).
7. `time_shift`/`previous_sample` stay `Stateful` per D-022; reclassification to `Windowed` with a halo is an optional, bit-identical optimisation.
8. New schema ids `maths-case-1` and `mathfn-manifest-1`, the builtin settings schemas `builtin-<id>-<version>`, and the `project.json: builtinMaths` key (per-source overrides as `builtin` items in sidecar `localMaths`).
9. Validity at the C ABI is the 05 §10.1 rule 5 bitmap (LSB first, `NULL` = all valid), shared with the importer ABI; the earlier one-byte-per-sample proposal (D-086) is withdrawn (D-131).
10. `rand_val`/`rand_chan` are deterministic (seeded by definition hash and sample index) so cached results are reproducible.
11. Staging amendment to D-022 (DECISIONS.md Addendum A): 0.1 evaluates every node over the whole extent with one task per node and parallelism across nodes only; the chunk-parallel evaluator (register bytecode, cumulative halos, `Ordered` checkpoints, per-chunk tasks, chunk-order determinism test) is v1.0 (§6.0).
12. Refinement of D-022's node version and D-016's `DistanceIndex` key (DECISIONS.md Addendum A): node versions hash exactly the logged columns, range-group versions, details and constants a node reads, never `DataSource::version`; range versions are per group path; `DistanceIndex` is keyed by `Corr Dist` column version, lap anchor markers and stretch settings (§7.2).
13. `MathSelection` (§7.5) is the maths-local selection copy; `esses_maths` never names `plotcore::SelectionSnapshot`.
14. `MathModuleIo` (maths-1 ⇄ `MathModule`) lives in `esses_maths`; the v1.0 maths XML importer is `esses_project::CfgMathsImporter` in `libs/project/src/cfgimport/` (pugixml private), reached through the same-layer edge `esses_project → esses_maths` (01 §12 D-1, D-122). Moving it into `esses_maths` instead was the alternative listed in `OPEN_QUESTIONS.md` (F-1) and is not adopted.
15. The maths ABI lives in the single header `sdk/esses_abi.h` and reuses 05 §10.2's `esses_str`, `esses_status` (1:1 with `ErrorCode` by value, D-051), `esses_dtype`, `esses_host_v1` and `esses_call_v1` (§10.2, D-131). The alternative of a separate `esses_abi_common.h` with a shorter status list and an explicit mapping table was listed in `OPEN_QUESTIONS.md` (F-13) and is not adopted.
16. Maths task keys carry `/c<chunkNo>` or `/all` (§7.4) so share-key coalescing never returns another chunk's `Future`.
17. Constant lookup is own module → local → other enabled global modules in project order with `W-CONST-AMBIGUOUS`; aliases are checked before channel names as D-013 states (§3.3).
