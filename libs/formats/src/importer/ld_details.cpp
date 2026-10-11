// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// .ld encodings, the raw-header blob and the details of the header and the detail records (05 sections 3.2,
// 3.3) in both directions.
#include "importer/ld_support.hpp"
#include "ld/ld_codes.hpp"

#include <esses/units/unit_registry.hpp>

#include <fast_float/fast_float.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

namespace esses::formats::ldimpl {
namespace {

namespace L = ld::layout;
using model::Detail;
using model::Provenance;
using model::Quantityd;

constexpr std::array<std::byte, 4> kBlobTag = {std::byte{'E'}, std::byte{'S'}, std::byte{'R'},
                                               std::byte{'1'}};

void putU32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    }
}

std::uint32_t getU32(std::span<const std::byte> b, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[at + static_cast<std::size_t>(i)]))
             << (8 * i);
    }
    return v;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

// A decimal number written as text ("23.5", "-4", "1013"), with the number of decimals it shows.
std::optional<std::pair<double, int>> parseDecimal(std::string_view text) {
    text = trim(text);
    if (text.empty()) {
        return std::nullopt;
    }
    double v = 0.0;
    // fast_float, not std::from_chars: Apple Clang 15's libc++ has no floating-point from_chars.
    const auto r =
        fast_float::from_chars(text.data(), text.data() + text.size(), v, fast_float::chars_format::fixed);
    if (r.ec != std::errc() || r.ptr != text.data() + text.size() || !std::isfinite(v)) {
        return std::nullopt;
    }
    const std::size_t dot = text.find('.');
    const int dps = dot == std::string_view::npos ? 0 : static_cast<int>(text.size() - dot - 1);
    return std::make_pair(v, std::min(dps, 15));
}

class DetailList {
public:
    void text(std::string id, std::string_view value) {
        if (!trim(value).empty()) {
            out.push_back(Detail{std::move(id), std::string(value), Provenance::Logged, std::nullopt});
        }
    }
    void number(std::string id, double v, std::string symbol, int dps) {
        if (v != 0.0) {
            Quantityd q;
            q.v = v;
            q.symbol = std::move(symbol);
            q.dps = dps;
            out.push_back(Detail{std::move(id), std::move(q), Provenance::Logged, std::nullopt});
        }
    }
    void weather(std::string id, const ld::LdWeatherValue& w) {
        const auto parsed = parseDecimal(w.value);
        const std::string unit(trim(w.unit));
        if (parsed) {
            Quantityd q;
            q.v = parsed->first;
            q.dps = parsed->second;
            q.symbol = unit;
            out.push_back(Detail{std::move(id), std::move(q), Provenance::Logged, std::nullopt});
            return;
        }
        const std::string value(trim(w.value));
        text(std::move(id), unit.empty() || value.empty() ? value : value + " " + unit);
    }
    std::vector<Detail> out;
};

std::string twoDigits(int v) {
    std::string s = std::to_string(v);
    return s.size() < 2 ? "0" + s : s;
}

struct Civil {
    int y, m, d, hh, mm, ss;
};
Civil civilOf(const model::DateTime& dt) {
    using namespace std::chrono;
    const std::int64_t localUs = dt.unixUs + std::int64_t{dt.tzOffsetMin} * 60'000'000;
    const auto secs = static_cast<std::int64_t>(std::floor(static_cast<double>(localUs) / 1e6));
    const sys_seconds tp{seconds{secs}};
    const sys_days day = floor<days>(tp);
    const year_month_day ymd{day};
    const auto rest = static_cast<int>((tp - day).count());
    return Civil{static_cast<int>(ymd.year()),
                 static_cast<int>(static_cast<unsigned>(ymd.month())),
                 static_cast<int>(static_cast<unsigned>(ymd.day())),
                 rest / 3600,
                 (rest / 60) % 60,
                 rest % 60};
}

// The value of a numeric detail in `baseSymbol` (the base unit of its quantity), or nullopt.
std::optional<double> inBase(const model::DetailValue& value, std::string_view baseSymbol,
                             const units::UnitRegistry* units) {
    const auto* q = std::get_if<Quantityd>(&value);
    if (q == nullptr) {
        if (const auto* s = std::get_if<std::string>(&value)) {
            if (const auto parsed = parseDecimal(*s)) {
                return parsed->first; // a plain number written as text: taken as the base unit
            }
        }
        return std::nullopt;
    }
    if (q->symbol == baseSymbol) {
        return q->v;
    }
    if (units == nullptr) {
        return std::nullopt;
    }
    std::optional<units::UnitId> unit;
    if (q->unit != units::kNoUnit && units->contains(q->unit)) {
        unit = q->unit;
    } else {
        unit = units->find(q->symbol);
    }
    if (!unit) {
        return std::nullopt;
    }
    const units::Quantity& qty = units->quantity(units->quantityOf(*unit));
    if (units->unit(qty.base).symbol != baseSymbol) {
        return std::nullopt;
    }
    return units->toBase(*unit)(q->v);
}

template <class T> std::optional<T> fieldValue(double v) {
    const double r = std::round(v);
    if (!std::isfinite(r) || r < 0.0 || r > static_cast<double>(std::numeric_limits<T>::max())) {
        return std::nullopt;
    }
    return static_cast<T>(r);
}

} // namespace

LdEncoding encodingOf(const ld::LdChannel& c) {
    LdEncoding e;
    e.typeCode = c.enc.typeCode;
    e.width = c.enc.bytesPerSample;
    e.rateHz = c.enc.rateHz;
    e.shift = c.enc.shift;
    e.mul = c.enc.mul;
    e.scale = c.enc.scale;
    e.dec = c.enc.dec;
    e.dpsByte = c.enc.dpsByte;
    e.interpByte = c.enc.interpByte;
    e.counter = c.enc.counter;
    e.name = c.name32;
    e.longName = c.longName;
    e.unit = c.unit;
    e.shortName = c.shortName;
    e.enumRef = c.enumRef;
    e.displayRange = c.displayRange;
    e.rawRecord = c.enc.raw;
    return e;
}

std::vector<std::byte> packRawHeader(const ld::LdFile& file) {
    std::vector<std::byte> out(file.rawHeader().begin(), file.rawHeader().end());
    if (out.size() != L::kHeaderSize) {
        return {};
    }
    out.insert(out.end(), kBlobTag.begin(), kBlobTag.end());
    for (const std::span<const std::byte> rec :
         {file.rawEventRecord(), file.rawVenueRecord(), file.rawVehicleRecord(), file.rawWeatherRecord()}) {
        putU32(out, static_cast<std::uint32_t>(rec.size()));
        out.insert(out.end(), rec.begin(), rec.end());
    }
    return out;
}

std::optional<RawHeaderParts> unpackRawHeader(std::span<const std::byte> blob) {
    if (blob.size() < L::kHeaderSize + kBlobTag.size() ||
        !std::equal(kBlobTag.begin(), kBlobTag.end(), blob.begin() + L::kHeaderSize)) {
        return std::nullopt;
    }
    RawHeaderParts parts;
    parts.header.assign(blob.begin(), blob.begin() + L::kHeaderSize);
    std::size_t at = L::kHeaderSize + kBlobTag.size();
    for (std::vector<std::byte>* rec : {&parts.event, &parts.venue, &parts.vehicle, &parts.weather}) {
        if (at + 4 > blob.size()) {
            return std::nullopt;
        }
        const std::uint32_t n = getU32(blob, at);
        at += 4;
        if (n > blob.size() - at) {
            return std::nullopt;
        }
        rec->assign(blob.begin() + static_cast<std::ptrdiff_t>(at),
                    blob.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    }
    return parts;
}

std::optional<UtcInstant> utcOf(const ld::LdHeader& header) {
    if (!header.dateTime) {
        return std::nullopt;
    }
    using namespace std::chrono;
    const ld::LocalDateTime& t = *header.dateTime;
    const year_month_day ymd{year{t.year}, month{static_cast<unsigned>(t.month)},
                             day{static_cast<unsigned>(t.day)}};
    if (!ymd.ok() || t.hour < 0 || t.hour > 23 || t.minute < 0 || t.minute > 59 || t.second < 0 ||
        t.second > 59) {
        return std::nullopt;
    }
    const std::int64_t daysSinceEpoch = sys_days{ymd}.time_since_epoch().count();
    const std::int64_t secs =
        daysSinceEpoch * 86'400 + std::int64_t{t.hour} * 3'600 + std::int64_t{t.minute} * 60 + t.second;
    return UtcInstant{secs * 1'000'000};
}

std::vector<model::Detail> detailsOf(const ld::LdFile& file) {
    const ld::LdHeader& h = file.header();
    const auto& event = file.eventRecord();
    const auto& venue = file.venueRecord();
    const auto& vehicle = file.vehicleRecord();
    const auto& weather = file.weatherRecord();
    DetailList d;
    d.text("Device Type", h.deviceType);
    if (h.serial != 0) {
        d.text("Device Serial", std::to_string(h.serial));
    }
    if (h.deviceVersion != 0) {
        d.text("Device Version", std::to_string(h.deviceVersion));
    }
    d.text("Log Date", h.date);
    d.text("Log Time", h.time);
    d.text("Driver", h.driver);
    // Header fields first; a record's copy fills an empty header field (Venue, Vehicle Id) or wins when it is
    // not empty (Session from the event record, Engine Id from the vehicle record), 05 sections 3.2 and 3.3.
    d.text("Vehicle Id", !h.vehicleId.empty() || !vehicle ? h.vehicleId : vehicle->id);
    d.text("Engine Id", vehicle && !vehicle->engineId.empty() ? vehicle->engineId : h.engineId);
    d.text("Venue", !h.venue.empty() || !venue ? h.venue : venue->name);
    d.text("Session", event && !event->session.empty() ? event->session : h.session);
    d.text("Short Comment", h.shortComment);
    d.text("Team", h.team);
    if (event) {
        d.text("Event", event->name);
        d.text("Long Comment", event->longComment);
    }
    if (venue) {
        d.number("Venue Length", venue->lengthMm / 1000.0, "m", venue->lengthMm % 1000 == 0 ? 0 : 3);
        d.text("Venue Category", venue->category);
    }
    if (vehicle) {
        d.text("Vehicle Desc", vehicle->desc);
        d.number("Vehicle Weight", vehicle->weightKg, "kg", 0);
        d.number("Fuel Tank Capacity", vehicle->fuelTankX10 / 10000.0, "m3", 4); // litres x 10 -> m3
        d.text("Vehicle Type", vehicle->type);
        d.text("Vehicle Drive Type", vehicle->driveType);
        d.number("Diff Ratio", vehicle->diffX1000 / 1000.0, "ratio", 3);
        for (std::uint32_t g = 0; g < L::kGearCount; ++g) {
            d.number("Gear " + std::to_string(g + 1), vehicle->gearsX1000[g] / 1000.0, "ratio", 3);
        }
        d.number("Vehicle Track", vehicle->trackMm / 1000.0, "m", 3);
        d.number("Vehicle Wheelbase", vehicle->wheelbaseMm / 1000.0, "m", 3);
        d.text("Vehicle Comment", vehicle->comment);
        d.text("Vehicle Number", vehicle->number);
    }
    if (weather) {
        d.text("Sky", weather->sky);
        d.weather("Air Temp", weather->airTemp);
        d.weather("Track Temp", weather->trackTemp);
        d.weather("Baro Pressure", weather->baroPressure);
        d.weather("Rel Humidity", weather->relHumidity);
        d.weather("Wind Speed", weather->windSpeed);
        d.text("Wind Direction", weather->windDirection);
        d.text("Weather Comment", weather->comment);
    }
    return std::move(d.out);
}

ld::LdWriteDetails writeDetailsOf(const std::vector<model::Detail>& details, const units::UnitRegistry* units,
                                  ImportLog& notes) {
    ld::LdWriteDetails w;
    const auto skipped = [&](const Detail& d, const std::string& why) {
        ImportLogEntry::Context ctx;
        ctx.detail = d.id;
        notes.add(ImportLogEntry::Level::Info, std::string(ld::code::kDetailSkipped),
                  "detail '" + d.id + "' not written to the .ld header: " + why, std::move(ctx));
    };
    const auto textOf = [](const model::DetailValue& v) { return model::detailValueText(v); };
    for (const Detail& d : details) {
        if (d.prov == Provenance::Derived) {
            continue; // the File group and statistics are computed, never written
        }
        const std::string& id = d.id;
        const auto number = [&](std::string_view base, double factor, auto& field) {
            using T = std::remove_reference_t<decltype(field)>;
            const auto v = inBase(d.value, base, units);
            if (!v) {
                skipped(d, "the value is not a number in a unit of the field's quantity");
                return;
            }
            const auto f = fieldValue<T>(*v * factor);
            if (!f) {
                skipped(d, "the value does not fit the field");
                return;
            }
            field = *f;
        };
        const auto weatherValue = [&](ld::LdWeatherValue& field) {
            if (const auto* q = std::get_if<Quantityd>(&d.value)) {
                char buf[64];
                const auto r = std::to_chars(buf, buf + sizeof(buf), q->v, std::chars_format::fixed,
                                             std::clamp(q->dps, 0, 15));
                field.value = r.ec == std::errc() ? std::string(buf, r.ptr) : textOf(d.value);
                field.unit = q->symbol;
            } else {
                field.value = textOf(d.value);
            }
        };
        if (id == "Log Date") {
            if (const auto* dt = std::get_if<model::DateTime>(&d.value)) {
                const Civil c = civilOf(*dt);
                w.date = twoDigits(c.d) + "/" + twoDigits(c.m) + "/" + std::to_string(c.y);
            } else {
                w.date = textOf(d.value);
            }
        } else if (id == "Log Time") {
            if (const auto* dt = std::get_if<model::DateTime>(&d.value)) {
                const Civil c = civilOf(*dt);
                w.time = twoDigits(c.hh) + ":" + twoDigits(c.mm) + ":" + twoDigits(c.ss);
            } else {
                w.time = textOf(d.value);
            }
        } else if (id == "Driver") {
            w.driver = textOf(d.value);
        } else if (id == "Vehicle Id") {
            w.vehicleId = textOf(d.value);
            w.vehicleRecordId = w.vehicleId;
        } else if (id == "Engine Id") {
            w.engineId = textOf(d.value);
            w.vehicleEngineId = w.engineId;
        } else if (id == "Venue") {
            w.venue = textOf(d.value);
            w.venueName = w.venue;
        } else if (id == "Session") {
            w.session = textOf(d.value);
        } else if (id == "Short Comment") {
            w.shortComment = textOf(d.value);
        } else if (id == "Team") {
            w.team = textOf(d.value);
        } else if (id == "Event") {
            w.eventName = textOf(d.value);
        } else if (id == "Long Comment") {
            w.longComment = textOf(d.value);
        } else if (id == "Venue Length") {
            number("m", 1000.0, w.venueLengthMm);
        } else if (id == "Venue Category") {
            w.venueCategory = textOf(d.value);
        } else if (id == "Vehicle Desc") {
            w.vehicleDesc = textOf(d.value);
        } else if (id == "Vehicle Weight") {
            number("kg", 1.0, w.weightKg);
        } else if (id == "Fuel Tank Capacity") {
            number("m3", 10000.0, w.fuelTankX10);
        } else if (id == "Vehicle Type") {
            w.vehicleType = textOf(d.value);
        } else if (id == "Vehicle Drive Type") {
            w.driveType = textOf(d.value);
        } else if (id == "Diff Ratio") {
            number("ratio", 1000.0, w.diffX1000);
        } else if (id.starts_with("Gear ")) {
            int g = 0;
            const auto r = std::from_chars(id.data() + 5, id.data() + id.size(), g);
            if (r.ec == std::errc() && r.ptr == id.data() + id.size() && g >= 1 &&
                g <= static_cast<int>(L::kGearCount)) {
                number("ratio", 1000.0, w.gearsX1000[static_cast<std::size_t>(g - 1)]);
            }
        } else if (id == "Vehicle Track") {
            number("m", 1000.0, w.trackMm);
        } else if (id == "Vehicle Wheelbase") {
            number("m", 1000.0, w.wheelbaseMm);
        } else if (id == "Vehicle Comment") {
            w.vehicleComment = textOf(d.value);
        } else if (id == "Vehicle Number") {
            w.vehicleNumber = textOf(d.value);
        } else if (id == "Sky") {
            w.sky = textOf(d.value);
        } else if (id == "Air Temp") {
            weatherValue(w.airTemp);
        } else if (id == "Track Temp") {
            weatherValue(w.trackTemp);
        } else if (id == "Baro Pressure") {
            weatherValue(w.baroPressure);
        } else if (id == "Rel Humidity") {
            weatherValue(w.relHumidity);
        } else if (id == "Wind Speed") {
            weatherValue(w.windSpeed);
        } else if (id == "Wind Direction") {
            w.windDirection = textOf(d.value);
        } else if (id == "Weather Comment") {
            w.weatherComment = textOf(d.value);
        }
        // Device Type/Serial/Version are writer options; other ids have no .ld field (the .ldx keeps them).
    }
    return w;
}

} // namespace esses::formats::ldimpl
