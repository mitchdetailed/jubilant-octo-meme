// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// New code written from docs/formats/ld-converter-port.md (sections 1, 2 and 3) and 05 section 5 (D-144).
#include "ld/ld_writer.hpp"

#include "formats_build_config.hpp"
#include "ld/ld_codes.hpp"
#include "ld/ld_strings.hpp"

#include <esses/base/atomic_file.hpp>
#include <esses/base/byte_reader.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>

namespace esses::formats::ld {
namespace L = layout;

std::string_view writeDialectName(WriteDialect d) noexcept {
    switch (d) {
    case WriteDialect::PcLayout:
        return "pc-layout";
    case WriteDialect::M1:
        return "m1";
    case WriteDialect::CSeries:
        return "c-series";
    case WriteDialect::SimExporter:
        return "sim-exporter";
    case WriteDialect::ConverterLegacy:
        return "converter-legacy";
    }
    return "pc-layout";
}

namespace {

constexpr std::uint32_t kLegacyAuxContentOffset = 62; // legacy aux block: content 62 bytes after its start

class Image {
public:
    explicit Image(std::size_t size) : b_(size) {}
    template <class T> void put(std::uint64_t off, T v) {
        static_assert(std::is_arithmetic_v<T>);
        using U = std::conditional_t<
            sizeof(T) == 1, std::uint8_t,
            std::conditional_t<sizeof(T) == 2, std::uint16_t,
                               std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
        const U raw = std::bit_cast<U>(v);
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            b_[static_cast<std::size_t>(off) + i] = static_cast<std::byte>((raw >> (8 * i)) & 0xFFu);
        }
    }
    void bytes(std::uint64_t off, std::span<const std::byte> s) {
        std::copy(s.begin(), s.end(), b_.begin() + static_cast<std::ptrdiff_t>(off));
    }
    void zero(std::uint64_t off, std::uint64_t len) {
        std::fill_n(b_.begin() + static_cast<std::ptrdiff_t>(off), static_cast<std::size_t>(len),
                    std::byte{0});
    }
    std::vector<std::byte> take() { return std::move(b_); }

private:
    std::vector<std::byte> b_;
};

class Notes {
public:
    explicit Notes(ImportLog* log) : log_(log) {}
    void add(std::string_view c, std::string message, std::optional<std::string> channel = std::nullopt) {
        if (log_ == nullptr) {
            return;
        }
        const CodeInfo* info = findCode(c);
        ImportLogEntry::Context ctx;
        ctx.channel = std::move(channel);
        log_->add(info != nullptr ? info->level : ImportLogEntry::Level::Info, std::string(c),
                  std::move(message), std::move(ctx));
    }

private:
    ImportLog* log_;
};

// Writes `s` into a zeroed field of `width` bytes holding at most `maxBytes` (width: full field allowed).
void putText(Image& img, std::uint64_t off, std::uint32_t width, std::uint32_t maxBytes, std::string_view s,
             Notes& notes, std::string_view what) {
    img.zero(off, width);
    if (s.empty()) {
        return;
    }
    const EncodedText t = encodeText(s, maxBytes);
    if (t.truncated) {
        notes.add(code::kTruncated, std::string(what) + " cut to " + std::to_string(maxBytes) + " bytes");
    }
    if (t.utf8) {
        notes.add(code::kStringEncoding, std::string(what) + " written as UTF-8");
    }
    img.bytes(off, t.bytes);
}
void putText64(Image& img, std::uint64_t off, std::string_view s, Notes& n, std::string_view what) {
    putText(img, off, L::kText64, L::kText64 - 1, s, n, what);
}

bool isFloatPair(std::uint16_t code, std::uint16_t width) {
    return (code == L::kTypeFloat && (width == 2 || width == 4 || width == 8)) ||
           (code == L::kTypeDouble && width == 8);
}

std::uint64_t alignUp(std::uint64_t v, std::uint32_t a) {
    if (a <= 1) {
        return v;
    }
    return (v + a - 1) / a * a;
}

// The stored value of sample i as double (for raw max/min), from whichever sample vector is in use.
double storedValue(const LdWriteChannel& c, std::size_t i, bool isFloat) {
    if (!c.rawData.empty()) {
        const std::byte* p = c.rawData.data() + i * c.width;
        if (isFloat) {
            switch (c.width) {
            case 2:
                return halfToDouble(loadAs<std::uint16_t>(p, Endian::Little));
            case 4:
                return static_cast<double>(loadAs<float>(p, Endian::Little));
            default:
                return loadAs<double>(p, Endian::Little);
            }
        }
        const bool u = c.typeCode == L::kTypeUnsigned4;
        switch (c.width) {
        case 1:
            return u ? static_cast<double>(loadAs<std::uint8_t>(p, Endian::Little))
                     : static_cast<double>(loadAs<std::int8_t>(p, Endian::Little));
        case 2:
            return u ? static_cast<double>(loadAs<std::uint16_t>(p, Endian::Little))
                     : static_cast<double>(loadAs<std::int16_t>(p, Endian::Little));
        case 4:
            return u ? static_cast<double>(loadAs<std::uint32_t>(p, Endian::Little))
                     : static_cast<double>(loadAs<std::int32_t>(p, Endian::Little));
        default:
            return static_cast<double>(loadAs<std::int64_t>(p, Endian::Little));
        }
    }
    return isFloat ? c.rawFloats[i] : static_cast<double>(c.rawInts[i]);
}

std::int32_t toI32Sat(double v) {
    if (!(v == v)) {
        return 0;
    }
    if (v >= 2147483647.0) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (v <= -2147483648.0) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(std::llround(v));
}

struct ChannelPlan {
    std::uint64_t record = 0;
    std::uint64_t recordSize = L::kRecordSize;
    std::uint64_t gap = 0;
    std::uint32_t gapEvents = 0;
    std::uint64_t aux = 0;
    std::uint64_t auxSize = 0;
    std::vector<std::byte> longName; // encoded, without NUL
    bool hasLongName = false;
    std::uint64_t data = 0;
    std::uint64_t count = 0;
    bool isFloat = false;
};

struct DialectDefaults {
    std::string deviceType;
    std::uint32_t serial = 1;
    std::uint16_t version = 0;
    std::uint16_t u040 = 2;
    std::uint16_t u054 = 0x0080;
    bool countCopy = true;
    std::uint16_t u05A = 1;
    std::uint16_t u05C = 1;
};

DialectDefaults defaultsFor(WriteDialect d) {
    DialectDefaults x;
    x.deviceType = ESSES_FORMATS_LD_DEVICE_TYPE;
    x.version = static_cast<std::uint16_t>(ESSES_FORMATS_LD_DEVICE_VERSION);
    switch (d) {
    case WriteDialect::PcLayout:
    case WriteDialect::ConverterLegacy:
        break;
    case WriteDialect::M1:
        x.deviceType = "M1";
        x.version = 100;
        x.u05A = 10;
        break;
    case WriteDialect::CSeries:
        x.deviceType = "C125";
        x.version = 650;
        x.u040 = 0;
        x.u054 = 0;
        x.countCopy = false;
        break;
    case WriteDialect::SimExporter:
        x.deviceType = "ADL";
        x.serial = 0x1F44;
        x.version = 420;
        x.u054 = 0xADB0;
        break;
    }
    return x;
}

std::uint64_t enumBlockSize(const std::vector<LdEnumTable>& tables,
                            std::vector<std::vector<std::byte>>& labels) {
    std::uint64_t size = 8;
    for (const LdEnumTable& t : tables) {
        size += 10;
        for (const LdEnumEntry& e : t.entries) {
            EncodedText enc = encodeText(e.label, 65535u - 3u);
            size += 8u + enc.bytes.size() + 3u;
            if (e.extended || e.severity != 0) {
                size += L::kEnumSeverityBlockSize;
            }
            labels.push_back(std::move(enc.bytes));
        }
    }
    return size;
}

} // namespace

Result<std::vector<std::byte>> LdWriter::build(const LdWriteInput& in, const LdWriteOptions& opt,
                                               ImportLog* log) {
    Notes notes(log);
    const DialectDefaults dd = defaultsFor(opt.dialect);
    const bool legacy = opt.dialect == WriteDialect::ConverterLegacy;
    const bool cSeries = opt.dialect == WriteDialect::CSeries;
    const bool terminator = opt.terminator.value_or(opt.dialect == WriteDialect::M1);
    const std::uint32_t configRegion = opt.configRegionBytes.value_or(cSeries ? 0x10000u : 0u);
    if (!opt.rawHeader.empty() && opt.rawHeader.size() != L::kHeaderSize) {
        return fail(ErrorCode::Invalid, "LdWriter: rawHeader must hold 1,762 bytes");
    }
    if (in.channels.size() > 0xFFFE) {
        return fail(ErrorCode::Invalid, "LdWriter: more channels than the u16 header count can hold");
    }

    // ---- pass 1: layout ------------------------------------------------------------------------------
    std::uint64_t cur = L::kHeaderSize + configRegion;
    const std::uint64_t eventOff = cur;
    cur += L::kEventRecordSize;
    std::uint64_t venueOff = 0, vehicleOff = 0, weatherOff = 0;
    if (!legacy) {
        venueOff = cur;
        cur += L::kVenueRecordSize;
        vehicleOff = cur;
        cur += L::kVehicleRecordSize;
        weatherOff = cur;
        cur += L::kWeatherRecordSize;
    }
    std::uint64_t eventsOff = 0;
    if (!in.events.empty()) {
        eventsOff = cur;
        cur += L::kEventBlockHeaderSize + std::uint64_t{L::kEventRecordBytes} * in.events.size();
    }
    std::uint64_t enumOff = 0;
    std::vector<std::vector<std::byte>> enumLabels;
    if (!in.enumTables.empty()) {
        enumOff = cur;
        cur += enumBlockSize(in.enumTables, enumLabels);
    }

    std::vector<ChannelPlan> plans(in.channels.size());
    for (std::size_t i = 0; i < in.channels.size(); ++i) {
        const LdWriteChannel& c = in.channels[i];
        ChannelPlan& p = plans[i];
        if (c.width != 1 && c.width != 2 && c.width != 4 && c.width != 8) {
            return fail(ErrorCode::Invalid, "LdWriter: channel '" + c.name + "' has width " +
                                                std::to_string(c.width) + "; 1, 2, 4 or 8 expected");
        }
        p.isFloat = isFloatPair(c.typeCode, c.width);
        const int used =
            (c.rawData.empty() ? 0 : 1) + (c.rawInts.empty() ? 0 : 1) + (c.rawFloats.empty() ? 0 : 1);
        if (used > 1) {
            return fail(ErrorCode::Invalid,
                        "LdWriter: channel '" + c.name + "' has more than one sample vector");
        }
        if (!c.rawData.empty()) {
            if (c.rawData.size() % c.width != 0) {
                return fail(ErrorCode::Invalid,
                            "LdWriter: channel '" + c.name + "' raw data is not whole samples");
            }
            p.count = c.rawData.size() / c.width;
        } else if (!c.rawInts.empty()) {
            if (p.isFloat) {
                return fail(ErrorCode::Invalid,
                            "LdWriter: channel '" + c.name + "' is a float pair; use rawFloats");
            }
            p.count = c.rawInts.size();
        } else if (!c.rawFloats.empty()) {
            if (!p.isFloat) {
                return fail(ErrorCode::Invalid,
                            "LdWriter: channel '" + c.name + "' is an integer pair; use rawInts");
            }
            p.count = c.rawFloats.size();
        }
        if (p.count > 0xFFFFFFFFull) {
            return fail(ErrorCode::Invalid,
                        "LdWriter: channel '" + c.name + "' has more than 2^32 - 1 samples");
        }
        // Names: the 32-byte field keeps the start; a longer name (or an explicit one) goes to the aux block.
        const EncodedText full = encodeText(c.name, 0xFFFFu);
        std::string longName = c.longName;
        if (longName.empty() && full.bytes.size() > L::kNameWidth) {
            longName = c.name;
        }
        if (!longName.empty()) {
            EncodedText ln = encodeText(longName, 0xFFFEu);
            if (ln.utf8) {
                notes.add(code::kStringEncoding, "long name written as UTF-8", c.name);
            }
            p.longName = std::move(ln.bytes);
            p.hasLongName = true;
        }

        p.record = cur;
        p.recordSize = legacy ? L::kLegacyRecordSize : L::kRecordSize;
        cur += p.recordSize;
        if (legacy) {
            // Legacy records carry their long name in an unreferenced block (port specification Q-W30).
            p.aux = cur;
            p.auxSize = kLegacyAuxContentOffset + 8 + (p.hasLongName ? 8 + p.longName.size() + 1 : 0);
            cur += p.auxSize;
            continue;
        }
        if (c.rawGapEvents) {
            p.gapEvents = static_cast<std::uint32_t>(c.rawGapEvents->size());
        } else {
            p.gapEvents = static_cast<std::uint32_t>(2 * c.invalid.size());
        }
        if (p.gapEvents != 0 || c.rawGapEvents) {
            p.gap = cur;
            cur += L::kGapHeaderSize + std::uint64_t{L::kGapEventSize} * p.gapEvents;
        }
        if (p.hasLongName || c.enumRef) {
            p.aux = cur;
            p.auxSize = L::kAuxContentOffset + 8 + (c.enumRef ? 10 : 0) +
                        (p.hasLongName ? 8 + p.longName.size() + 1 + (c.longNamePadByte ? 1 : 0) : 0);
            cur += p.auxSize;
        }
    }
    std::uint64_t termOff = 0;
    if (terminator) {
        termOff = cur;
        cur += L::kRecordSize;
    }
    std::uint64_t firstData = 0;
    for (std::size_t i = 0; i < plans.size(); ++i) {
        cur = alignUp(cur, opt.dataAlignment);
        plans[i].data = cur;
        if (i == 0) {
            firstData = cur;
        }
        cur += plans[i].count * in.channels[i].width;
    }
    const std::uint64_t total = cur;
    if (total > 0xFFFFFFFFull) {
        return fail(ErrorCode::Invalid,
                    "LdWriter: the file would exceed the 4 GiB a u32 pointer can address");
    }
    const auto u32 = [](std::uint64_t v) { return static_cast<std::uint32_t>(v); };
    const auto records = static_cast<std::uint32_t>(in.channels.size() + (terminator ? 1 : 0));
    const bool headerOnly = records == 0;
    const std::uint64_t metaPtr = headerOnly ? total : (plans.empty() ? termOff : plans.front().record);
    const std::uint64_t dataPtr = headerOnly ? total : (plans.empty() ? total : firstData);

    Image img(static_cast<std::size_t>(total));

    // ---- header ----------------------------------------------------------------------------------------
    const LdWriteDetails& d = in.details;
    if (!opt.rawHeader.empty()) {
        img.bytes(0, opt.rawHeader);
        const bool hadCopy =
            loadAs<std::uint16_t>(opt.rawHeader.data() + L::kHdrChannelCount2, Endian::Little) != 0;
        img.put<std::uint16_t>(L::kHdrChannelCount2, hadCopy ? static_cast<std::uint16_t>(records) : 0);
        img.put<std::uint32_t>(L::kHdrUnknownPtr004, 0);
        img.put<std::uint32_t>(L::kHdrUnknownPtr68C, 0);
    } else {
        img.put<std::uint32_t>(L::kHdrMagic, L::kMagic);
        img.put<std::uint16_t>(L::kHdrUnknown040, dd.u040);
        img.put<std::uint32_t>(L::kHdrTicks, L::kTicksPerSecond);
        img.put<std::uint32_t>(L::kHdrSerial, opt.serial.value_or(dd.serial));
        putText(img, L::kHdrDeviceType, L::kDeviceTypeWidth, L::kDeviceTypeWidth,
                opt.deviceType.value_or(dd.deviceType), notes, "device type");
        img.put<std::uint16_t>(L::kHdrDeviceVersion, opt.deviceVersion.value_or(dd.version));
        img.put<std::uint16_t>(L::kHdrUnknown054, dd.u054);
        img.put<std::uint16_t>(L::kHdrChannelCount2, dd.countCopy ? static_cast<std::uint16_t>(records) : 0);
        img.put<std::uint16_t>(L::kHdrUnknown05A, dd.u05A);
        img.put<std::uint16_t>(L::kHdrUnknown05C, dd.u05C);
        putText(img, L::kHdrDate, L::kDateWriteWidth, L::kDateWriteWidth, d.date, notes, "date");
        putText(img, L::kHdrTime, L::kTimeWriteWidth, L::kTimeWriteWidth, d.time, notes, "time");
        putText64(img, L::kHdrDriver, d.driver, notes, "driver");
        putText64(img, L::kHdrVehicleId, d.vehicleId, notes, "vehicle id");
        putText64(img, L::kHdrEngineId, d.engineId, notes, "engine id");
        putText64(img, L::kHdrVenue, d.venue, notes, "venue");
        putText64(img, L::kHdrSession, d.session, notes, "session");
        putText64(img, L::kHdrShortComment, d.shortComment, notes, "short comment");
        putText(img, L::kHdrTeam, L::kText32, L::kText32 - 1, d.team, notes, "team");
    }
    img.put<std::uint32_t>(L::kHdrMetaPtr, u32(metaPtr));
    img.put<std::uint32_t>(L::kHdrDataPtr, u32(dataPtr));
    img.put<std::uint32_t>(L::kHdrEventPtr, u32(eventOff));
    img.put<std::uint16_t>(L::kHdrChannelCount,
                           opt.headerChannelCount.value_or(static_cast<std::uint16_t>(records)));
    img.put<std::uint32_t>(L::kHdrProLogging, 0); // D-143: never computed, always 0
    img.put<std::uint32_t>(L::kHdrDeviceEventPtr, u32(eventsOff));
    img.put<std::uint32_t>(L::kHdrEnumPtr, u32(enumOff));

    // ---- detail records -----------------------------------------------------------------------------------
    const auto base = [&](std::uint64_t off, const std::vector<std::byte>& raw, std::uint32_t size) {
        if (raw.empty()) {
            return false;
        }
        img.bytes(off, std::span<const std::byte>(raw.data(), std::min<std::size_t>(raw.size(), size)));
        return true;
    };
    if (!base(eventOff, opt.rawEvent, L::kEventRecordSize)) {
        putText64(img, eventOff + L::kEvName, d.eventName, notes, "event name");
        putText64(img, eventOff + L::kEvSession, d.eventSession.empty() ? d.session : d.eventSession, notes,
                  "event session");
        putText(img, eventOff + L::kEvLongComment, L::kLongCommentWidth, L::kLongCommentWidth - 1,
                d.longComment, notes, "long comment");
    }
    img.put<std::uint32_t>(eventOff + L::kEvVenuePtr, u32(venueOff));
    img.put<std::uint32_t>(eventOff + L::kEvWeatherPtr, u32(weatherOff));
    if (!legacy) {
        if (!base(venueOff, opt.rawVenue, L::kVenueRecordSize)) {
            putText64(img, venueOff + L::kVnName, d.venueName, notes, "venue name");
            img.put<std::uint32_t>(venueOff + L::kVnLengthMm, d.venueLengthMm);
            putText(img, venueOff + L::kVnCategory, L::kText32, L::kText32 - 1, d.venueCategory, notes,
                    "venue category");
        }
        img.put<std::uint32_t>(venueOff + L::kVnVehiclePtr, u32(vehicleOff));
        if (!base(vehicleOff, opt.rawVehicle, L::kVehicleRecordSize)) {
            const std::uint64_t v = vehicleOff;
            putText64(img, v + L::kVhId, d.vehicleRecordId, notes, "vehicle id");
            putText64(img, v + L::kVhDesc, d.vehicleDesc, notes, "vehicle description");
            putText64(img, v + L::kVhEngineId, d.vehicleEngineId, notes, "vehicle engine id");
            img.put<std::uint16_t>(v + L::kVhWeightKg, d.weightKg);
            img.put<std::uint16_t>(v + L::kVhFuelTankX10, d.fuelTankX10);
            putText(img, v + L::kVhType, L::kText32, L::kText32 - 1, d.vehicleType, notes, "vehicle type");
            putText(img, v + L::kVhDriveType, L::kText32, L::kText32 - 1, d.driveType, notes, "drive type");
            img.put<std::uint16_t>(v + L::kVhDiffX1000, d.diffX1000);
            for (std::uint32_t g = 0; g < L::kGearCount; ++g) {
                img.put<std::uint16_t>(v + L::kVhGear1X1000 + std::uint64_t{2} * g, d.gearsX1000[g]);
            }
            img.put<std::uint16_t>(v + L::kVhTrackMm, d.trackMm);
            img.put<std::uint16_t>(v + L::kVhWheelbaseMm, d.wheelbaseMm);
            putText(img, v + L::kVhComment, L::kLongCommentWidth, L::kLongCommentWidth - 1, d.vehicleComment,
                    notes, "vehicle comment");
            putText(img, v + L::kVhNumber, L::kText32, L::kText32 - 1, d.vehicleNumber, notes,
                    "vehicle number");
        }
        if (!base(weatherOff, opt.rawWeather, L::kWeatherRecordSize)) {
            const std::uint64_t w = weatherOff;
            putText64(img, w + L::kWxSky, d.sky, notes, "sky");
            const auto pair = [&](std::uint32_t o, const LdWeatherValue& v, std::string_view what) {
                putText(img, w + o, L::kWxValueWidth, L::kWxValueWidth - 1, v.value, notes, what);
                putText(img, w + o + L::kWxUnitOffset, L::kWxUnitWidth, L::kWxUnitWidth - 1, v.unit, notes,
                        what);
            };
            pair(L::kWxAirTemp, d.airTemp, "air temperature");
            pair(L::kWxTrackTemp, d.trackTemp, "track temperature");
            pair(L::kWxBaroPressure, d.baroPressure, "barometric pressure");
            pair(L::kWxRelHumidity, d.relHumidity, "relative humidity");
            pair(L::kWxWindSpeed, d.windSpeed, "wind speed");
            putText64(img, w + L::kWxWindDirection, d.windDirection, notes, "wind direction");
            putText(img, w + L::kWxComment, L::kLongCommentWidth, L::kLongCommentWidth - 1, d.weatherComment,
                    notes, "weather comment");
        }
    }

    // ---- device-event block -------------------------------------------------------------------------------
    if (eventsOff != 0) {
        img.put<std::uint32_t>(eventsOff, L::kEventBlockType);
        img.put<std::uint32_t>(eventsOff + 4, u32(in.events.size()));
        img.put<std::uint32_t>(eventsOff + 8, u32(eventsOff + L::kEventBlockHeaderSize));
        for (std::size_t i = 0; i < in.events.size(); ++i) {
            const std::uint64_t o = eventsOff + L::kEventBlockHeaderSize + i * L::kEventRecordBytes;
            img.put<std::uint16_t>(o, in.events[i].code);
            img.put<double>(o + 2, in.events[i].timeUs);
        }
    }

    // ---- enumeration block
    // ----------------------------------------------------------------------------------
    if (enumOff != 0) {
        std::uint64_t o = enumOff;
        img.put<std::uint32_t>(o, L::kEnumBlockMarker);
        img.put<std::uint32_t>(o + 4, u32(in.enumTables.size()));
        o += 8;
        std::size_t label = 0;
        for (const LdEnumTable& t : in.enumTables) {
            if (t.entries.size() > 0xFFFF) {
                return fail(ErrorCode::Invalid, "LdWriter: enumeration table with more than 65,535 values");
            }
            img.put<std::uint32_t>(o, L::kEnumTableMarker);
            img.put<std::uint16_t>(o + 4, static_cast<std::uint16_t>(t.entries.size()));
            img.put<std::uint16_t>(o + 6, L::kEnumIndexSize);
            img.put<std::uint16_t>(o + 8, t.index);
            o += 10;
            for (const LdEnumEntry& e : t.entries) {
                const std::vector<std::byte>& lb = enumLabels[label++];
                const bool ext = e.extended || e.severity != 0;
                img.put<std::uint32_t>(o, L::kEnumValueMarker);
                img.put<std::uint16_t>(o + 4, ext ? 1 : 0);
                img.put<std::uint16_t>(o + 6, static_cast<std::uint16_t>(lb.size() + 3));
                img.put<std::int16_t>(o + 8, e.value);
                img.bytes(o + 10, lb);
                o += 8 + lb.size() + 3;
                if (ext) {
                    img.put<std::uint32_t>(o, L::kEnumSeverityMarker);
                    img.put<std::uint16_t>(o + 6, L::kEnumIndexSize);
                    img.put<std::uint16_t>(o + 8, e.severity);
                    o += L::kEnumSeverityBlockSize;
                }
            }
        }
    }

    // ---- channel records, gap and aux blocks
    // -----------------------------------------------------------------
    for (std::size_t i = 0; i < in.channels.size(); ++i) {
        const LdWriteChannel& c = in.channels[i];
        const ChannelPlan& p = plans[i];
        const std::uint64_t r = p.record;
        const std::optional<std::string> chName = c.name;
        const std::uint64_t prev = i == 0 ? 0 : plans[i - 1].record;
        const std::uint64_t next = i + 1 < plans.size() ? plans[i + 1].record : (terminator ? termOff : 0);
        if (c.rawRecord) {
            img.bytes(r, *c.rawRecord);
        } else {
            img.put<std::uint16_t>(r + L::kRecCounter,
                                   c.counter.value_or(static_cast<std::uint16_t>(L::kFirstCounter + i)));
            img.put<std::uint16_t>(r + L::kRecTypeCode, c.typeCode);
            img.put<std::uint16_t>(r + L::kRecWidth, c.width);
            img.put<std::uint16_t>(r + L::kRecRate, c.rateHz);
            img.put<std::int16_t>(r + L::kRecShift, c.shift);
            img.put<std::int16_t>(r + L::kRecMul, c.mul);
            img.put<std::int16_t>(r + L::kRecScale, c.scale);
            img.put<std::int16_t>(r + L::kRecDec, c.dec);
            putText(img, r + L::kRecName, L::kNameWidth, L::kNameWidth, c.name, notes, "channel name");
            if (cSeries) {
                putText(img, r + L::kRecShortName, L::kShortNameWidth, L::kShortNameWidth, c.shortName, notes,
                        "short name");
                putText(img, r + L::kRecUnit, L::kUnitWidth, L::kUnitWidth, c.unit, notes, "unit");
            } else {
                putText(img, r + L::kRecShortName, L::kLongUnitWidth, L::kLongUnitWidth, c.unit, notes,
                        "unit");
            }
            // Raw max/min of the valid stored values (0x54/0x58).
            std::int32_t mx = 0, mn = 0;
            if (!c.rawMax || !c.rawMin) {
                std::vector<LdInterval> inv = c.invalid;
                std::sort(inv.begin(), inv.end(),
                          [](const LdInterval& a, const LdInterval& b) { return a.begin < b.begin; });
                double hi = -std::numeric_limits<double>::infinity();
                double lo = std::numeric_limits<double>::infinity();
                std::size_t k = 0;
                for (std::uint64_t s = 0; s < p.count; ++s) {
                    while (k < inv.size() && inv[k].end <= s) {
                        ++k;
                    }
                    if (k < inv.size() && inv[k].begin <= s) {
                        continue;
                    }
                    const double v = storedValue(c, static_cast<std::size_t>(s), p.isFloat);
                    if (v == v) {
                        hi = std::max(hi, v);
                        lo = std::min(lo, v);
                    }
                }
                if (lo <= hi) {
                    mx = toI32Sat(hi);
                    mn = toI32Sat(lo);
                }
            }
            img.put<std::int32_t>(r + L::kRecRawMax, c.rawMax.value_or(mx));
            img.put<std::int32_t>(r + L::kRecRawMin, c.rawMin.value_or(mn));
            if (c.displayRange) {
                img.put<float>(r + L::kRecRangeA, c.displayRange->first);
                img.put<float>(r + L::kRecRangeB, c.displayRange->second);
            }
            img.put<std::uint8_t>(r + L::kRecDps, c.dpsByte);
            img.put<std::uint8_t>(r + L::kRecInterp, c.interpByte);
        }
        img.put<std::uint32_t>(r + L::kRecPrev, u32(prev));
        img.put<std::uint32_t>(r + L::kRecNext, u32(next));
        img.put<std::uint32_t>(r + L::kRecData, u32(p.data));
        img.put<std::uint32_t>(r + L::kRecCount, c.declaredCount.value_or(u32(p.count)));
        img.put<std::uint32_t>(r + L::kRecGapPtr, u32(p.gap));
        img.put<std::uint32_t>(r + L::kRecAuxPtr, legacy ? 0u : u32(p.aux));

        if (legacy) {
            // 132-byte record: the extra u32 at +0x80 points 8 bytes before the unreferenced block.
            img.put<std::uint32_t>(r + 0x80, u32(p.aux - 8));
            img.put<std::uint32_t>(p.aux + 2, u32(p.aux + kLegacyAuxContentOffset));
            const std::uint64_t content = p.aux + kLegacyAuxContentOffset;
            img.put<std::uint32_t>(content, L::kAuxContentMarker);
            img.put<std::uint32_t>(content + 4, p.hasLongName ? 1u : 0u);
            if (p.hasLongName) {
                img.put<std::uint32_t>(content + 8, L::kAuxArgLongName);
                img.put<std::uint16_t>(content + 14, static_cast<std::uint16_t>(p.longName.size() + 1));
                img.bytes(content + 16, p.longName);
            }
        }
        if (p.gap != 0) {
            img.put<std::uint16_t>(p.gap + L::kGapRate, c.rateHz);
            img.put<std::uint32_t>(p.gap + L::kGapCount, p.gapEvents);
            img.put<std::uint32_t>(p.gap + L::kGapArrayPtr, u32(p.gap + L::kGapHeaderSize));
            std::uint64_t o = p.gap + L::kGapHeaderSize;
            const auto event = [&](std::uint16_t kind, std::uint64_t idx) {
                img.put<std::uint16_t>(o, kind);
                img.put<std::uint32_t>(o + 2, u32(std::min<std::uint64_t>(idx, 0xFFFFFFFFu)));
                o += L::kGapEventSize;
            };
            if (c.rawGapEvents) {
                for (const auto& [kind, idx] : *c.rawGapEvents) {
                    event(kind, idx);
                }
            } else {
                for (const LdInterval& iv : c.invalid) {
                    event(L::kGapEnter, iv.begin);
                    event(L::kGapExit, iv.end);
                }
            }
        }
        if (p.aux != 0 && !legacy) {
            const std::uint64_t content = p.aux + L::kAuxContentOffset;
            img.put<std::uint32_t>(p.aux + L::kAuxContentPtr, u32(content));
            img.put<std::uint32_t>(content, L::kAuxContentMarker);
            img.put<std::uint32_t>(content + 4, (c.enumRef ? 1u : 0u) + (p.hasLongName ? 1u : 0u));
            std::uint64_t o = content + 8;
            if (c.enumRef) {
                img.put<std::uint32_t>(o, L::kAuxArgEnumRef);
                img.put<std::uint16_t>(o + 6, L::kEnumIndexSize);
                img.put<std::uint16_t>(o + 8, *c.enumRef);
                o += 10;
            }
            if (p.hasLongName) {
                img.put<std::uint32_t>(o, L::kAuxArgLongName);
                img.put<std::uint16_t>(o + 6, static_cast<std::uint16_t>(p.longName.size() + 1));
                img.bytes(o + 8, p.longName);
            }
        }

        // Samples.
        const std::uint64_t w = c.width;
        if (!c.rawData.empty()) {
            img.bytes(p.data, c.rawData);
        } else if (p.isFloat) {
            for (std::uint64_t s = 0; s < p.count; ++s) {
                const double v = c.rawFloats[static_cast<std::size_t>(s)];
                const std::uint64_t o = p.data + s * w;
                if (w == 2) {
                    img.put<std::uint16_t>(o, doubleToHalf(v));
                } else if (w == 4) {
                    img.put<float>(o, narrowToFloat(v));
                } else {
                    img.put<double>(o, v);
                }
            }
        } else {
            const bool isUnsigned = c.typeCode == L::kTypeUnsigned4 && w < 8;
            const std::int64_t lo = isUnsigned ? 0
                                    : w == 8   ? std::numeric_limits<std::int64_t>::min()
                                               : -(std::int64_t{1} << (8 * w - 1));
            const std::int64_t hi = w == 8       ? std::numeric_limits<std::int64_t>::max()
                                    : isUnsigned ? (std::int64_t{1} << (8 * w)) - 1
                                                 : (std::int64_t{1} << (8 * w - 1)) - 1;
            bool clamped = false;
            for (std::uint64_t s = 0; s < p.count; ++s) {
                std::int64_t v = c.rawInts[static_cast<std::size_t>(s)];
                if (v < lo || v > hi) {
                    v = std::clamp(v, lo, hi);
                    clamped = true;
                }
                const std::uint64_t o = p.data + s * w;
                switch (w) {
                case 1:
                    img.put<std::uint8_t>(o, static_cast<std::uint8_t>(v));
                    break;
                case 2:
                    img.put<std::uint16_t>(o, static_cast<std::uint16_t>(v));
                    break;
                case 4:
                    img.put<std::uint32_t>(o, static_cast<std::uint32_t>(v));
                    break;
                default:
                    img.put<std::int64_t>(o, v);
                    break;
                }
            }
            if (clamped) {
                notes.add(code::kValueClamped, "raw values outside the stored width clamped", chName);
            }
        }
    }
    if (terminator) {
        img.put<std::uint32_t>(termOff + L::kRecPrev, u32(plans.empty() ? 0 : plans.back().record));
    }
    return img.take();
}

Result<std::uint64_t> LdWriter::write(const std::filesystem::path& path, const LdWriteInput& input,
                                      const LdWriteOptions& options, ImportLog* notes) {
    auto bytes = build(input, options, notes);
    if (!bytes) {
        return Failure{bytes.error()};
    }
    if (auto r = AtomicFile::write(path, std::span<const std::byte>(*bytes)); !r) {
        return Failure{r.error()};
    }
    return static_cast<std::uint64_t>(bytes->size());
}

Result<LdWriteInput> LdWriter::inputFromFile(const LdFile& f) {
    LdWriteInput in;
    LdWriteDetails& d = in.details;
    const LdHeader& h = f.header();
    d.date = h.date;
    d.time = h.time;
    d.driver = h.driver;
    d.vehicleId = h.vehicleId;
    d.engineId = h.engineId;
    d.venue = h.venue;
    d.session = h.session;
    d.shortComment = h.shortComment;
    d.team = h.team;
    if (const auto& e = f.eventRecord()) {
        d.eventName = e->name;
        d.eventSession = e->session;
        d.longComment = e->longComment;
    }
    if (const auto& v = f.venueRecord()) {
        d.venueName = v->name;
        d.venueLengthMm = v->lengthMm;
        d.venueCategory = v->category;
    }
    if (const auto& v = f.vehicleRecord()) {
        d.vehicleRecordId = v->id;
        d.vehicleDesc = v->desc;
        d.vehicleEngineId = v->engineId;
        d.weightKg = v->weightKg;
        d.fuelTankX10 = v->fuelTankX10;
        d.vehicleType = v->type;
        d.driveType = v->driveType;
        d.diffX1000 = v->diffX1000;
        d.gearsX1000 = v->gearsX1000;
        d.trackMm = v->trackMm;
        d.wheelbaseMm = v->wheelbaseMm;
        d.vehicleComment = v->comment;
        d.vehicleNumber = v->number;
    }
    if (const auto& w = f.weatherRecord()) {
        d.sky = w->sky;
        d.airTemp = w->airTemp;
        d.trackTemp = w->trackTemp;
        d.baroPressure = w->baroPressure;
        d.relHumidity = w->relHumidity;
        d.windSpeed = w->windSpeed;
        d.windDirection = w->windDirection;
        d.weatherComment = w->comment;
    }
    const std::span<const std::byte> bytes = f.bytes();
    for (const LdChannel& c : f.channels()) {
        LdWriteChannel w;
        const LdChannelEncoding& e = c.enc;
        w.name = c.name32;
        w.longName = c.longName;
        w.shortName = c.shortName;
        w.unit = c.unit;
        w.typeCode = e.typeCode;
        w.width = e.bytesPerSample;
        w.rateHz = e.rateHz;
        w.shift = e.shift;
        w.mul = e.mul;
        w.scale = e.scale;
        w.dec = e.dec;
        w.dpsByte = e.dpsByte;
        w.interpByte = e.interpByte;
        w.counter = e.counter;
        w.enumRef = c.enumRef;
        w.longNamePadByte = c.longNamePadByte;
        w.rawMax = e.rawMax;
        w.rawMin = e.rawMin;
        w.rawRecord = e.raw;
        w.invalid = c.invalid;
        const std::uint64_t n = c.sampleCount * e.bytesPerSample;
        const auto data = bytes.subspan(static_cast<std::size_t>(e.dataOffset), static_cast<std::size_t>(n));
        w.rawData.assign(data.begin(), data.end());
        in.channels.push_back(std::move(w));
    }
    in.enumTables = f.enumTables();
    for (const LdDeviceEvent& e : f.deviceEvents()) {
        in.events.push_back({e.code, e.timeUs});
    }
    return in;
}

LdWriteOptions LdWriter::optionsFromFile(const LdFile& f) {
    LdWriteOptions o;
    const std::uint32_t dia = f.dialect();
    o.dialect = (dia & dialect::kCSeries) != 0 ? WriteDialect::CSeries
                : (dia & dialect::kM1) != 0    ? WriteDialect::M1
                                               : WriteDialect::PcLayout;
    o.terminator = f.recordsWalked() > f.channels().size() && (dia & dialect::kM1) != 0;
    // The logger layout (u16 0 at 0x040, event record after a configuration region) is what the reader keys
    // the C-series record rules on; a header-modified copy keeps it although its device type is not
    // C<digits>, so the copy written back keeps it too.
    if (f.header().unknown040 == 0 && f.header().eventPtr > 0x10000u) {
        o.configRegionBytes = 0x10000u;
    }
    o.dataAlignment = 1;
    const auto copy = [](std::span<const std::byte> s) { return std::vector<std::byte>(s.begin(), s.end()); };
    o.rawHeader = copy(f.rawHeader());
    o.rawEvent = copy(f.rawEventRecord());
    o.rawVenue = copy(f.rawVenueRecord());
    o.rawVehicle = copy(f.rawVehicleRecord());
    o.rawWeather = copy(f.rawWeatherRecord());
    return o;
}

std::int64_t quantiseRaw(double value, std::int16_t shift, std::int16_t mul, std::int16_t scale,
                         std::int16_t dec, ShiftOrder order) noexcept {
    const double s = scale == 0 ? 1.0 : static_cast<double>(scale);
    const double m = mul == 0 ? 1.0 : static_cast<double>(mul);
    const double p = std::pow(10.0, static_cast<double>(std::abs(static_cast<int>(dec))));
    const double rawFactor = dec < 0 ? p : 1.0;
    const double den = dec >= 0 ? s * p : s;
    const auto sh = static_cast<double>(shift);
    const double raw = order == ShiftOrder::AfterMul ? (value - sh) * den / (rawFactor * m)
                                                     : (value * den / m - sh * den) / rawFactor;
    if (!(raw == raw)) {
        return 0;
    }
    if (raw >= 9.2e18) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (raw <= -9.2e18) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return std::llround(raw);
}

} // namespace esses::formats::ld
