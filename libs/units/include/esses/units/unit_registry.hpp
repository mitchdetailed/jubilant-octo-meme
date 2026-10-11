// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// UnitRegistry (02 section 2, D-010, D-141): quantities with a dimension and a display family, affine units,
// and the spellings that resolve to them, loaded from data/units.json (schema units-1) plus an optional
// per-user units.user.json.
//
// Model. Samples and persisted limits are stored in the base unit of their quantity; a unit converts as
// value_in_unit = value_in_base * scalar + offset. Conversion is allowed only within one display family, so
// Temperature and Temperature Delta, Pressure and Pressure Delta, Torque and Energy, Angular Speed and
// Frequency never convert into each other even though their dimensions match (02 section 2.2 rule 2).
//
// Ids are process-local, assigned in load order (shipped table first, then the user file) and never written
// to files (02 section 2.2 rule 6). Unit 0 is the unitless unit with the empty symbol (kNoUnit) and quantity
// 0 is "Unitless". A loaded registry is immutable; every const member is safe to call from any thread.
//
// Lookup (find / match), 02 section 2.2 rule 3. Leading and trailing white space is dropped and inner runs of
// white space collapse to one space, then, in order:
//   1. exact: the symbol, an alias or the display form, case-sensitively;
//   2. case-folded: the same spellings compared after esses::caseFold (NFKC-lite + full case folding), except
//      spellings a unit lists as caseSensitive ("g" the gram does not swallow "G" the acceleration unit);
//   3. normalised: both sides through the spelling normaliser: the degree glyphs and their common mojibake
//      (°, º, ˚; ø and ░ before C or F) become "deg", "degree(s)" becomes "deg", "degC" becomes "deg C",
//      µ/μ becomes u, Ω becomes ohm, λ becomes lambda, the multiplication signs * · ⋅ × become ".", and "^"
//      before a digit or sign is dropped (m/s^2 = m/s² = m/s2).
// A spelling that resolves nowhere is not an error here: the importer's unit mapping decides (D-028).
// find(spelling, quantity) restricts the search to one quantity's units and treats every spelling of that
// quantity case-insensitively (a dictionary that says Acceleration + "g" means the acceleration unit).
#pragma once

#include <esses/base/config.hpp>
#include <esses/base/result.hpp>
#include <esses/units/converter.hpp>
#include <esses/units/dimension.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace esses::units {

using UnitId = std::uint16_t;
using QuantityId = std::uint16_t;

inline constexpr UnitId kNoUnit = 0;       // the unitless unit (empty symbol), base of quantity 0
inline constexpr UnitId kNone = kNoUnit;   // spelling used by 03 section 3.2
inline constexpr QuantityId kUnitless = 0; // the "Unitless" quantity

struct Quantity {
    QuantityId id = kUnitless;
    std::string name; // "Temperature", "Temperature Delta", "Angular Speed"
    Dimension dim;
    std::string family;    // display family: distinct for Delta vs Absolute, Torque vs Energy
    UnitId base = kNoUnit; // the SI base unit of this quantity (scalar 1, offset 0)
    bool isDelta = false;
    std::optional<QuantityId> deltaFamily; // Temperature -> Temperature Delta; Pressure -> Pressure Delta
    std::vector<std::string> aliases;      // further names quantityByName accepts ("Angular Velocity")
};

struct Unit {
    UnitId id = kNoUnit;
    std::string symbol;  // canonical ASCII key written in files: "km/h", "kPa a", "deg C"
    std::string display; // Unicode form: "°C", "µs", "m³/s"; empty = symbol
    QuantityId qty = kUnitless;
    double scalar = 1.0; // value_in_unit = value_in_base * scalar + offset
    double offset = 0.0;
    std::vector<std::string> aliases;       // case-insensitive import spellings, legacy spellings included
    std::vector<std::string> caseSensitive; // spellings of this unit that match only exactly ("g")
    std::string compat;                     // "" or a tag ("legacy"): kept so imported data resolves; not
                                            // offered in unit pickers

    // The text to show: display when set, else the symbol.
    std::string_view label() const noexcept { return display.empty() ? std::string_view(symbol) : display; }
    bool isCompat() const noexcept { return !compat.empty(); }
};

// How find() matched a spelling (for resolution traces, 02 section 4.6).
enum class MatchKind : std::uint8_t {
    Symbol,     // the unit's symbol, exactly
    Alias,      // an alias or the display form, exactly
    CaseFolded, // a symbol, alias or display form after white-space collapsing and case folding
    Normalised  // after the spelling normaliser
};
std::string_view matchKindName(MatchKind kind) noexcept; // "symbol", "alias", "case-folded", "normalised"

struct UnitMatch {
    UnitId unit = kNoUnit;
    MatchKind kind = MatchKind::Symbol;
    friend constexpr bool operator==(const UnitMatch&, const UnitMatch&) = default;
};

namespace detail {
struct SpellingHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};
template <class V> using SpellingMap = std::unordered_map<std::string, V, SpellingHash, std::equal_to<>>;
} // namespace detail

class UnitRegistry {
public:
    static constexpr std::string_view kSchemaId = "urn:esses:units-1";
    static constexpr int kSchemaMajor = 1;
    static constexpr int kSchemaVersion = 1; // newest minor this build writes and understands fully

    // Builds a registry from the text of data/units.json and, when userJson is not empty, units.user.json.
    // Errors: Format (JSON syntax; context.offset), Unsupported (a newer schema major; context.schemaId),
    // Invalid (the content breaks a units-1 rule; context.detail is the JSON pointer of the offending member,
    // the message names the rule). A failing user document fails the whole call; a caller that wants to go on
    // without it loads the shipped table alone.
    static Result<UnitRegistry> load(std::string_view unitsJson, std::string_view userJson = {});
    // The same from files: unitsFile must exist (Io otherwise); userFile is skipped when empty or absent.
    // Errors carry context.path of the file at fault.
    static Result<UnitRegistry> loadFiles(const std::filesystem::path& unitsFile,
                                          const std::filesystem::path& userFile = {});

    // ---- lookup -----------------------------------------------------------------------------------------
    std::optional<UnitId> find(std::string_view symbolOrAlias) const;
    std::optional<UnitId> find(std::string_view symbolOrAlias, QuantityId within) const;
    std::optional<UnitMatch> match(std::string_view symbolOrAlias,
                                   std::optional<QuantityId> within = std::nullopt) const;

    // ---- units and quantities ---------------------------------------------------------------------------
    bool contains(UnitId id) const noexcept { return id < units_.size(); }
    bool containsQuantity(QuantityId id) const noexcept { return id < quantities_.size(); }
    // An invalid id is a programming error (ESSES_ASSERT); release builds fall back to unit / quantity 0.
    const Unit& unit(UnitId id) const;
    const Quantity& quantity(QuantityId id) const;
    QuantityId quantityOf(UnitId id) const;
    // By name or quantity alias, case-insensitively ("angular velocity" finds Angular Speed).
    std::optional<QuantityId> quantityByName(std::string_view name) const;
    // A quantity's units in units.json order (display lists); the base unit is one of them.
    std::span<const UnitId> unitsOf(QuantityId id) const;
    std::span<const Unit> units() const noexcept { return units_; }
    std::span<const Quantity> quantities() const noexcept { return quantities_; }
    std::size_t unitCount() const noexcept { return units_.size(); }
    std::size_t quantityCount() const noexcept { return quantities_.size(); }

    // ---- conversion -------------------------------------------------------------------------------------
    // fail(ErrorCode::Invalid, detail "unit.family") when the units' families (or dimensions) differ, for
    // example deg C -> dC or kPa a -> kPa g; detail "unit.id" for an unknown id. from == to is the identity.
    Result<Converter> converter(UnitId from, UnitId to) const;
    Converter toBase(UnitId id) const;   // unit -> base unit of its quantity; identity for an invalid id
    Converter fromBase(UnitId id) const; // base unit -> unit

    // ---- quantity relations -----------------------------------------------------------------------------
    // The delta quantity a difference of two values of `id` belongs to (Temperature -> Temperature Delta);
    // nullopt when the quantity has no separate delta family (the difference keeps the quantity).
    std::optional<QuantityId> deltaOf(QuantityId id) const;
    // The inverse relation: the absolute quantity whose deltaFamily is `id` (Temperature Delta ->
    // Temperature).
    std::optional<QuantityId> absoluteOf(QuantityId id) const;
    // The quantity of a dimension. Several quantities can share one (Temperature, Temperature Delta); a
    // non-empty familyHint picks the one whose family or name matches it case-insensitively, otherwise the
    // first non-delta quantity in units.json order wins (Unitless for a pure number, Frequency for T^-1).
    // nullopt for an unknown dimension or when no quantity has it.
    std::optional<QuantityId> byDimension(const Dimension& dim, std::string_view familyHint = {}) const;

private:
    friend class RegistryBuilder;
    UnitRegistry() = default;

    struct Affine { // toBase: base = toA * x + toB; fromBase: x = fromA * base + fromB
        double toA = 1.0, toB = 0.0, fromA = 1.0, fromB = 0.0;
    };
    struct UnitKeys { // pre-computed lookup keys of one unit, for scoped lookups
        std::vector<std::string> exact;
        std::vector<std::string> folded;
        std::vector<std::string> normalised;
    };

    std::optional<UnitMatch> matchScoped(std::string_view collapsed, QuantityId within) const;

    std::vector<Quantity> quantities_;
    std::vector<Unit> units_;
    std::vector<Affine> affine_;
    std::vector<UnitKeys> keys_;
    std::vector<std::vector<UnitId>> unitsOfQuantity_;
    std::vector<std::optional<QuantityId>> absoluteOf_;
    detail::SpellingMap<UnitId> exact_;
    detail::SpellingMap<UnitId> folded_;
    detail::SpellingMap<UnitId> normalised_;
    detail::SpellingMap<QuantityId> quantityNames_; // case-folded names and aliases
};

} // namespace esses::units
