# esses_units

Layer 0, Qt-free. Quantities, units and conversions (01_ARCHITECTURE.md section 2.1, 02_DATA_MODEL.md section 2;
D-010, D-068, D-085, D-141):

- `Dimension`: exponents of `[L, M, T, I, Θ, N, J]` with `*`, `/`, `inverse()`, `pow(int)`, `pow(Rational)`;
  `unknown()` for fractional powers and exponent overflow (D-085), `toString()` (`L·T⁻²`) and `toAsciiString()`
  (`L.T^-2`) for diagnostics.
- `Converter`: the affine map `y = a·x + b`, with `inverse()`, `then()` and `applyInPlace()` for sample blocks.
- `Quantity`, `Unit` and `UnitRegistry`: loaded from `data/units.json` (schema `units-1`,
  `schemas/units-1.schema.json`) plus an optional per-user `units.user.json`. Samples and persisted limits are
  stored in the base unit of their quantity; a unit converts as `value_in_unit = value_in_base · scalar + offset`.
  Conversion is allowed only within one display family, so Temperature / Temperature Delta, Pressure /
  Pressure Delta, Torque / Energy and Angular Speed / Frequency never convert into each other.

Public headers live in `include/esses/units/` (`units.hpp` includes them all); the types are in namespace
`esses::units`.

## Lookup

`find(spelling)` and `match(spelling)` (which also reports how it matched) trim and collapse white space, then
try in order: the symbol, an alias or the display form exactly; the same after `esses::caseFold`; the same after
the spelling normaliser (degree glyphs and their mojibake `ø`/`░` before C or F, `degree(s)`, `degC`, micro, ohm,
lambda, `*`/`·`/`⋅`/`×` to `.`, `^` before an exponent). A unit can list spellings in `caseSensitive`; those
match exactly only, which keeps `g` (the gram) and `G` (standard gravity) apart. `find(spelling, quantity)` searches
one quantity and folds every spelling of it, so a dictionary row "Acceleration, g" finds `G`. An unknown spelling
is not an error here; the importer's unit mapping decides (D-028).

## The table

`data/units.json` is our own table: 29 quantities and 111 units from the SI Brochure, NIST SP 811 and standard
automotive units, sized by need (the shipped readers and design examples, and every unit spelling of the private
S-1 corpus, which `tests/private_corpus_tests.cpp` checks when `fixtures/private/s1` is present: each spelling
resolves or is listed with a reason in `tests/data/corpus_unmapped_units.json`). Factors that NIST defines as
"one unit = n base units" are written as `toBase`. Spellings kept only so imported legacy configuration and user
maths resolve (`%Trim`, `Trim`, `1000rpm`, `dBTDC`, `dRTD`, the fuel-specific A/F units) carry `"compat":
"legacy"` and are defined from their physical meaning. AFR is lambda times the stoichiometric ratio: the
fuel-specific A/F units are fixed-scalar lambda units, and a bare `AFR` is left to the unit mapping, because the
vehicle's ratio is the `Stoich Ratio` detail used by the shipped maths item, not a unit (D-068). No vendor unit
file was used.

## Differences from 02 section 2

- `UnitRegistry::load` takes the JSON text (`load(unitsJson, userJson)`) or files (`loadFiles(units, user)`)
  instead of a `JsonDoc`, which is not implemented yet; a `JsonDoc` overload can forward to it.
- Added to the API: `match()` / `MatchKind` (for resolution traces), `find(spelling, quantity)`, `absoluteOf()`,
  `contains()`, `units()` / `quantities()` iteration, `Quantity::aliases`, `Unit::caseSensitive`,
  `Unit::compat`, `Unit::label()`, `kNone` (03's spelling of `kNoUnit`), `kUnitless`.
- `units-1` additions (schema updated): optional `toBase`, `caseSensitive` and `note` on units, `aliases` and
  `note` on quantities, the empty symbol for the unitless unit, and `-128` reserved in `dim`.
- `"compat"` is the neutral tag `legacy`; 02 section 2.3's example tag would fail the vendor-mark lint (D-125).
- `Angular Speed` accepts the quantity name `Angular Velocity` (used by an example in 03_MATHS_ENGINE.md) and
  `Energy` accepts `Energy & Work`.

Must never depend on: any `esses_*` library other than `esses_base`, Qt, or third-party libraries other than the
private nlohmann-json.
