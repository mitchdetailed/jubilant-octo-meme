// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Private helpers of the report engines (04 sections 12-13). Owner: the reports work (src/reports/).

#include "report_common.hpp"

#include <esses/model/detail.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <numeric>
#include <string>
#include <utility>
#include <variant>

namespace esses::analysis::reports_detail {

namespace {

constexpr std::int64_t kSecondsPerDay = 86'400;

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil, public domain).
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146'097 + static_cast<std::int64_t>(doe) - 719'468;
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b) noexcept {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Local seconds since the epoch of a date-time (its wall clock as written).
std::int64_t localSeconds(const model::DateTime& dt) noexcept {
    return floorDiv(dt.unixUs, 1'000'000) + static_cast<std::int64_t>(dt.tzOffsetMin) * 60;
}

// Reads the unsigned decimal at text[pos...], advancing pos; nullopt when there is no digit.
std::optional<unsigned> readNumber(std::string_view text, std::size_t& pos) noexcept {
    unsigned v = 0;
    const char* first = text.data() + pos;
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc() || ptr == first) {
        return std::nullopt;
    }
    pos += static_cast<std::size_t>(ptr - first);
    return v;
}

bool expect(std::string_view text, std::size_t& pos, char c) noexcept {
    if (pos < text.size() && text[pos] == c) {
        ++pos;
        return true;
    }
    return false;
}

// A real calendar date: the day within its month's length (29 February in leap years only).
bool validDate(std::int64_t y, unsigned m, unsigned d) noexcept {
    if (y < 1900 || y > 9999 || m < 1 || m > 12 || d < 1) {
        return false;
    }
    constexpr std::array<unsigned, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return d <= kDays[m - 1] + (m == 2 && leap ? 1U : 0U);
}

// Days since the epoch of "a/b/yyyy" or "yyyy-mm-dd" (leading/trailing text after the date is ignored). The
// loggers write "dd/mm/yyyy"; a text that cannot be that but is a valid "mm/dd/yyyy" (b > 12, a <= 12, as a
// US-style importer writes it) reads that way, and an impossible date (31/02) is no date (D-197 (b)).
std::optional<std::int64_t> dateTextDays(std::string_view text) noexcept {
    std::size_t pos = 0;
    const std::optional<unsigned> a = readNumber(text, pos);
    if (!a) {
        return std::nullopt;
    }
    if (expect(text, pos, '/')) {
        const std::optional<unsigned> m = readNumber(text, pos);
        if (!m || !expect(text, pos, '/')) {
            return std::nullopt;
        }
        const std::optional<unsigned> y = readNumber(text, pos);
        if (!y) {
            return std::nullopt;
        }
        if (validDate(*y, *m, *a)) {
            return daysFromCivil(*y, *m, *a);
        }
        if (*m > 12 && validDate(*y, *a, *m)) {
            return daysFromCivil(*y, *a, *m);
        }
        return std::nullopt;
    }
    if (expect(text, pos, '-')) {
        const std::optional<unsigned> m = readNumber(text, pos);
        if (!m || !expect(text, pos, '-')) {
            return std::nullopt;
        }
        const std::optional<unsigned> d = readNumber(text, pos);
        if (!d || !validDate(*a, *m, *d)) {
            return std::nullopt;
        }
        return daysFromCivil(*a, *m, *d);
    }
    return std::nullopt;
}

// Seconds of the day of "hh:mm" or "hh:mm:ss[.fff]".
std::optional<std::int64_t> timeTextSeconds(std::string_view text) noexcept {
    std::size_t pos = 0;
    const std::optional<unsigned> h = readNumber(text, pos);
    if (!h || !expect(text, pos, ':')) {
        return std::nullopt;
    }
    const std::optional<unsigned> m = readNumber(text, pos);
    if (!m) {
        return std::nullopt;
    }
    unsigned s = 0;
    if (expect(text, pos, ':')) {
        const std::optional<unsigned> sec = readNumber(text, pos);
        if (!sec) {
            return std::nullopt;
        }
        s = *sec;
    }
    if (*h > 23 || *m > 59 || s > 60) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*h) * 3600 + static_cast<std::int64_t>(*m) * 60 + s;
}

int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

ResolvedRange resolveRange(const ReportSource& source) {
    ResolvedRange out;
    if (!source.source) {
        return out;
    }
    const std::string_view layerName = source.range.layer.empty() ? source.layer : source.range.layer;
    const model::DataLayer* layer = source.source->layer(layerName);
    if (layer == nullptr) {
        return out;
    }
    const model::RangeGroup* group = layer->ranges.find(source.range.groupPath);
    if (group == nullptr) {
        return out;
    }
    const model::Range* byName = nullptr;
    for (const model::Range& r : group->ranges) {
        if (r.id == source.range.range) {
            out.layer = layer;
            out.range = &r;
            return out;
        }
        if (byName == nullptr && !source.range.name.empty() && r.name == source.range.name) {
            byName = &r;
        }
    }
    if (byName != nullptr) {
        out.layer = layer;
        out.range = byName;
    }
    return out;
}

std::optional<std::int64_t> chronologicalKey(const model::DataSource& source) {
    const model::Detail* date = model::findDetail(source.details, "Log Date");
    if (date == nullptr) {
        return std::nullopt;
    }
    std::optional<std::int64_t> days;
    std::optional<std::int64_t> seconds;
    if (const auto* dt = std::get_if<model::DateTime>(&date->value)) {
        const std::int64_t local = localSeconds(*dt);
        days = floorDiv(local, kSecondsPerDay);
        seconds = local - *days * kSecondsPerDay;
    } else if (const auto* text = std::get_if<std::string>(&date->value)) {
        days = dateTextDays(*text);
    }
    if (!days) {
        return std::nullopt;
    }
    if (const model::Detail* time = model::findDetail(source.details, "Log Time")) {
        if (const auto* dt = std::get_if<model::DateTime>(&time->value)) {
            const std::int64_t local = localSeconds(*dt);
            seconds = local - floorDiv(local, kSecondsPerDay) * kSecondsPerDay;
        } else if (const auto* text = std::get_if<std::string>(&time->value)) {
            if (const std::optional<std::int64_t> s = timeTextSeconds(*text)) {
                seconds = s;
            }
        }
    }
    return *days * kSecondsPerDay + seconds.value_or(0);
}

std::vector<std::size_t> chronologicalRanks(const std::vector<ReportSource>& sources) {
    // Distinct sources in order of first appearance.
    std::vector<const model::DataSource*> distinct;
    std::vector<std::size_t> slot(sources.size(), 0);
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const model::DataSource* s = sources[i].source.get();
        const auto it = std::find(distinct.begin(), distinct.end(), s);
        slot[i] = static_cast<std::size_t>(it - distinct.begin());
        if (it == distinct.end()) {
            distinct.push_back(s);
        }
    }
    std::vector<std::optional<std::int64_t>> keys(distinct.size());
    for (std::size_t d = 0; d < distinct.size(); ++d) {
        if (distinct[d] != nullptr) {
            keys[d] = chronologicalKey(*distinct[d]);
        }
    }
    std::vector<std::size_t> order(distinct.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&keys](std::size_t a, std::size_t b) {
        const std::optional<std::int64_t>& ka = keys[a];
        const std::optional<std::int64_t>& kb = keys[b];
        if (ka.has_value() != kb.has_value()) {
            return ka.has_value();
        }
        return ka.has_value() && kb.has_value() && *ka < *kb;
    });
    std::vector<std::size_t> rankOf(distinct.size(), 0);
    for (std::size_t r = 0; r < order.size(); ++r) {
        rankOf[order[r]] = r;
    }
    std::vector<std::size_t> ranks(sources.size(), 0);
    for (std::size_t i = 0; i < sources.size(); ++i) {
        ranks[i] = rankOf[slot[i]];
    }
    return ranks;
}

std::optional<std::uint32_t> parseColour(std::string_view text) noexcept {
    if ((text.size() != 7 && text.size() != 9) || text.front() != '#') {
        return std::nullopt;
    }
    std::uint32_t v = 0;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const int h = hexDigit(text[i]);
        if (h < 0) {
            return std::nullopt;
        }
        v = (v << 4) | static_cast<std::uint32_t>(h);
    }
    return text.size() == 7 ? (v << 8) | 0xFFu : v;
}

} // namespace esses::analysis::reports_detail
