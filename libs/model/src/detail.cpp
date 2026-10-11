// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/casefold.hpp>
#include <esses/model/detail.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <string>
#include <system_error>
#include <utility>

namespace esses::model {

// ---- provenance
// ---------------------------------------------------------------------------------------------

std::string_view provenanceName(Provenance p) noexcept {
    switch (p) {
    case Provenance::Logged:
        return "Logged";
    case Provenance::Companion:
        return "Companion";
    case Provenance::User:
        return "User";
    case Provenance::Derived:
        return "Derived";
    }
    return "Logged";
}

std::optional<Provenance> parseProvenance(std::string_view text) noexcept {
    for (const Provenance p :
         {Provenance::Logged, Provenance::Companion, Provenance::User, Provenance::Derived}) {
        if (text == provenanceName(p)) {
            return p;
        }
    }
    return std::nullopt;
}

// ---- DateTime
// -------------------------------------------------------------------------------------------------

namespace {

constexpr std::int64_t kUsPerDay = 86'400'000'000;
constexpr std::int64_t kUsPerMinute = 60'000'000;

constexpr std::int64_t floorDiv(std::int64_t a, std::int64_t b) noexcept {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Proleptic Gregorian calendar <-> days since 1970-01-01 (the 400-year era decomposition).
constexpr std::int64_t daysFromCivil(std::int64_t y, int m, int d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = floorDiv(y, 400);
    const std::int64_t yoe = y - era * 400;                         // [0, 399]
    const std::int64_t mp = m > 2 ? m - 3 : m + 9;                  // March = 0
    const std::int64_t doy = (153 * mp + 2) / 5 + d - 1;            // [0, 365]
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
    return era * 146'097 + doe - 719'468;
}

struct Civil {
    std::int64_t y;
    int m;
    int d;
};

constexpr Civil civilFromDays(std::int64_t z) noexcept {
    z += 719'468;
    const std::int64_t era = floorDiv(z, 146'097);
    const std::int64_t doe = z - era * 146'097;                                       // [0, 146096]
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36'524 - doe / 146'096) / 365; // [0, 399]
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                 // [0, 365]
    const std::int64_t mp = (5 * doy + 2) / 153;                                      // [0, 11]
    const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    return Civil{yoe + era * 400 + (m <= 2 ? 1 : 0), m, d};
}

static_assert(daysFromCivil(1970, 1, 1) == 0);
static_assert(daysFromCivil(2000, 3, 1) == 11'017);
static_assert(civilFromDays(-1).y == 1969 && civilFromDays(-1).m == 12 && civilFromDays(-1).d == 31);

constexpr bool isLeap(std::int64_t y) noexcept {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

constexpr int daysInMonth(std::int64_t y, int m) noexcept {
    constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && isLeap(y) ? 29 : kDays[static_cast<std::size_t>(m - 1)];
}

void appendPadded(std::string& out, std::int64_t v, int width) {
    if (v < 0) {
        out += '-';
        v = -v;
    }
    std::array<char, 24> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    const auto len = static_cast<int>(r.ptr - buf.data());
    for (int i = len; i < width; ++i) {
        out += '0';
    }
    out.append(buf.data(), r.ptr);
}

// Reads exactly `n` decimal digits at s[pos]; advances pos.
bool readDigits(std::string_view s, std::size_t& pos, std::size_t n, int& out) noexcept {
    if (pos + n > s.size()) {
        return false;
    }
    int v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = s[pos + i];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    pos += n;
    out = v;
    return true;
}

bool expect(std::string_view s, std::size_t& pos, char c) noexcept {
    if (pos < s.size() && s[pos] == c) {
        ++pos;
        return true;
    }
    return false;
}

} // namespace

std::string DateTime::iso() const {
    const std::int64_t local = unixUs + static_cast<std::int64_t>(tzOffsetMin) * kUsPerMinute;
    const std::int64_t days = floorDiv(local, kUsPerDay);
    const std::int64_t usOfDay = local - days * kUsPerDay;
    const Civil c = civilFromDays(days);
    const std::int64_t secOfDay = usOfDay / 1'000'000;
    const std::int64_t frac = usOfDay % 1'000'000;

    std::string out;
    out.reserve(32);
    appendPadded(out, c.y, 4);
    out += '-';
    appendPadded(out, c.m, 2);
    out += '-';
    appendPadded(out, c.d, 2);
    out += 'T';
    appendPadded(out, secOfDay / 3600, 2);
    out += ':';
    appendPadded(out, (secOfDay / 60) % 60, 2);
    out += ':';
    appendPadded(out, secOfDay % 60, 2);
    if (frac != 0) {
        out += '.';
        if (frac % 1000 == 0) {
            appendPadded(out, frac / 1000, 3);
        } else {
            appendPadded(out, frac, 6);
        }
    }
    if (tzOffsetMin == 0) {
        out += 'Z';
    } else {
        const std::int64_t off = tzOffsetMin < 0 ? -static_cast<std::int64_t>(tzOffsetMin) : tzOffsetMin;
        out += tzOffsetMin < 0 ? '-' : '+';
        appendPadded(out, off / 60, 2);
        out += ':';
        appendPadded(out, off % 60, 2);
    }
    return out;
}

std::optional<DateTime> DateTime::parseIso(std::string_view s) noexcept {
    std::size_t pos = 0;
    int y = 0;
    int mo = 0;
    int d = 0;
    int h = 0;
    int mi = 0;
    int sec = 0;
    if (!readDigits(s, pos, 4, y) || !expect(s, pos, '-') || !readDigits(s, pos, 2, mo) ||
        !expect(s, pos, '-') || !readDigits(s, pos, 2, d) || !expect(s, pos, 'T') ||
        !readDigits(s, pos, 2, h) || !expect(s, pos, ':') || !readDigits(s, pos, 2, mi) ||
        !expect(s, pos, ':') || !readDigits(s, pos, 2, sec)) {
        return std::nullopt;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > daysInMonth(y, mo) || h > 23 || mi > 59 || sec > 59) {
        return std::nullopt;
    }
    std::int64_t fracUs = 0;
    if (expect(s, pos, '.')) {
        std::size_t digits = 0;
        std::int64_t scale = 100'000;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            if (digits < 6) {
                fracUs += (s[pos] - '0') * scale;
                scale /= 10;
            }
            ++digits;
            ++pos;
        }
        if (digits == 0 || digits > 9) {
            return std::nullopt;
        }
    }
    int offsetMin = 0;
    if (expect(s, pos, 'Z')) {
        offsetMin = 0;
    } else if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
        const bool negative = s[pos] == '-';
        ++pos;
        int oh = 0;
        int om = 0;
        if (!readDigits(s, pos, 2, oh) || !expect(s, pos, ':') || !readDigits(s, pos, 2, om) || oh > 23 ||
            om > 59) {
            return std::nullopt;
        }
        offsetMin = (oh * 60 + om) * (negative ? -1 : 1);
    } else {
        return std::nullopt;
    }
    if (pos != s.size()) {
        return std::nullopt;
    }
    const std::int64_t days = daysFromCivil(y, mo, d);
    const std::int64_t local =
        days * kUsPerDay +
        (static_cast<std::int64_t>(h) * 3600 + static_cast<std::int64_t>(mi) * 60 + sec) * 1'000'000 + fracUs;
    return DateTime{local - static_cast<std::int64_t>(offsetMin) * kUsPerMinute, offsetMin};
}

DateTime DateTime::nowUtc() {
    const auto now = std::chrono::system_clock::now(); // the Unix epoch in C++20
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    return DateTime{static_cast<std::int64_t>(us), 0};
}

// ---- standard detail ids
// --------------------------------------------------------------------------------------

namespace {

using DT = DetailType;
constexpr StandardDetail kStandardDetails[] = {
    // Summary
    {"Event", "Summary", DT::Text, "", false},
    {"Venue", "Summary", DT::Text, "", false},
    {"Venue Category", "Summary", DT::Text, "", false},
    {"Driver", "Summary", DT::Text, "", false},
    {"Team", "Summary", DT::Text, "", false},
    {"Vehicle Id", "Summary", DT::Text, "", false},
    {"Vehicle Desc", "Summary", DT::Text, "", false},
    {"Vehicle Number", "Summary", DT::Text, "", false},
    {"Engine Id", "Summary", DT::Text, "", false},
    {"Session", "Summary", DT::Text, "", false},
    {"Short Comment", "Summary", DT::Text, "", false},
    {"Long Comment", "Summary", DT::Text, "", false},
    {"Venue Length", "Summary", DT::Number, "Distance", false},
    {"Start Lap", "Summary", DT::Number, "", false},
    {"Log Date", "Summary", DT::DateTime, "", false},
    {"Log Time", "Summary", DT::DateTime, "", false},
    // Statistics (derived, circuit)
    {"Fastest Time", "Statistics", DT::Number, "Time", true},
    {"Fastest Lap", "Statistics", DT::Number, "", true},
    {"Total Laps", "Statistics", DT::Number, "", true},
    // File (derived)
    {"File Name", "File", DT::Text, "", true},
    {"File Path", "File", DT::Text, "", true},
    {"File Size", "File", DT::Number, "", true},
    // Weather
    {"Sky", "Weather", DT::Text, "", false},
    {"Weather Comment", "Weather", DT::Text, "", false},
    {"Track Temp", "Weather", DT::Number, "Temperature", false},
    {"Air Temp", "Weather", DT::Number, "Temperature", false},
    {"Baro Pressure", "Weather", DT::Number, "Pressure", false},
    {"Rel Humidity", "Weather", DT::Number, "Ratio", false},
    {"Wind Direction", "Weather", DT::Number, "Angle", false},
    {"Wind Speed", "Weather", DT::Number, "Speed", false},
    // Device
    {"Device Type", "Device", DT::Text, "", false},
    {"Device Version", "Device", DT::Text, "", false},
    {"Device Serial", "Device", DT::Text, "", false},
    {"Device Options", "Device", DT::Text, "", false},
    // Vehicle
    {"Vehicle Type", "Vehicle", DT::Text, "", false},
    {"Vehicle Drive Type", "Vehicle", DT::Text, "", false},
    {"Vehicle Comment", "Vehicle", DT::Text, "", false},
    {"Vehicle Weight", "Vehicle", DT::Number, "Mass", false},
    {"Fuel Tank Capacity", "Vehicle", DT::Number, "Volume", false},
    {"Vehicle Track", "Vehicle", DT::Number, "Distance", false},
    {"Vehicle Wheelbase", "Vehicle", DT::Number, "Distance", false},
    {"Diff Ratio", "Vehicle", DT::Number, "Ratio", false},
    {"Gear 1", "Vehicle", DT::Number, "Ratio", false},
    {"Gear 2", "Vehicle", DT::Number, "Ratio", false},
    {"Gear 3", "Vehicle", DT::Number, "Ratio", false},
    {"Gear 4", "Vehicle", DT::Number, "Ratio", false},
    {"Gear 5", "Vehicle", DT::Number, "Ratio", false},
    {"Gear 6", "Vehicle", DT::Number, "Ratio", false},
    {"Gear 7", "Vehicle", DT::Number, "Ratio", false},
    {"Gear 8", "Vehicle", DT::Number, "Ratio", false},
    {"Gear 9", "Vehicle", DT::Number, "Ratio", false},
    {"Gear 10", "Vehicle", DT::Number, "Ratio", false},
    // Drag run (profile drag, v1.0)
    {"Run Name", "Drag", DT::Text, "", false},
    {"Lane", "Drag", DT::Text, "", false},
    {"Dial in Time", "Drag", DT::Number, "Time", false},
    {"Reaction Time", "Drag", DT::Number, "Time", false},
    {"Time 60 ft", "Drag", DT::Number, "Time", false},
    {"Time 330 ft", "Drag", DT::Number, "Time", false},
    {"Time 660 ft", "Drag", DT::Number, "Time", false},
    {"Time 1000 ft", "Drag", DT::Number, "Time", false},
    {"Time 1320 ft", "Drag", DT::Number, "Time", false},
    {"Speed 660 ft", "Drag", DT::Number, "Speed", false},
    {"Speed 1320 ft", "Drag", DT::Number, "Speed", false},
    // Esses addition (D-068)
    {"Stoich Ratio", "Esses", DT::Number, "Ratio", false},
};

template <class Vec>
auto findDetailImpl(Vec& details, std::string_view id) noexcept -> decltype(&details[0]) {
    for (auto& d : details) {
        if (d.id == id) {
            return &d;
        }
    }
    const std::string folded = caseFold(id);
    for (auto& d : details) {
        if (caseFold(d.id) == folded) {
            return &d;
        }
    }
    return nullptr;
}

} // namespace

std::span<const StandardDetail> standardDetails() noexcept {
    return kStandardDetails;
}

const StandardDetail* findStandardDetail(std::string_view id) noexcept {
    for (const StandardDetail& s : kStandardDetails) {
        if (s.id == id) {
            return &s;
        }
    }
    for (const StandardDetail& s : kStandardDetails) {
        if (ciEquals(s.id, id)) {
            return &s;
        }
    }
    return nullptr;
}

const Detail* findDetail(const std::vector<Detail>& details, std::string_view id) noexcept {
    return findDetailImpl(details, id);
}

Detail* findDetail(std::vector<Detail>& details, std::string_view id) noexcept {
    return findDetailImpl(details, id);
}

std::string detailValueText(const DetailValue& value) {
    if (const auto* s = std::get_if<std::string>(&value)) {
        return *s;
    }
    if (const auto* q = std::get_if<Quantityd>(&value)) {
        std::array<char, 64> buf{};
        const int dps = q->dps < 0 ? 0 : (q->dps > 15 ? 15 : q->dps);
        const auto r =
            std::to_chars(buf.data(), buf.data() + buf.size(), q->v, std::chars_format::fixed, dps);
        std::string out(buf.data(), r.ec == std::errc{} ? r.ptr : buf.data());
        if (!q->symbol.empty()) {
            out += ' ';
            out += q->symbol;
        }
        return out;
    }
    return std::get<DateTime>(value).iso();
}

std::vector<Detail> overlayDetails(std::vector<Detail> lower, const std::vector<Detail>& upper) {
    for (const Detail& u : upper) {
        if (Detail* l = findDetail(lower, u.id)) {
            DetailValue replaced = std::move(l->value);
            l->value = u.value;
            l->prov = u.prov;
            l->original = std::move(replaced);
        } else {
            lower.push_back(u);
        }
    }
    return lower;
}

std::vector<Detail> mergeDetails(std::vector<Detail> file, std::vector<Detail> companion,
                                 std::vector<Detail> sidecar) {
    for (Detail& d : companion) {
        d.prov = Provenance::Companion;
    }
    for (Detail& d : sidecar) {
        d.prov = Provenance::User;
    }
    return overlayDetails(overlayDetails(std::move(file), companion), sidecar);
}

// ---- numeric values
// -------------------------------------------------------------------------------------------

Result<void> resolveUnit(Quantityd& q, const units::UnitRegistry& registry) {
    const auto id = registry.find(q.symbol);
    if (!id) {
        ErrorContext ctx;
        ctx.detail = q.symbol;
        return fail(ErrorCode::NotFound, "unit '" + q.symbol + "' is not in the unit registry",
                    std::move(ctx));
    }
    q.unit = *id;
    return ok();
}

std::vector<ModelNote> resolveDetailUnits(std::vector<Detail>& details, const units::UnitRegistry& registry) {
    std::vector<ModelNote> notes;
    for (Detail& d : details) {
        bool failed = false;
        std::string symbol;
        const auto resolveOne = [&](DetailValue& v) {
            if (auto* q = std::get_if<Quantityd>(&v)) {
                if (!resolveUnit(*q, registry)) {
                    failed = true;
                    symbol = q->symbol;
                }
            }
        };
        resolveOne(d.value);
        if (d.original) {
            resolveOne(*d.original);
        }
        if (failed) {
            notes.push_back(ModelNote{ModelNote::Level::Warning, std::string(kNoteDetailUnitUnknown),
                                      "detail '" + d.id + "': unit '" + symbol + "' does not resolve", "",
                                      d.id});
        }
    }
    return notes;
}

std::optional<double> toBase(const Quantityd& q, const units::UnitRegistry& registry) {
    units::UnitId unit = q.unit;
    if (unit == units::kNoUnit && !q.symbol.empty()) {
        const auto id = registry.find(q.symbol);
        if (!id) {
            return std::nullopt;
        }
        unit = *id;
    }
    return registry.toBase(unit)(q.v);
}

Quantityd makeQuantity(double v, units::UnitId unit, const units::UnitRegistry& registry, int dps) {
    Quantityd q;
    q.v = v;
    q.unit = registry.contains(unit) ? unit : units::kNoUnit;
    q.dps = dps;
    q.symbol = registry.unit(q.unit).symbol;
    return q;
}

} // namespace esses::model
