// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Details (02 section 4.2): the typed key/value facts of a source ("Driver", "Venue", "Diff Ratio",
// "Log Date") with provenance, and the merge precedence sidecar > companion > file (D-015, 02 section 5.3
// rule 2).
#pragma once

#include <esses/base/result.hpp>
#include <esses/model/notes.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace esses::model {

// Where a detail, a marker or a range override comes from (D-014). Logged = the log file; Companion = the
// .ldx (or another companion); User = a sidecar edit; Derived = computed by Esses (rules, file facts, the
// SOL/EOL an importer synthesises).
enum class Provenance : std::uint8_t { Logged, Companion, User, Derived };
std::string_view provenanceName(Provenance p) noexcept; // "Logged", "Companion", "User", "Derived"
std::optional<Provenance> parseProvenance(std::string_view text) noexcept;

// An absolute instant with the UTC offset it was written in. iso() gives
// "YYYY-MM-DDTHH:MM:SS[.fff|.ffffff](Z|+HH:MM|-HH:MM)": the fraction is omitted when zero, three digits when
// it is a whole number of milliseconds and six otherwise; offset 0 is written "Z".
struct DateTime {
    std::int64_t unixUs = 0;      // microseconds since 1970-01-01T00:00:00Z
    std::int32_t tzOffsetMin = 0; // offset of the local wall time from UTC, minutes

    std::string iso() const;
    // Strict ISO 8601 with seconds and an offset (the sidecar-1 isoDateTime pattern); up to nine fraction
    // digits, truncated to microseconds. nullopt for anything else or an impossible date.
    static std::optional<DateTime> parseIso(std::string_view text) noexcept;
    static DateTime nowUtc();

    friend bool operator==(const DateTime&, const DateTime&) = default;
};

// A numeric detail value. `v` is expressed in the unit spelled by `symbol` (a units-1 symbol or alias as
// written; "" = unitless). Importers that build details in memory use the quantity's base unit and set both
// `unit` and `symbol`; a document loaded from disk carries the written symbol only, and resolveUnit() fills
// `unit` once a UnitRegistry is at hand (unit ids are process-local and never persisted, 02 section 2.2 rule
// 6). Keeping the written spelling makes a sidecar round trip byte-identical (D-074).
struct Quantityd {
    double v = 0.0;
    units::UnitId unit = units::kNoUnit; // resolved id of `symbol`; kNoUnit while unresolved or unitless
    int dps = 2;                         // display decimals
    std::string symbol;                  // the unit as written ("ratio", "m", "deg C"); "" = unitless

    friend bool operator==(const Quantityd&, const Quantityd&) = default;
};

using DetailValue = std::variant<std::string, Quantityd, DateTime>;

struct Detail {
    std::string id; // "Driver", "Venue", "Vehicle Id", "Diff Ratio", "Log Date", custom
    DetailValue value;
    Provenance prov = Provenance::Logged;
    std::optional<DetailValue> original; // the value this one replaced when prov != Logged

    friend bool operator==(const Detail&, const Detail&) = default;
};

// ---- standard detail ids (02 section 4.2 behaviour 2)
// ------------------------------------------------------
enum class DetailType : std::uint8_t { Text, Number, DateTime };
struct StandardDetail {
    std::string_view id; // "Venue Length"
    std::string_view
        group;       // "Summary", "Statistics", "File", "Weather", "Device", "Vehicle", "Drag", "Esses"
    DetailType type; // Text, Number or DateTime
    std::string_view quantity; // units-1 quantity name of a Number ("Length"); "" = a plain count
    bool derived; // computed after the rules run (Statistics, File); never written to the sidecar
};
std::span<const StandardDetail> standardDetails() noexcept;
// Case-insensitive (caseFold) lookup; "Gear 1" ... "Gear 10" and the drag-run "Time <n> ft" / "Speed <n> ft"
// entries are listed individually.
const StandardDetail* findStandardDetail(std::string_view id) noexcept;

// Detail lookup by id, case-insensitive (caseFold); the first match wins.
const Detail* findDetail(const std::vector<Detail>& details, std::string_view id) noexcept;
Detail* findDetail(std::vector<Detail>& details, std::string_view id) noexcept;

// Plain text of a value for labels and range names: the string, the number with `dps` decimals followed by
// " <symbol>" when it has one, or the ISO form of a date-time.
std::string detailValueText(const DetailValue& value);

// Overlays `upper` onto `lower` (one precedence step of D-015): a detail of `upper` replaces the detail with
// the same id (case-insensitive) in place, takes `upper`'s provenance and records the replaced value as
// `original`; a detail without a counterpart is appended (its own `original` is kept). Order: `lower`'s
// order, then the new ids in `upper`'s order.
std::vector<Detail> overlayDetails(std::vector<Detail> lower, const std::vector<Detail>& upper);
// sidecar > companion > file. Companion details get prov = Companion, sidecar details prov = User.
std::vector<Detail> mergeDetails(std::vector<Detail> file, std::vector<Detail> companion,
                                 std::vector<Detail> sidecar);

// ---- numeric values and units --------------------------------------------------------------------------
// Sets q.unit from q.symbol. Errors: NotFound (context.detail = the symbol) when the spelling resolves
// nowhere; q is left unchanged.
Result<void> resolveUnit(Quantityd& q, const units::UnitRegistry& registry);
// Resolves the unit of every numeric detail (value and original); one Warning note "detail.unit_unknown"
// per detail whose symbol does not resolve.
std::vector<ModelNote> resolveDetailUnits(std::vector<Detail>& details, const units::UnitRegistry& registry);
inline constexpr std::string_view kNoteDetailUnitUnknown = "detail.unit_unknown";
// q.v in the base unit of its quantity; nullopt while q.symbol is non-empty and unresolved.
std::optional<double> toBase(const Quantityd& q, const units::UnitRegistry& registry);
// A value of `unit` with the registry's symbol for it.
Quantityd makeQuantity(double v, units::UnitId unit, const units::UnitRegistry& registry, int dps = 2);

} // namespace esses::model
