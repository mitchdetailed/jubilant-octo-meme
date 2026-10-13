# esses_maths

Layer 2, Qt-free (01 §2.1, 03 §1). M1 is the minimal subset of 07 §4.1 and D-044: a basic `MathGraph`
(define/require/version), `PlanProvider` into the `ChunkCache` on the 0.1 whole-node path (03 §6.0, D-139),
`EvalContext`, and the built-in module `esses.corrected_speed_distance` with its settings schema. M2: the
language (`Lexer`, `Parser`, `print`, `TypeChecker`, `compile` with the fused `EwKernel`, `evaluateWhole`,
`MathInputGatherer::fill`) and `MathModuleIo` are implemented (see "The language" below; `esses-cli eval` and
`maths check` in `apps/cli`), and so are the kernels of every intrinsic with the function registry and the case
corpus (see "The kernels" below), and so is the graph: shadowing chains, `define`/`setModules`, conditions, cycles,
versions, `SelectionDependent` keys, `MathsScheduler` and `esses.gps_corrections` (see "The graph" below); what is
left of the contract's `M2-STUB`s is listed under "M2 contract".

Namespace `esses::maths`; public headers in `include/esses/maths/` (umbrella `maths.hpp`; snake_case file names
per the repository convention where 03 §1 lists `Parser.h`-style names). Links `esses_base`, `esses_units`,
`esses_store`, `esses_model`, `esses_analysis` (same-layer edge); never `esses_formats` or `esses_project`.
nlohmann-json and json-schema-validator are private. Sources and tests are globbed.

| Header | Contents | Design |
|---|---|---|
| `channel_key.hpp` | `ChannelKey{src, layer, nameCf}` (+ `std::hash`), `Version` (= `Hash128`), `SampleRange`, `ColumnHandle` (with the evaluation's `diags`) | 03 §4.3 rule 9, §7.1-7.2 |
| `diagnostics.hpp` | `Span`, `Diagnostic`, `Diagnostics`, `hasError` | 03 §3.1, §2.5 |
| `math_status.hpp` | `MathState`, `MathStatus`, `mathStateName`, `toChannelStatus` | 03 §7.3, D-060 |
| `maths_compat.hpp` | `MathsCompat` | 03 §2.3, D-020 |
| `math_module.hpp` | `ItemKind`, `MathItem` (builtin fields), `MathModule` (M1 subset of maths-1) | 03 §4.1 |
| `eval_context.hpp` | `MathRangeGeom`, `MathSelection`, `EvalContext` | 03 §7.5-7.6, D-050 |
| `builtin_module.hpp` | `DisplayDefaults`, `WholeExtentFn`, `BuiltinOutput` (`upstream`, `inputs`), `IBuiltinModule`, `validateSettings` | 03 §9, D-016, D-024 |
| `whole_signal.hpp` | `readWholeSignal` (a uniform series into an `analysis::UniformSignal`, worker only) | 03 §6.0 |
| `corrected_speed_distance.hpp` | `CorrectedSpeedDistanceModule` (`kId`, `kVersion`, `kSettingsSchemaId`, `kSettingsSchemaFile`, `kSpeedOutput`, `kDistanceOutput`, `loadSchema`, `validateSettings`, `findSchemasDir`) | 03 §9.1 |
| `plan_provider.hpp` | `PlanSnapshot`, `PlanProvider : store::IChunkProvider` | 03 §6.0, §6.2, §7.2 |
| `math_graph.hpp` | `NodeId`, `MathRequest`, `MathInput`, `MathNodeInfo`, `MathGraph` (pimpl; `defineBuiltin`, `define`, `remove`, `setEnabled`, `setModules`, `setResolverTables`, `setCompat`, `setSelection`, `setDistanceIndexes`, `trace`, `setModuleEnabled`, `setSource`, `invalidate`, `version`, `require`, `prefetch`, `applyCompletion`, `applyReadyCompletions`, `status` (by key and by node), `nodes`, `consumersOf`, `outputs`, `descriptor`, `find`, `taskKey`, `taskKeyPrefix`, `evaluationCount`) | 03 §4.3, §7.1-7.3, §8; D-171 |

## How it works (0.1 whole-node path)

- `defineBuiltin(module, settings)` expands the module (null settings: its defaults) into one node per output.
  `defHash` = xxh3(engine version, module id and version, canonical settings JSON, output definition, compat
  switches); `version` = xxh3(defHash, enabled, module enabled, sorted upstream versions, layer extent, and for
  each `BuiltinOutput::inputs` name present in the layer its column id, store version, unit, dtype, interp and
  time base). `DataSource::version` is never hashed (D-140): a sidecar commit that shares the columns keeps
  every version. A node whose version changes drops its cached column, failure and evaluation and becomes
  Pending (Disabled while its module is disabled).
- `require(key)` never blocks. A cached column of the current version (or a memoised non-cancelled failure)
  comes back as a ready `Future`; otherwise the upstream nodes are required first and one task is submitted at
  the requester's priority under the share key `maths/<src hex>/<layer>/<name>/<version hex>/all`, so a repeat
  joins the queued or running task (one more requester, priority raised). The task waits for its upstream
  Futures (Rule W: a still-queued one runs inline), reads their sealed columns back with `readWholeSignal`
  (Corr Dist therefore integrates Corr Speed as stored, F32), calls `BuiltinOutput::evaluate` over the whole
  extent, and seals the result into the `ChunkCache` through a `Column` over a `PlanProvider` seeded with the
  extent (chunk key prefix `chunk/<src hex>/<layer>/maths/<name>/<version hex>`: a new version's column never
  joins or raises a chunk task of the superseded one). The status becomes Evaluating.
- Versions (03 §7.2) hash each input as the evaluation resolves it (channel name, then logged name) with its
  channel id, status, unit, dtype, interpolation and column identity/version, so an input matched by its logged
  name only, or one whose status turns to Error, re-versions its readers. When a node's version changes while
  its evaluation is in flight, that evaluation and its chunk tasks are cancelled (`cancelAll` on the old keys).
- The host posts each completion to the owning thread and calls `applyCompletion(key, version, result)`, or
  calls `applyReadyCompletions()` (the CLI after waiting): Ok/Warning (diagnostics from the handle; warnings
  give the summary "first message (+n more)"), Pending after Cancelled, Error otherwise with the error's
  `context.detail` as the code when it is one (`E-CSD-NO-SPEED`), else `E-EVAL`. A completion for an older
  version is ignored. Upstream failures propagate with "input '<name>': " and their code.
- `prefetch(Priority::Background)` is the D-016 / AC-M1-10 hook: it requires every enabled output and returns
  `MathRequest{key, version, future}` for the host to post. `TaskPool::cancelAll(MathGraph::taskKeyPrefix(src))`
  stops a closing source's evaluations.
- `PlanProvider` slices chunks from the evaluated extent (NaN and a cleared valid bit at invalid samples, no
  bitmap for a chunk without one), releases the extent once every chunk has been served, and evaluates the
  whole extent again through `PlanSnapshot::evaluate` if a chunk is asked for later (buildSummary, M5 eviction).
  A cancelled evaluation is retried; any other failure is memoised.

Settings schema: `schemas/builtin-esses.corrected_speed_distance-1.schema.json` (`$id`
`urn:esses:builtin-esses.corrected_speed_distance-1`), every member optional (absent = module default), with
valid/invalid examples under `schemas/examples/builtin-esses.corrected_speed_distance-1/`. `apps/cli` copies
`schemas/*.schema.json` next to the executables (`<exe dir>/data/schemas`); `findSchemasDir(dataDir)` tries
`<dataDir>/schemas`, then `<exe dir>/data/schemas`, then the source tree (`ESSES_MATHS_SOURCE_DIR`). The host
calls `loadSchema(findSchemasDir(paths.dataDir()))` once; `expand()` then validates settings first
(`E-CSD-SETTINGS`, `context.detail` = JSON pointer) and only then parses them with
`analysis::CorrectedSpeedSettings::fromJson`.

## Tests (`maths.*`, Catch2)

- `plan_provider_tests.cpp`: chunks sealed into a `ChunkCache` with validity and NaN, F32/F64/I32, extent
  release and bit-identical recomputation, cancelled vs memoised failures; `readWholeSignal`.
- `math_graph_tests.cpp` (with `FakeModule`, no analysis needed): definition and descriptors, require through
  `PlanProvider`, version-keyed caching (no re-evaluation, shared columns), invalidation by settings, by a new or
  bumped input column, by an input appearing/disappearing and by module enable (never by a sidecar commit),
  stale/cancelled completions, warnings, error propagation and memoisation, the Background prefetch hook (tasks
  queued in the Background band, raised by a Visible require through the share key), `cancelAll` on the source
  prefix, rejected output sets.
- `corrected_speed_distance_tests.cpp`: AC-M1-10 core part (defaults equal the shipped example and validate;
  every schema example through `validateSettings`; invalid settings rejected before any node exists), schema
  loading, `expand` wiring and a four-wheel fixture through `MathGraph` + `PlanProvider` (Corr Speed =
  fastestWheel, Corr Dist = trapezoid integral of stored Corr Speed within 1e-9). The last two skip with the
  stub's message while `analysis::CorrectedSpeed` is an M1 stub.

AC-M1-12 runs this suite under TSan (CI `sanitize`, Linux/Clang); the GUI-side prefetch test is
`tests/ui/corr_dist_prefetch` (esses_qtmodel).

## Deviations from the design text

- Default settings (M1 integration; D-154, proposed in `DECISIONS.md` A.12, with 03 §9.1 and 07 §4.1 updated):
  `alternateList.channels` is 03 §9.1's list with `"Speed"` appended (11 entries). The sim-exporter dialect (`synthetic-acc-dialect`, ACC-style exports)
  logs its road speed as `SPEED`, which no other entry names; without it the 07 §4.1 M1 data path (ACC `.ld`
  -> Corr Speed/Corr Dist -> distance mode, AC-M1-01) shows "no distance data". Lookups are case-insensitive and
  a channel whose unit is not a speed is skipped, so the entry is inert for other files; the importer
  `Dictionary` (M2) or project Aliases can supersede it.
- JSON values are `model::OpaqueJson` where 03 writes `JsonDoc` (`esses_base`'s `JsonDoc` is still reserved).
- `IBuiltinModule::expand(settings, UnitRegistry, Diagnostics)` instead of `(settings, Scope, Diagnostics)`; the
  maths `Scope` arrives with the parser (M2).
- `BuiltinOutput` carries `upstream` names, `inputs` (the logged channels it may read, which the node version
  hashes in place of a compiled `Plan`'s inputs) and a whole-extent `evaluate(ctx, upstream, diags)` instead of
  `variant<expression, unique_ptr<Kernel>>` (Kernel classes M2; 03 §6.0 whole-node path).
- `EvalContext::getChannel` (a `MathInputGatherer` factory) is omitted until `MathInputGatherer` exists (M2).
- `MathGraph` takes a `TaskPool*` and a `ChunkCache*` (null: the installed ones at require time) and adds
  `applyCompletion`/`applyReadyCompletions` (the owning-thread status write of 03 §7.3 / 01 §5.3 rule 10),
  `prefetch`, `descriptor`, `find` and `taskKey`; `setSource` and `setModuleEnabled` return the re-versioned keys.
  The M2 members are described under "The graph".
- `SampleRange` (half-open, `all()`) and `ColumnHandle{key, version, descriptor, series, diags}` are defined
  here; 03 names them without declaring them. `require`'s range is accepted but every 0.1 evaluation covers the
  whole extent; `MathStatus::progress` is 0 while Evaluating and 1 when done (no per-chunk progress in 0.1).
- The output geometry (rate, t0, size) is the evaluated signal's, not computed beforehand from 03 §6.4.
- A failure other than Cancelled is memoised per node version (03 §7.3 Error until something changes).
- Output names may not repeat within one built-in module (`E-DUPLICATE-OUTPUT`); since M2, outputs of different
  modules with one name form a shadowing chain in registration order (03 §4.3).
- `tools/check_schemas.py` accepts `builtin-<module id>-<version>.schema.json` (the id may contain `.` and `_`)
  as a settings fragment without the document `$schema`/`schemaVersion` envelope (03 §9, 02 §6.2
  `builtinMaths`).

## The language (`src/lang`, M2)

| File | What | Design |
|---|---|---|
| `lexer.cpp` | `Lexer`: UTF-8 in the C locale; every lexical error is a diagnostic and the lexer carries on | 03 §2.1 |
| `parser.cpp` | `Parser::parse` (Pratt, the binding powers of 03 §2.3, `unaryMinusAbovePower`, `I-COMPAT-POW`, error recovery, depth limit 256), `print` (canonical form, parentheses that read the same under both compat settings), `Ast::sourceHash` (xxh3 of the canonical text with case-folded names) | 03 §2.2-§2.5, §3.1 |
| `type_checker.cpp`, `signatures.*` | `TypeChecker::check`: one iterative post-order walk (no recursion), kinds, units, dimension vectors, scales, constant folding, the rules of 03 §3.2; the intrinsic signature table (arities, parameter roles, result rules) | 03 §3.2, §5, D-021, D-085 |
| `compile.cpp`, `ew_kernel.*`, `ew_ops.hpp` | `compile`: maximal elementwise subtrees fuse into one `EwKernel` (constants folded into immediates, linear-scan registers over 4,096-sample blocks, one tight loop per instruction with the operator as a template parameter); every other intrinsic is a stage from `makeIntrinsicKernel`; the result unit to base conversion; inputs, dtype, `selectionDependent`, `rangeGroups`, `defHash` | 03 §4.4, §6.0-§6.5, §7.2 |
| `evaluate.cpp` | `evaluateWhole` (the 0.1 whole-node evaluation: a `PlanStream` pulled to the end) and `MathInputGatherer::fill` (pins and gathers through `Resampler`; an identity gather copies the chunks' samples directly) | 03 §6.0, §6.3 |
| `plan_stream.*` | `PlanStream` (D-191): a plan evaluated in pieces of 16,384 result samples through sliding windows (`start`, `pull`, `restartable`, `restartAt`); `finaliseResult` (the output dtype's rounding and the non-finite rule) | 03 §6.0, §6.5, §6.6 |
| `math_module_io.cpp` | `MathModuleIo`: maths-1 <-> `MathModule` (the written form below) | 03 §4.1, 02 §6.5 |
| `scope.cpp` | the data-less `Scope` and `Scope::dataBacked` | 03 §3.3, §11 item 12 |

**Diagnostics** (`src/lang/codes.hpp`). The codes of 03 §2.5 as written there: `E-QUOTE-MISSING`, `E-UNIT-BRACKET`,
`E-PAREN-UNBALANCED`, `E-CALL-UNCLOSED`, `E-ARITY`, `E-ARG-TYPE`, `E-COMMA-MISSING`, `E-UNKNOWN-FUNCTION` (with the
nearest name), `E-UNKNOWN-CHANNEL` / `I-UNRESOLVED`, `E-UNIT-UNKNOWN` (with the nearest symbol); of 03 §3.2:
`E-INTEGER-REQUIRED`, `W-DIM-MISMATCH`, `W-UNIT-SCALE` (Info), `W-UNIT-MISSING`, `W-UNIT-FAMILY`, `W-RESULT-DIM`,
`I-RESULT-SCALE`, `W-WINDOW-ARG`, `W-STYLE-CASE` (Style), `I-COMPAT-POW`; of 03 §4.4, §5.3 and §6.4: `E-RATE-ZERO`,
`E-KIND-UNSUPPORTED`, `E-PLUGIN-UNAVAILABLE`, `E-ARG-VALUE`. Ours: `E-SYNTAX` (any other unexpected token),
`E-EXPR-EMPTY`, `E-UNEXPECTED-CHAR`, `E-COMMENT-UNCLOSED`, `E-NUMBER-FORMAT`, `E-EXPR-TOO-DEEP`, `E-NAME-EMPTY`,
`E-KERNEL-UNAVAILABLE` (a call without a kernel: the kernels of `src/kernels` and external functions), and from
the kernels `E-REF-SOURCE` (variance against a reference lap in another source, D-185 (l)). Every diagnostic
carries its byte span; `Parser::parse` sorts them by position. The readings behind them are the `lang.*` rows of
`docs/maths-compat.md`.

**Corrections of the M2 review** (D-185, D-186, D-188; each with a regression test in
`tests/graph/graph_review_tests.cpp`, `tests/lang/` or the case corpus): the result rate counts a channel read only
by a statistic; integer results are I32 only when they provably fit (else F64) and integer or Bool results with a
declared non-base unit are real; non-finite results are invalid; a literal factor in `* /` makes the scale
unknown (no `I-RESULT-SCALE` through hand conversions); a table axis is checked against its unit; the power switch
is hashed through the parse only; synthesised filter and scale_offset references carry their unit; Filter items
follow `smooth()`'s window rules; LapScalar and Scalar results default to Previous; `previous_sample` of an item's
own name with nothing before it is `E-CYCLE`; the selection key hashes the resolved math window; a timestamped
channel's nominal rate is whole Hz; scale_offset is computed in the unit of its offset; and `MathGraph` built
without a source (`esses-cli maths check` without `--log`) checks items without compiling those that read an
unresolved name.

**How the pieces meet.**
- `compile()` type-checks a private copy of the parsed AST (pass the parser's AST; do not check it into the same
  `Diagnostics` first) and fails on the first error (`ErrorCode::Invalid`, `context.detail` = the code,
  `context.span`); warnings never block (D-021). A declared result unit is checked only when the item sets its unit
  or quantity. Unresolved references (data-less scope) cannot be compiled (`E-UNKNOWN-CHANNEL`).
- **Stage inputs of an intrinsic call** (the convention `makeIntrinsicKernel` reads): the call's series arguments
  (roles Value, Channel, Cond, Reset of `signatures.hpp`, and `derivative`'s second argument when it varies per
  sample) are the stage's inputs in argument order (a constant series argument, such as the 1 of
  `integrate(1, cond)`, becomes a constant stage); every parameter is a literal node of the AST passed to
  `makeIntrinsicKernel`: `IntLit` for points, filter orders and counts, `DecLit` for seconds, hertz and ripple (in
  s, Hz and %, constants and unit conversions folded), `StrLit` for range paths, `Keyword` for rise/fall. The first
  argument of `smooth` is gathered `Linear` (03 §6.4 rule 6).
- **`evaluateWhole`** is a `PlanStream` (D-191) pulled from the first result sample to the last. The stream gathers
  each `PlanInput` through `EvalContext::getChannel` (the host's series, unit converter and grid; the plan's
  interpolation) on demand, at most one input chunk per gather, calls `reset(ctx)` on every stage once, and produces
  the result in pieces of 16,384 samples: each `kern::Streamable` stage steps through the extent carrying its state
  (running sums, filter state, latches) and reads its inputs from sliding windows that keep one piece plus the
  context it needs on each side (`behind()`, `ahead()`); a stage that needs its whole input first (a backward filter
  pass, the statistics, the Variance kernels) runs once over the whole extent (03 §6.0 item 3) when its output is
  first needed. Buffers are F64 with NaN at invalid samples and no bitmap. Every output sample has the value of one
  whole-extent run, bit for bit (each kernel does the same operations in the same order whatever the pieces). The
  result is rounded to the plan's dtype (an F32 result as F32 stores it) by `lang::finaliseResult`.
- **`defHash`** = xxh3 of the engine version, `Ast::sourceHash`, item kind, rate, result unit symbol, interpolation,
  precision, enumeration and every `MathsCompat` switch (`functionAliases` included). Unit ids are hashed as symbols.
- **maths-1 written form** (`MathModuleIo::toJson`): `$schema`, `schemaVersion`, the module members, each item's
  kind-core members always (expression: `expr`, `rate`, `quantity`, `resultUnit`, `display`, `interp`; filter:
  `window`, `display`; scale_offset: `scale`, `offset`; tables: axes, `values`, `interpolation`, `outOfRange`;
  builtin/plugin: `module`, `version`) and every other member that differs from its default, so
  `save(load(save(load(f)))) == save(load(f))` and nothing a file says is lost. Units are written as registry
  symbols, quantities by registry name (an alias such as "Angular Velocity" comes back as "Angular Speed").

**Performance** (AC-M2-14, 07 §12: elementwise 2.16 M samples <= 10 ms on one core). Measured on the maintainer's
machine (not the reference machine, D-042) with a Release build of the hidden test
`esses_maths_tests "[elementwise-bench]"`, median of 7, while other builds were running: the fused stage of
`'Ground Speed'[m/s] * 2` 4.0-4.8 ms, of `'Ground Speed'[km/h] * 2 + 1` 4.7-5.2 ms, of a six-instruction `choose`
expression 11-14 ms; `evaluateWhole` including the gather from the chunk store and the 2.16 M-sample result
12-14 ms for the first two. `esses-cli eval` of `'SPEED'[m/s] * 2` over the 3.6 M samples of
`synthetic-acc-dialect`, decoding of the `.ld` columns included, 55 ms (03 §14.4: under 100 ms).

**Tests** (`tests/lang/`, Catch2): `parser_tests.cpp` (token kinds and spans, literals, keywords, comments, the
precedence table under both compat settings, `I-COMPAT-POW`, word operators, every syntax code, error recovery,
hostile nesting, `print` round trips under both settings, `sourceHash`), `type_checker_tests.cpp` (kinds, units,
dimensions, rates, every checker code, text comparisons, `choose`, `LapScalar`, calculus units, the signature table
against the registry manifests once `registerIntrinsics` fills them), `compile_eval_tests.cpp` (golden values over
in-memory channels: arithmetic, unit and result conversions, validity rules, mixed rates, constants and text
details, random functions, block-size independence, cancellation, integer and bit operations; plan inputs, stages,
dtype, `defHash`, refusals), `math_module_io_tests.cpp` (the schema examples, the written form, unknown members,
units, refusals with JSON pointers, save and load), `elementwise_bench_tests.cpp` (the hidden measurement),
`plan_stream_tests.cpp` (D-191, seed `ESSES_TEST_SEED`: the stream pulled in random pieces, and `evaluateWhole`,
against the whole-extent evaluation of 0.1 through `Kernel::run` with bitmaps, bit for bit, for 49 expressions over
every kernel class with invalid runs, a re-prime gap and resampled inputs; `restartAt` at chunk boundaries and
arbitrary samples against the continuous stream; the elementwise kernel's in-place loads; the I32 truncation).
Fuzzing:
`tests/fuzz/fuzz_maths_parser.cpp` (parse under both settings, print round trip, checker with and without data,
compile).

## The kernels (`src/kernels`, M2)

| File | What | Design |
|---|---|---|
| `intrinsics.cpp` | the name and class table; `registerIntrinsics` (one `FunctionManifest` per intrinsic: arguments and arities from the language's signature table, argument kinds and dimensions, the unit rule of 03 §3.2 rule 9, our one-line description, provenance `intrinsic`; `blockCapable` for the elementwise and range functions); `makeIntrinsicKernel` (dispatch by kernel class) | 03 §5, §10.1 |
| `windowed.cpp` | `smooth`, `derivative` (against time, or against a per-sample y): windows of n points (seconds -> odd n at the result rate), odd centred, even one earlier, truncated at the extent edges, invalid samples left out; sliding F64 sums re-summed every max(4,096, n) outputs, derivative windows of at most 8 points summed directly | 03 §5.2 |
| `iir.cpp` | `filter_lp/hp` (RC sections, orders 1-3, -1, -2) and `filter_cheby_lp/hp/bp/bs` (Chebyshev type I, Butterworth for ripple 0; orders 2/4/6 and zero-phase -4/-8/-12): passes of second-order sections in transposed direct form II, run in place in the output buffer, primed with the steady state of the first (backward: last) valid sample, holding through invalid samples, re-primed after gaps over 1 s; Nyquist through `Kernel::status` | 03 §5.3, D-169 |
| `stateful.cpp` | `integrate`, `integrate_dist` (Corr Dist is its last stage input), `stat_*` (two-pass per segment; without reset over the math window and broadcast), `running_min/max`, `flip_flop`, `edge_delay`, `time_valid`, `time_shift`, `pulse_width`, `previous_sample`; inputs read in place | 03 §5.4, D-080, D-082 |
| `range.cpp` | `range_is`, `range_change` from `EvalContext::ranges` (chunk-independent; each range's samples from its bounds through `Rate::indexAt`, no time per sample); `E-RANGE-UNKNOWN` through `Kernel::status` listing the source's paths | 03 §5.5 |
| `variance.cpp` | `variance_time`, `variance_dist`: Main minus reference at the same time or distance from the lap start with both laps' offsets and stretch; a reference lap in another source is read through `EvalContext::getChannel` when the argument is a channel | 03 §5.6, D-081 |
| `table.cpp` | the Table kernel of a `table2d` item (`kern::makeTableKernel`): linear/next/previous, clamp/invalid, uniform fast path, values converted to the base unit; `E-TABLE-AXIS` | 03 §4.4, §5.7 |
| `kernel_util.hpp`, `kernels_internal.hpp` | `Reader`/`Writer` (an F64 view through its pointer, its bitmap from the NaN marks), `readAt`/`writeAt` for other dtypes, the literal parameters of a call, window sizes, `kernelError`; the factories per class | |
| `stream.*` | `kern::Streamable` (D-191): `begin`, `step(from, to)` with the state carried, `behind`/`ahead`, `canStream`, `restartPoint`/`beginAt`; `runStreamed` (`Kernel::run` of a streamable kernel: one step over the view); `noNaN` | 03 §6.0, §6.1 |
| `function_registry.cpp`, `kernel_stubs.cpp` | `FunctionRegistry` (final since the contracts), the `MathInputGatherer` constructor | 03 §10.1, §6.3 |

**Conventions.** Every non-elementwise kernel declares `ExtentMode::Whole` (Range: `Parallel`). Smooth, derivative,
the forward-only filters (RC orders 1-3, Chebyshev 2/4/6), integrate, running, flip_flop, edge_delay, time_valid,
time_shift, pulse_width, previous_sample, range and table are `kern::Streamable` (D-191): the evaluator steps them
through the extent in pieces, and their `run()` is one step over the view; the zero-phase and backward filters, the
statistics and the Variance kernels see the whole extent in one `run()` (03 §6.0 item 3). Elementwise, range, table,
time_shift, previous_sample and the direct derivative keep no state along the extent, and smooth and the sliding
derivative start afresh at every re-summation point: they restart at any chunk (`restartPoint`). Where a step's
input has no invalid sample, smooth and the two-point derivative take loops without the validity tests that do the
same operations in the same order (the derivative's abscissa terms evaluated exactly), and the RC passes keep their
state in registers, several forward passes interleaved per sample. Values are read as doubles from any dtype and
accumulated in F64; an invalid output sample is NaN with a cleared validity bit. Parameters come from the literal nodes `compile()` folds into the
AST (`IntLit` points/orders/counts, `DecLit` seconds/hertz/ripple, `StrLit` range paths, `Keyword` rise/fall). A
failure known only at evaluation (a cutoff at or above half the result rate, an unknown range path) is reported by
`Kernel::status()`, which `evaluateWhole` checks after `reset()` and `run()` (D-170). The readings behind every
function are the rows of `docs/maths-compat.md` (`ew.*`, `window.*`, `smooth.*`, `derivative.*`, `iir.*`,
`stateful.*`, `integrate*.*`, `stat.*`, `running.*`, `flip_flop.*`, `edge_delay.*`, `time_shift.*`, `pulse_width.*`,
`previous_sample.*`, `range.*`, `table.*`, `variance.*`, `rate.*`), each with its case files.

**Case corpus (AC-M2-02, D-170).** `tests/maths/cases/<id>.case.json` (schema `maths-case-1`, 238 cases at M2) are
written by `tools/maths_ref.py`, an independent per-sample reference in Python (brute-force windows, two-pass least
squares, the IIR filters as one full-order direct-form-I recursion, D-008 grid arithmetic); never edit one by hand:
change the reference, run `python -I tools/maths_ref.py --write tests/maths/cases`, and `--check` in review.
`tests/kernels/case_runner.hpp` builds an in-memory source from a case (channels with their dtype, interpolation and
validity, constants, details, range groups, Main/reference with offsets and stretch, Corr Dist distance tables) and
runs `Parser`, `compile` (plus the Table stage for a `table2d` case, as the host appends it) and `evaluateWhole`.

**Tests** (`tests/kernels/`): `case_corpus_tests.cpp` (the corpus, at least 150 files), `kernel_tests.cpp` (one
manifest per intrinsic that agrees with the signature table and validates as `mathfn-manifest-1`; the kernel class of
each non-elementwise call; `makeIntrinsicKernel` refusals; `Kernel::status` for Nyquist and an unknown range path;
`E-NO-DISTANCE`; Table refusals; halos and extents), `kernel_property_tests.cpp` (seed `ESSES_TEST_SEED`: sliding
windows against brute force on random inputs with invalid samples; window and integral identities; RC filters against
the scalar recursions; zero phase symmetric in time; low passes keep a constant and high passes remove it;
bit-identical repeats and validity = NaN; Table lookups against a linear search), `kernel_bench_tests.cpp` (hidden
measurement `"[.kernel-bench]"` over 21.6 M samples). This machine (Release, 16 logical processors, other builds
running), `evaluateWhole` including the gather and the result, median of 3: `filter_lp(x, 5, -2)` 340 ms (the v1.0
gate of 07 §12 is 150 ms; AC-M2-10 records it only), `filter_cheby_lp(x, 5, -8, 0.5)` 347 ms, `smooth(x, 0.5)` at
1 kHz 310 ms, `derivative(x, 11)` 377 ms, `integrate(x, x > 0)` 296 ms, `stat_mean(x)` 167 ms,
`range_change("Outings:Laps")` over 200 laps 101 ms, and `x * 1` (the gather and the result alone) 97 ms.

## The graph (`src/graph`, M2)

| File | What | Design |
|---|---|---|
| `graph_internal.hpp` | `MathGraph::Impl`, the node record (`graph::MathNode`), the stored documents, `FrozenScope` (the data-backed scope frozen per item for its tasks), `EvalMemo`, shared helpers | 03 §4.3, §7 |
| `math_graph.cpp` | the public members: the documents (`defineBuiltin`, `define`, `remove`, `setEnabled`, `setModules`, `setResolverTables`, `setCompat`, `setModuleEnabled`, `setSource`), selection re-keying, `invalidate`, completions, status, `nodes()`, descriptors, `trace` | 03 §7.1-7.3, §8 |
| `graph_build.cpp` | `rebuild()`: nodes, chains, conditions, the Scope order of 03 §3.3, cycles, compile, versions | 03 §3.3, §4.2-4.4, §7.2 |
| `graph_eval.cpp` | `require()`: one task per node through `MathsScheduler`, the item and built-in tasks, sealing into the `ChunkCache`, the math window; `buildPlan` (the table stage when `compile()` has not appended it) | 03 §6.0, §7.4-7.6 |
| `stream_provider.*` | `StreamPlanProvider` (D-191): an expression node's chunks pulled from a `PlanStream`, rounded and written as each chunk is reached; chunk-independent (restartable plans: an evaluation per worker restarted at the chunk; others: one shared stream under a lock) | 03 §6.0, §6.2, §7.2 |
| `maths_scheduler.cpp` | `MathsScheduler::submit` (share key `<prefix>/all`, `/c<n>` for the v1.0 chunk path) | 03 §7.4 |
| `gps_corrections.cpp` | `esses.gps_corrections` v1: the zero-fix step; settings schema `schemas/builtin-esses.gps_corrections-1.schema.json` with examples | 03 §9.2 |

**How it works.**
- The graph keeps the documents (built-in registrations, the global modules in project order, the local and the
  transient module, the project's mappings and aliases, the compat switches, the selection) and rebuilds what is
  derived from them on every structural change: the nodes in chain-building order (built-in outputs, then local,
  global and transient items), one chain per case-folded name (D-171 (a), (b)), the module conditions (D-171 (c)),
  and the resolution of every referenced name in the order of 03 §3.3: mappings, aliases (`W-ALIAS-FALLBACK` past a
  missing candidate or a maths candidate with a definition error, D-171 (d); alias cycles with their path), the
  chain head (for a self-maths reference the enabled element before the requester), the logged channel (canonical
  name, then logged name), a numeric or text detail, a constant (own module, local, other enabled global modules
  whose condition holds, in project order: `W-CONST-AMBIGUOUS`; a channel and a constant of one name:
  `W-CONST-SHADOWED`). Each reference is a `MathInput` with its `ResolutionTrace` (`nodes()`).
- Nodes build in an approximate dependency order, then depth first, so an item compiles after what it reads; a
  reference to a node still on the build stack closes a cycle: every node of it gets `E-CYCLE` with the path from
  itself ("'A' → 'B' → 'C' → 'A'"), and its consumers fail with that code when evaluated. Nesting beyond 256 levels
  is `E-EXPR-TOO-DEEP`. An item whose compile signature (item, compiled text, compat switches, resolved inputs) is
  unchanged reuses the previous compile.
- Kinds: an expression compiles as written; a filter item compiles `smooth('N', points | seconds)`, a scale_offset
  item `'N' * scale + offset` with the offset converted as a delta into the unit N is stored in (03 §4.4); a
  `table2d` item its axis expression with the Table stage; `table3d` and `regression` are `E-KIND-UNSUPPORTED`,
  `plugin` `E-PLUGIN-UNAVAILABLE` (the items are kept). A `builtin` item overrides a registered built-in module's
  settings for its source (D-171 (f)). `integrate_dist` resolves `Corr Dist` like a reference (D-170).
- Each item compiles against a `FrozenScope` of what its references resolved to; its task compiles a fresh `Plan`
  from the same AST and scope on the worker (kernels keep state) and never reads the graph (01 §5.3 rule 9). Inputs
  are logged series or the upstream nodes' sealed columns; `EvalContext::getChannel` converts from the unit an
  input is stored in into the unit the plan asks for. Built-in outputs keep their M1 tasks.
- Versions (03 §7.2, D-140, D-171 (h)): the definition hash (`compile()`'s: canonical expression, kind, rate, result
  unit, interpolation, precision, enumeration, table data, compat switches; built-ins: module id and version,
  canonical settings, output definition) with the node's identity, enable state, chain index, the sorted upstream
  versions, exactly the logged columns read, the range-group versions of the plan's range paths, the values of the
  details and constants read, the layer extent and the result rate. A rebuild keeps the column, status and
  evaluation of every node whose identity and version hold and cancels the superseded evaluations; comments,
  spacing and display settings re-version nothing.
- `SelectionDependent` nodes (the plan's flag, or an upstream one) evaluate with the math window of 03 §7.6 and cache
  under `xxh3(MathSelection::key, distance generation)`; a node keeps the columns of its 8 latest selections.
- Tasks of one version and selection share an `EvalMemo`: a `require` that races the completion of the node's task
  (whose share key no longer coalesces once it is done) returns the sealed column instead of evaluating again.
- `PlanProvider` copies a chunk without an invalid sample in one vectorised loop (I32: rounded half away from zero,
  clamped), and only builds a bitmap for chunks that need one. It serves the built-in outputs.
- An item's task compiles its plan and starts a `PlanStream` (so `E-ARG-VALUE`, `E-RANGE-UNKNOWN` and the other
  evaluation failures come before any column), then seals a `Column` over a `StreamPlanProvider` (D-191): each
  chunk is pulled from the stream, rounded and written, and sealed (pyramid, summary) while its samples are in the
  cache; the result never exists as a whole-extent buffer. The provider is chunk-independent, so `Column::require`
  offers the node's later chunks to idle workers, one priority band below the node's (they never overtake another
  node's task) while the node's task builds the chunks in order and takes back any still queued. A plan whose stages
  all restart is evaluated per chunk by whichever worker builds it (an evaluation of its own, restarted at the
  chunk; the samples are the same); any other plan runs one stream in order under a lock, and a worker that needs a
  later chunk advances it and leaves the chunks it passes for their own requests, so the rounding, writing and
  sealing spread while the evaluation stays sequential. Evaluations are released after the last chunk; a later
  request (eviction, a summary rebuild) starts afresh. A cancelled evaluation is retried; any other failure is
  memoised.

**Tests** (`tests/graph/`, Catch2, tags `[graph]`, `[gps]`): `graph_shadowing_tests.cpp` (AC-M2-03: the chain order
and its swap, transient last, disabled elements skipped, the first definition per module, a filter item reading the
unshadowed input and equal to `smooth()` of the logged channel, cycles with the path, conditions, constants, details,
mappings, aliases with fall-through and cycles), `graph_invalidation_tests.cpp` (AC-M2-04: a 50-item module, one edit
re-evaluates its 3-node cone, asserted by versions and `evaluationCount()`; constant, cosmetic, sidecar, Recalculate,
compat and module-enable cases; `SelectionDependent` re-keying and switching back without an evaluation),
`graph_items_tests.cpp` (declared units, explicit rates, scale_offset deltas and the shadowed descriptor, table2d,
the unsupported kinds, `define()` diagnostics, built-in outputs read by items, the `builtin` override,
`integrate_dist`), `gps_corrections_tests.cpp` (defaults, the schema and its examples, the zero-fix step, Disabled
without channels, the item's `enabled`, prefixed outputs), `maths_scheduler_tests.cpp`, `stream_provider_tests.cpp`
(D-191: a node's chunks equal `PlanProvider`'s chunks of `evaluateWhole` bit for bit, pyramid included, for F32,
F64 and I32 columns with invalid and overflowing samples; chunks built concurrently in any order and again after the
release; failures remembered, cancellations not; MathGraph's columns equal `evaluateWhole` while the nodes run at
once); `math_graph_tests.cpp` (M1) now checks that a second built-in module shadows the first.

**Performance** (AC-M2-10; `bench/maths_bench`, `bench/workloads.md`): see "Maths readiness set" in
`bench/workloads.md` and `docs/milestones/M2.md` (AC-M2-10 row and open issue 5: the two-frame budget is met on this
machine since D-191; the remaining cost per kernel and in the store's seal). `esses-maths-bench --digest` prints a
fingerprint of every output of the readiness set and of 67 expressions over every kernel class; an evaluation change
that must not change results leaves every line unchanged. `--workers 1` measures the serial cost of the set.

## M2 contract (07 §5.1; `docs/milestones/M2.md` §2)

Three source roots, one owner each; the public headers stay in `include/esses/maths/`.

| Root | Headers | Contents | Design |
|---|---|---|---|
| `src/lang/` | `lexer.hpp`, `ast.hpp`, `types.hpp`, `parser.hpp`, `scope.hpp`, `type_checker.hpp`, `math_module_io.hpp` | `TokenKind`, `Token`, `Lexer`; `NodeKind`, `UnaryOp`, `BinaryOp`, `Node`, `Ast`, `print`; `Kind`, `Type`; `Parser`; `ScopeEntry`, `Scope` (the data-less base class; `MathGraph` derives the data-backed one); `TypeChecker`; `MathModuleIo` | 03 §2-§4.1, §11 |
| `src/kernels/` | `kernel.hpp`, `plan.hpp`, `intrinsics.hpp`, `math_function.hpp`, `function_registry.hpp`, `math_input_gatherer.hpp` | `KernelClass`, `ExtentMode`, `BlockView`, `ArenaView`, `BlockCtx`, `Kernel`, `kBlockSamples`; `PlanInput`, `Stage`, `Plan`, `compile`, `evaluateWhole`; `IntrinsicId`, `intrinsicName`, `intrinsicClass`, `registerIntrinsics`, `makeIntrinsicKernel`; `ArgSpec`, `FunctionManifest`, `SeriesValue`, `RangePath`, `Keyword`, `InvalidValue`, `Value`, `IMathFunction`; `FunctionRegistry`; `MathInputGatherer` | 03 §5, §6, §10.1 |
| `src/graph/` | `math_graph.hpp` (M2 members), `maths_scheduler.hpp`, `gps_corrections.hpp` | `NodeId`, `MathGraph::define/remove/setEnabled/setModules/setSelection/trace`; `MathTaskKey`, `MathsScheduler`; `GpsCorrectionsModule` (`esses.gps_corrections`, zero-fix step) | 03 §4.3, §7, §9.2 |
| (shared) | `math_item.hpp`, `math_module.hpp`, `math_selection.hpp`, `maths_compat.hpp`, `eval_context.hpp` | `UnitValue`, `DisplayDefaults`, `RateSpec`, `TableSpec`, `ItemKind`, `MathItem`; `MathConstant`, `MathEnumeration`, `MathModule`; `MathRangeGeom`, `MathSelection`; `MathsCompat::functionAliases`; `EvalContext::getChannel` | 03 §4.1, §7.5 |

### M2-STUB items the implementing agents must replace

- `src/lang/` (the language): none left. `compile`, `evaluateWhole` and `MathInputGatherer::fill` moved there from
  `src/kernels/kernel_stubs.cpp`, next to the `EwKernel` that `compile` emits (the constructor stays there).
- `src/kernels/` (the kernels): none left (`registerIntrinsics`, `makeIntrinsicKernel` and every kernel class are
  implemented, see "The kernels" above).
- `src/graph/` (the graph): none left. The M1 `MathGraph` moved from `src/math_graph.cpp` to
  `src/graph/math_graph.cpp`; every compat switch, `functionAliases` included, is part of the definition hashes.

Consumers: `esses_project` and `esses_qtmodel` (`MathModule`, `MathItem`, `MathModuleIo`), `esses_qtmodel`
(`Parser`, `TypeChecker`, `Scope`, `FunctionRegistry::builtin`, the `MathGraph` M2 members, `GpsCorrectionsModule`),
`esses_widgets` (`Diagnostics`, `MathModule`). Tests: `tests/m2_contract_tests.cpp` (intrinsic table, registry,
data-less scope, `ArenaView`, value-type defaults).

Contract rules (all M2 libraries): an `M2-STUB` is replaced in place (or moved into the owner's root) by
its implementation and the marker removed; `grep -rn "M2-STUB" libs` lists what is left. A signature that another
library already consumes (named under "Consumers" below) changes only together with that consumer; the owner may
reshape the others. Tests of a stub's behaviour belong to its owner; the `*contract*` tests cover what is final.

### Deviations of the M2 contract from the design text

- The evaluator (D-191): 03 §6.0 runs every 0.1 plan as one whole-extent task with whole-extent buffers between
  stages; here the task streams the plan in pieces (`PlanStream`, internal `kern::Streamable`) with the same values,
  and an expression node's chunks are sealed as they are produced, by any idle worker (`StreamPlanProvider`,
  chunk-independent), instead of slicing an evaluated extent (`PlanProvider`, which keeps the built-in outputs). The
  public `Kernel`, `BlockCtx` and `ExtentMode` are unchanged; `BlockCtx::temps` is empty in the 0.1 evaluator (no
  kernel uses it).
- The kernels (M2): `Kernel` adds `status()` and `analysis::RangeTables` adds `contains`/`paths` (D-170); `compile()`
  gives `integrate_dist` the scope's `Corr Dist` as its last stage input (D-170); the IIR filters are designed in-tree
  instead of through Iir1 (D-169); the manifest's `returns.unitRule` of `pulse_width` is `declared:s` (03 §5.4 says
  "time (s)"); `kern::makeTableKernel` (internal) builds the Table kernel of 03 §5.7 from a `TableSpec`.

- The language (M2): `ScopeEntry` adds `interp`, `dtype`, `number` (constants and numeric details, in `type.unit`),
  `text` (text details) and `diags` (what the lookup reports, e.g. `W-CONST-AMBIGUOUS`), which `compile` needs and
  03 §3.3 does not list; `Scope` adds `dataBacked()` (a miss is `E-UNKNOWN-CHANNEL` rather than `I-UNRESOLVED`; the
  base class answers false for itself and true for a derived class); `TypeChecker` adds
  `check(Ast&, const Scope&, const Options&, Diagnostics&)` with `Options{requester, resultUnit}` (the requester for
  self-maths lookups and the declared unit of 03 §3.2 rule 10). The `Convert` node keeps the unit's spelling in
  `Node::text` and the checker sets `Node::unit` when the conversion applies (the parser has no unit registry).
  `compile` type-checks its own copy of the AST. A bare literal takes the dimension of what it meets
  (`lang.literal-dimension` in `docs/maths-compat.md`, D-164); the diagnostic codes beyond 03 §2.5 and the nesting
  limit are D-165; `esses-cli eval` on the calling thread is D-166.

- `MathItem::module` (M1) is spelled `moduleId` as 03 §4.1 writes it, and `moduleVersion` defaults to 1;
  `DisplayDefaults` moved from `builtin_module.hpp` to `math_item.hpp`, and `MathRangeGeom`/`MathSelection` from
  `eval_context.hpp` to `math_selection.hpp` (03 §1 lists `MathItem.h` and `MathSelection.h`); the old headers
  include the new ones.
- `TableSpec` (named by 03 §4.1 without members) holds the `table2d`/`table3d` fields of the maths-1 example with
  nested `Axis`, `Interp` and `OutOfRange`; `MathModule` adds `kLocalId`, `kTransientId` and `unknown` (preserved
  top-level members, 02 §6.1).
- `MathInputGatherer` takes the series alone (its values `Column` is 03 §6.3's `src`) plus the result grid's `t0`,
  and `fill` returns `Result<void>`; `EvalContext::getChannel` returns `Result<MathInputGatherer>` as 03 §7.5 writes
  it (the M1 deviation that omitted it is closed).
- `Stage` stores its inputs as `Stage::Ref {Stage | Input, index}`, `PlanInput` adds the target `unit`, and `Plan`
  adds `rangeGroups` (the group paths whose `rangeVersion` enters the node version, 03 §7.2). `evaluateWhole` is the
  0.1 whole-node evaluation of a plan (03 §6.0).
- `FunctionRegistry::addExternal` returns `Result<void>` (E-FN-COLLISION reaches the caller) and adds
  `intrinsicId`, `external` and the process-wide `builtin()`; `ArgSpec::Kind` is a scoped enum.
- `MathGraph::trace` returns the trace by value; `setModules` and `setSelection` return the re-versioned keys like
  the M1 mutators. `MathsScheduler::submit(key, text, priority, cancel, work)` appends `/all` (or `/c<n>`) to `text`
  for the pool's share key; `MathTaskKey::node` names the task in code.
- The graph (M2, D-171): built-in outputs first and transient maths last in a chain, disabled elements skipped,
  conditions folded over details and module constants (`E-CONDITION`), alias fall-through on definition errors, the
  descriptor's unit as stored; `MathGraph` adds `nodes()`, `status(NodeId)`, `setResolverTables`, `setCompat`,
  `setDistanceIndexes`, `invalidate`, `evaluationCount` and `Impl` is named publicly (opaque) for the graph's
  sources; `NodeId::kIgnored`; `ColumnHandle::selectionKey`; `BuiltinOutput::required`/`missingSummary` and
  `IBuiltinModule::disabledItemDisablesOutputs`; `prefetch` requires the built-in outputs only. A built-in output
  named like an input reads its logged inputs (its `WholeExtentFn` reads the source), not the element before it.
