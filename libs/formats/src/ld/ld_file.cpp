// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// New code written from docs/formats/ld-converter-port.md (sections 1 and 3), docs/formats/ld.md and the
// public MIT layout notes (D-144). Nothing here is taken from the converter's sources or from any GPL
// project.
#include "ld/ld_file.hpp"

#include "ld/ld_codes.hpp"
#include "ld/ld_strings.hpp"

#include <esses/base/assert.hpp>
#include <esses/base/byte_reader.hpp>
#include <esses/store/resampler.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace esses::formats::ld {

using Level = ImportLogEntry::Level;
using Context = ImportLogEntry::Context;
namespace L = layout;

std::string dialectString(std::uint32_t flags) {
    static constexpr std::pair<std::uint32_t, std::string_view> kNames[] = {
        {dialect::kM1, "m1"},
        {dialect::kCSeries, "c-series"},
        {dialect::kPcLayout, "pc-layout"},
        {dialect::kConverterLegacy, "converter-legacy"},
        {dialect::kSimExporter, "sim-exporter"},
        {dialect::kProLogging, "pro-logging"}};
    std::string out;
    for (const auto& [bit, name] : kNames) {
        if ((flags & bit) != 0) {
            if (!out.empty()) {
                out += ',';
            }
            out += name;
        }
    }
    return out;
}

std::string deviceEventLabel(std::uint16_t code) {
    switch (code) {
    case 0:
        return "Firmware start";
    case 1:
        return "Firmware stop";
    case 2:
        return "Power cycled";
    case 3:
        return "Logging on";
    case 4:
        return "Logging off";
    case 5:
        return "Log discontinuity";
    default:
        return "Event " + std::to_string(code);
    }
}

namespace {

// Bounds-checked little-endian reads at absolute 64-bit offsets; a read that does not fit returns 0.
class Bytes {
public:
    explicit Bytes(std::span<const std::byte> b) noexcept : b_(b) {}
    std::uint64_t size() const noexcept { return b_.size(); }
    bool fits(std::uint64_t off, std::uint64_t len) const noexcept {
        return off <= b_.size() && len <= b_.size() - off;
    }
    template <class T> T at(std::uint64_t off) const noexcept {
        if (!fits(off, sizeof(T))) {
            return T{};
        }
        return loadAs<T>(b_.data() + off, Endian::Little);
    }
    std::uint8_t u8(std::uint64_t o) const noexcept { return at<std::uint8_t>(o); }
    std::uint16_t u16(std::uint64_t o) const noexcept { return at<std::uint16_t>(o); }
    std::int16_t i16(std::uint64_t o) const noexcept { return at<std::int16_t>(o); }
    std::uint32_t u32(std::uint64_t o) const noexcept { return at<std::uint32_t>(o); }
    std::int32_t i32(std::uint64_t o) const noexcept { return at<std::int32_t>(o); }
    float f32(std::uint64_t o) const noexcept { return at<float>(o); }
    double f64(std::uint64_t o) const noexcept { return at<double>(o); }
    // The part of [off, off + len) that lies in the file.
    std::span<const std::byte> span(std::uint64_t off, std::uint64_t len) const noexcept {
        if (off >= b_.size()) {
            return {};
        }
        return b_.subspan(static_cast<std::size_t>(off),
                          static_cast<std::size_t>(std::min<std::uint64_t>(len, b_.size() - off)));
    }

private:
    std::span<const std::byte> b_;
};

std::string hex(std::uint64_t v) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v, 16);
    return "0x" + std::string(buf, r.ptr);
}

bool parseInts(std::string_view s, char sep, int* out, int n, bool allowShort) {
    int k = 0;
    std::size_t pos = 0;
    while (true) {
        const std::size_t end = s.find(sep, pos);
        const std::string_view part =
            s.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        if (k >= n || part.empty() || part.size() > 4) {
            return false;
        }
        const auto r = std::from_chars(part.data(), part.data() + part.size(), out[k]);
        if (r.ec != std::errc() || r.ptr != part.data() + part.size()) {
            return false;
        }
        ++k;
        if (end == std::string_view::npos) {
            break;
        }
        pos = end + 1;
    }
    return k == n || (allowShort && k == n - 1);
}

std::optional<LocalDateTime> parseDateTime(std::string_view date, std::string_view time) {
    int d[3] = {0, 0, 0};
    int t[3] = {0, 0, 0};
    if (!parseInts(date, '/', d, 3, false) || !parseInts(time, ':', t, 3, true)) {
        return std::nullopt;
    }
    LocalDateTime dt{d[2], d[1], d[0], t[0], t[1], t[2]};
    if (dt.day < 1 || dt.day > 31 || dt.month < 1 || dt.month > 12 || dt.year < 1900 || dt.year > 2999 ||
        dt.hour > 23 || dt.minute > 59 || dt.second > 60 || dt.hour < 0 || dt.minute < 0 || dt.second < 0) {
        return std::nullopt;
    }
    return dt;
}

// Logger files keep their event record after a device-configuration region; PC-software files at 0x6E2.
constexpr std::uint32_t kLoggerEventFloor = 0x10000u;
// Aux blocks hold one or two arguments (port specification section 1.9); more are not read.
constexpr std::uint32_t kAuxMaxArgs = 16;

// Storage dtype of a scaled integer channel (05 section 3.5 as amended): F64 when F32 would lose raw steps,
// that is when the effective number of decimals dec + floor(log10 |scale|) is 5 or more (lat/lon-like
// values; M1 files carry decimals in scale = 10000 with dec 1-4, E-24), or when the record's raw range
// (0x54/0x58) of a 32- or 64-bit integer exceeds 2^24; F32 otherwise.
bool scaledNeedsF64(const LdChannelEncoding& e, SampleType t) noexcept {
    int scaleDigits = 0;
    for (int scale = e.scale == 0 ? 1 : std::abs(static_cast<int>(e.scale)); scale >= 10; scale /= 10) {
        ++scaleDigits;
    }
    if (e.dec + scaleDigits >= 5) {
        return true;
    }
    const bool wide = t == SampleType::I32 || t == SampleType::U32 || t == SampleType::I64;
    constexpr std::int64_t kF32Exact = std::int64_t{1} << 24;
    return wide &&
           (std::abs(std::int64_t{e.rawMax}) > kF32Exact || std::abs(std::int64_t{e.rawMin}) > kF32Exact);
}
// Enumeration scan (05 section 3.9): searched window, longest label item, budget slack in bytes.
constexpr std::uint64_t kEnumScanWindow = std::uint64_t{16} << 20;
constexpr std::uint32_t kEnumScanMaxItem = 1024;
constexpr std::uint64_t kEnumScanSlack = std::uint64_t{1} << 16;

bool isCSeriesDevice(std::string_view type) {
    return type.size() >= 2 && type[0] == 'C' &&
           std::all_of(type.begin() + 1, type.end(), [](char c) { return c >= '0' && c <= '9'; });
}

struct TypeResolution {
    std::optional<SampleType> type;
    bool guessed = false;
    bool unknown = false;
    bool bitfield = false;
};

TypeResolution resolveType(std::uint16_t code, std::uint16_t width) {
    const bool intClass = code == L::kTypeInt0 || code == L::kTypeInt3 || code == L::kTypeInt5;
    if (code == L::kTypeFloat && width == 4) {
        return {SampleType::F32};
    }
    if (code == L::kTypeFloat && width == 2) {
        return {SampleType::F16};
    }
    if (code == L::kTypeDouble && width == 8) {
        return {SampleType::F64};
    }
    if ((intClass || code == L::kTypeInt6) && width == 4) {
        return {SampleType::I32};
    }
    if (intClass && width == 2) {
        return {SampleType::I16};
    }
    // Pairs decoded by a guess the public sources do not agree on.
    if ((intClass || code == L::kTypeInt6) && width == 1) {
        return {SampleType::I8, true};
    }
    if (code == L::kTypeInt6 && width == 2) {
        return {SampleType::I16, true};
    }
    if ((intClass || code == L::kTypeInt6) && width == 8) {
        return {SampleType::I64, true};
    }
    if (code == L::kTypeUnsigned4) {
        switch (width) {
        case 1:
            return {SampleType::U8, true, false, true};
        case 2:
            return {SampleType::U16, true, false, true};
        case 4:
            return {SampleType::U32, true, false, true};
        case 8:
            return {SampleType::I64, true, false, true};
        default:
            return {std::nullopt, false, true};
        }
    }
    if (code == L::kTypeFloat && width == 8) {
        return {SampleType::F64, true};
    }
    if (code == L::kTypeDouble && width == 2) {
        return {SampleType::I16, true};
    }
    if (code == L::kTypeDouble && width == 4) {
        return {SampleType::I32, true};
    }
    // Unknown pair: a raw signed integer of the stored width when that width is decodable.
    switch (width) {
    case 1:
        return {SampleType::I8, false, true};
    case 2:
        return {SampleType::I16, false, true};
    case 4:
        return {SampleType::I32, false, true};
    case 8:
        return {SampleType::I64, false, true};
    default:
        return {std::nullopt, false, true};
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------------------
class LdParser {
public:
    explicit LdParser(LdFile& f) : f_(f), b_(f.bytes_) {}

    Result<void> run() {
        if (b_.size() < L::kHeaderSize) {
            ErrorContext ctx;
            ctx.path = f_.path_;
            ctx.detail = std::string(code::kTooSmall);
            return fail(ErrorCode::Format,
                        std::string(code::kTooSmall) + ": file of " + std::to_string(b_.size()) +
                            " bytes is shorter than the 1,762-byte .ld header",
                        std::move(ctx));
        }
        parseHeader();
        detectHeaderDialect();
        parseDetails();
        parseDeviceEvents();
        parseEnumBlock();
        walkChannels();
        resolveEnums();
        finishChannels();
        flushAggregates();
        finishFile();
        return ok();
    }

private:
    struct Aggregate {
        std::uint32_t count = 0;
        std::string message;
        std::optional<std::string> channel;
        std::optional<std::uint64_t> offset;
    };

    void add(std::string_view c, std::string message, Context ctx = {}) {
        const CodeInfo* info = findCode(c);
        ESSES_ASSERT_MSG(info != nullptr, "LdParser: import log code missing from the code table");
        f_.log_.add(info != nullptr ? info->level : Level::Info, std::string(c), std::move(message),
                    std::move(ctx));
    }
    static Context at(std::uint64_t offset, std::optional<std::string> channel = std::nullopt,
                      std::optional<std::string> detail = std::nullopt) {
        Context ctx;
        ctx.offset = offset;
        ctx.channel = std::move(channel);
        ctx.detail = std::move(detail);
        return ctx;
    }
    // Per-channel Info codes that fire on many channels are logged once per file with ctx.repeats = count.
    void aggregate(std::string_view c, std::string message, const std::optional<std::string>& channel,
                   std::uint64_t offset) {
        auto [it, inserted] = aggregates_.try_emplace(std::string(c));
        if (inserted) {
            aggregateOrder_.push_back(it->first);
            it->second.message = std::move(message);
            it->second.channel = channel;
            it->second.offset = offset;
        }
        ++it->second.count;
    }
    void flushAggregates() {
        for (const std::string& c : aggregateOrder_) {
            const Aggregate& a = aggregates_.at(c);
            Context ctx;
            ctx.channel = a.channel;
            ctx.offset = a.offset;
            ctx.repeats = a.count;
            std::string message = a.message;
            if (a.count > 1) {
                message += " (" + std::to_string(a.count) + " occurrences; first named)";
            }
            add(c, std::move(message), std::move(ctx));
        }
        aggregates_.clear();
        aggregateOrder_.clear();
    }

    FieldText text(std::uint64_t off, std::uint32_t width, const std::optional<std::string>& channel = {}) {
        FieldText t = decodeField(b_.span(off, width));
        if (t.encoding == TextEncoding::Utf8) {
            sawUtf8_ = true;
        } else if (t.encoding == TextEncoding::Cp1252) {
            sawCp1252_ = true;
        }
        if (t.trailingBytes) {
            aggregate(code::kStringTrailingBytes, "non-zero bytes after the NUL of a string field", channel,
                      off);
        }
        return t;
    }
    std::string str(std::uint64_t off, std::uint32_t width) { return text(off, width).text; }

    // ---- R1 header ------------------------------------------------------------------------------------
    void parseHeader() {
        LdHeader& h = f_.header_;
        h.magic = b_.u32(L::kHdrMagic);
        h.unknownPtr004 = b_.u32(L::kHdrUnknownPtr004);
        h.metaPtr = b_.u32(L::kHdrMetaPtr);
        h.dataPtr = b_.u32(L::kHdrDataPtr);
        h.eventPtr = b_.u32(L::kHdrEventPtr);
        h.unknown040 = b_.u16(L::kHdrUnknown040);
        h.ticks = b_.u32(L::kHdrTicks);
        h.serial = b_.u32(L::kHdrSerial);
        h.deviceType = str(L::kHdrDeviceType, L::kDeviceTypeWidth);
        h.deviceVersion = b_.u16(L::kHdrDeviceVersion);
        h.unknown054 = b_.u16(L::kHdrUnknown054);
        h.channelCount = b_.u16(L::kHdrChannelCount);
        h.channelCount2 = b_.u16(L::kHdrChannelCount2);
        h.unknown05A = b_.u16(L::kHdrUnknown05A);
        h.unknown05C = b_.u16(L::kHdrUnknown05C);
        h.date = str(L::kHdrDate, L::kDateReadWidth);
        h.time = str(L::kHdrTime, L::kTimeReadWidth);
        h.driver = str(L::kHdrDriver, L::kText64);
        h.vehicleId = str(L::kHdrVehicleId, L::kText64);
        h.engineId = str(L::kHdrEngineId, L::kText64);
        h.venue = str(L::kHdrVenue, L::kText64);
        h.proLoggingNonZero = b_.u32(L::kHdrProLogging) != 0;
        h.session = str(L::kHdrSession, L::kText64);
        h.shortComment = str(L::kHdrShortComment, L::kText64);
        h.unknown687 = b_.u8(L::kHdrUnknown687);
        h.deviceEventPtr = b_.u32(L::kHdrDeviceEventPtr);
        h.unknownPtr68C = b_.u32(L::kHdrUnknownPtr68C);
        h.enumPtr = b_.u32(L::kHdrEnumPtr);
        h.team = str(L::kHdrTeam, L::kText32);

        const auto raw = b_.span(0, L::kHeaderSize);
        f_.rawHeader_.assign(raw.begin(), raw.end());
        std::fill_n(f_.rawHeader_.begin() + L::kHdrProLogging, 4, std::byte{0}); // D-143

        if (h.magic != L::kMagic) {
            add(code::kMagicUnexpected, "u32 at 0x00 is " + hex(h.magic) + ", expected 0x40", at(0));
        }
        if (h.proLoggingNonZero) {
            add(code::kProLoggingNonzero, "u32 at 0x5DE is non-zero (recorded only as the pro-logging flag)",
                at(L::kHdrProLogging));
        }
        h.dateTime = parseDateTime(h.date, h.time);
        if (!h.dateTime) {
            add(code::kDateUnparsable,
                "header date/time '" + h.date + " " + h.time + "' not DD/MM/YYYY HH:MM:SS", at(L::kHdrDate));
        }
    }

    void detectHeaderDialect() {
        const LdHeader& h = f_.header_;
        std::uint32_t d = 0;
        if (h.deviceType == "M1") {
            d |= dialect::kM1;
        }
        // C-series logger layout: u16 at 0x40 is 0 and the event record sits after the device-configuration
        // region. This layout evidence, not the device type, selects the C-series record rules (unit rule,
        // interpolation byte 0): the header-modified copies of logger files (docs/formats/ld.md "Producer
        // fingerprints") replace the device type but keep the logger's channel records byte for byte. The
        // dialect label c-series also needs the logger's own device type (C<digits>).
        cSeriesRecords_ = h.unknown040 == 0 && h.eventPtr > kLoggerEventFloor;
        if (cSeriesRecords_ && isCSeriesDevice(h.deviceType)) {
            d |= dialect::kCSeries;
        }
        if (h.eventPtr == L::kHeaderSize && h.unknown054 == 0x0080) {
            d |= dialect::kPcLayout;
        }
        // Header defaults of the public writers (docs/formats/ld.md "Producer fingerprints"; unsettled: no
        // real file backs either). The [mi2] value u32 0x00010064 at 0x05A alone is not enough, because the
        // vendor PC software and the converter write it too (a C185 file re-saved on the PC carries it): the
        // [mi2] writer also writes 0 at 0x040, which PC-software files never do (2), and it does not write
        // the logger layout.
        const bool gotzlDefaults = h.deviceType == "ADL" && h.unknown054 == 0xADB0;
        const bool mi2Defaults = h.unknown05A == 0x0064 && h.unknown05C == 1 && h.unknown040 == 0 &&
                                 h.eventPtr <= kLoggerEventFloor;
        if (gotzlDefaults || mi2Defaults) {
            d |= dialect::kSimExporter;
        }
        if (h.proLoggingNonZero) {
            d |= dialect::kProLogging;
        }
        f_.dialect_ = d;
    }

    // ---- R2 detail records through their pointers ---------------------------------------------------------
    bool recordFits(std::uint32_t ptr, std::uint32_t used, std::string_view what) {
        if (ptr == 0) {
            return false;
        }
        if (ptr < L::kHeaderSize || !b_.fits(ptr, used)) {
            add(code::kBlockOutOfRange,
                std::string(what) + " record pointer " + hex(ptr) +
                    " does not fit in the file; record skipped",
                at(ptr, std::nullopt, std::string(what)));
            return false;
        }
        return true;
    }
    std::vector<std::byte> rawRecord(std::uint32_t ptr, std::uint32_t size) const {
        const auto s = b_.span(ptr, size);
        return {s.begin(), s.end()};
    }

    void parseDetails() {
        const std::uint32_t ev = f_.header_.eventPtr;
        if (!recordFits(ev, L::kEvUsed, "event")) {
            return;
        }
        LdEventRecord e;
        e.offset = ev;
        e.name = str(ev + L::kEvName, L::kText64);
        e.session = str(ev + L::kEvSession, L::kText64);
        e.longComment = str(ev + L::kEvLongComment, L::kLongCommentWidth);
        e.venuePtr = b_.u32(ev + L::kEvVenuePtr);
        e.weatherPtr = b_.u32(ev + L::kEvWeatherPtr);
        f_.rawEvent_ = rawRecord(ev, L::kEventRecordSize);
        f_.event_ = e;

        if (recordFits(e.venuePtr, L::kVnUsed, "venue")) {
            const std::uint32_t vp = e.venuePtr;
            LdVenueRecord v;
            v.offset = vp;
            v.name = str(vp + L::kVnName, L::kText64);
            v.lengthMm = b_.u32(vp + L::kVnLengthMm);
            v.vehiclePtr = b_.u32(vp + L::kVnVehiclePtr);
            v.category = str(vp + L::kVnCategory, L::kText32);
            f_.rawVenue_ = rawRecord(vp, L::kVenueRecordSize);
            f_.venue_ = v;
            if (recordFits(v.vehiclePtr, L::kVhUsed, "vehicle")) {
                const std::uint32_t hp = v.vehiclePtr;
                LdVehicleRecord h;
                h.offset = hp;
                h.id = str(hp + L::kVhId, L::kText64);
                h.desc = str(hp + L::kVhDesc, L::kText64);
                h.engineId = str(hp + L::kVhEngineId, L::kText64);
                h.weightKg = b_.u16(hp + L::kVhWeightKg);
                h.fuelTankX10 = b_.u16(hp + L::kVhFuelTankX10);
                h.type = str(hp + L::kVhType, L::kText32);
                h.driveType = str(hp + L::kVhDriveType, L::kText32);
                h.diffX1000 = b_.u16(hp + L::kVhDiffX1000);
                for (std::uint32_t g = 0; g < L::kGearCount; ++g) {
                    h.gearsX1000[g] = b_.u16(hp + L::kVhGear1X1000 + 2 * g);
                }
                h.trackMm = b_.u16(hp + L::kVhTrackMm);
                h.wheelbaseMm = b_.u16(hp + L::kVhWheelbaseMm);
                h.comment = str(hp + L::kVhComment, L::kLongCommentWidth);
                h.number = str(hp + L::kVhNumber, L::kText32);
                f_.rawVehicle_ = rawRecord(hp, L::kVehicleRecordSize);
                f_.vehicle_ = h;
            }
        }
        if (recordFits(e.weatherPtr, L::kWxUsed, "weather")) {
            const std::uint32_t wp = e.weatherPtr;
            LdWeatherRecord w;
            w.offset = wp;
            w.sky = str(wp + L::kWxSky, L::kText64);
            const auto pair = [&](std::uint32_t o) {
                return LdWeatherValue{str(wp + o, L::kWxValueWidth),
                                      str(wp + o + L::kWxUnitOffset, L::kWxUnitWidth)};
            };
            w.airTemp = pair(L::kWxAirTemp);
            w.trackTemp = pair(L::kWxTrackTemp);
            w.baroPressure = pair(L::kWxBaroPressure);
            w.relHumidity = pair(L::kWxRelHumidity);
            w.windSpeed = pair(L::kWxWindSpeed);
            w.windDirection = str(wp + L::kWxWindDirection, L::kText64);
            w.comment = str(wp + L::kWxComment, L::kLongCommentWidth);
            f_.rawWeather_ = rawRecord(wp, L::kWeatherRecordSize);
            f_.weather_ = w;
        }
    }

    // ---- R10 device events --------------------------------------------------------------------------------
    void parseDeviceEvents() {
        const std::uint32_t p = f_.header_.deviceEventPtr;
        if (p == 0) {
            return;
        }
        if (p < L::kHeaderSize || !b_.fits(p, L::kEventBlockHeaderSize)) {
            add(code::kEventBlockMalformed, "device-event block at " + hex(p) + " does not fit in the file",
                at(p));
            return;
        }
        const std::uint32_t type = b_.u32(p);
        if (type != L::kEventBlockType) {
            add(code::kEventBlockUnknown, "device-event block type " + std::to_string(type) + " ignored",
                at(p));
            return;
        }
        std::uint64_t count = b_.u32(p + 4);
        const std::uint64_t arr = b_.u32(p + 8);
        if (count != 0 && arr < L::kHeaderSize) {
            // An array inside the header would read header bytes as events.
            add(code::kEventBlockMalformed,
                "device-event record array pointer " + hex(arr) + " lies inside the header; no records read",
                at(p));
            return;
        }
        const std::uint64_t avail = arr <= b_.size() ? (b_.size() - arr) / L::kEventRecordBytes : 0;
        if (count > avail) {
            add(code::kEventBlockMalformed,
                "device-event block declares " + std::to_string(count) + " records, " +
                    std::to_string(avail) + " fit in the file",
                at(p));
            count = avail;
        }
        std::uint32_t nonFinite = 0;
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t o = arr + i * L::kEventRecordBytes;
            LdDeviceEvent e;
            e.code = b_.u16(o);
            e.timeUs = b_.f64(o + 2);
            e.storedIndex = static_cast<std::uint32_t>(i);
            if (!std::isfinite(e.timeUs) || std::abs(e.timeUs) > 9.0e18) {
                ++nonFinite;
                continue;
            }
            e.tUs = static_cast<Micros>(std::llround(e.timeUs));
            f_.events_.push_back(e);
        }
        if (nonFinite != 0) {
            add(code::kEventBlockMalformed,
                std::to_string(nonFinite) + " device-event records with a non-finite time skipped", at(p));
        }
        std::stable_sort(f_.events_.begin(), f_.events_.end(),
                         [](const LdDeviceEvent& a, const LdDeviceEvent& b) {
                             return a.tUs < b.tUs || (a.tUs == b.tUs && a.storedIndex < b.storedIndex);
                         });
        for (std::size_t i = 1; i < f_.events_.size(); ++i) {
            if (f_.events_[i].tUs == f_.events_[i - 1].tUs) {
                f_.events_[i].ordinal = f_.events_[i - 1].ordinal + 1;
            }
        }
    }

    // ---- R4 enumeration definitions -----------------------------------------------------------------------
    // Limits of the signature scan (05 section 3.9): a candidate table must stay inside the scanned region,
    // its labels are short, and every byte examined is paid from a per-file budget (D-042: probe is
    // O(header)).
    struct ScanLimits {
        std::uint64_t end = 0;           // a candidate's bytes must lie below this offset
        std::uint32_t maxItemSize = 0;   // longest label item accepted
        std::uint64_t* budget = nullptr; // bytes still allowed to be examined
    };
    static bool spend(const ScanLimits* scan, std::uint64_t bytes) {
        if (scan == nullptr) {
            return true;
        }
        if (*scan->budget < bytes) {
            *scan->budget = 0;
            return false;
        }
        *scan->budget -= bytes;
        return true;
    }

    // Parses one table at `pos` (its u32 2 marker); advances pos. Returns an error message, or "" on success.
    // With `scan`, the value chain is first checked without decoding any label, inside the scan's limits.
    std::string parseEnumTable(std::uint64_t& pos, LdEnumTable& out, const ScanLimits* scan = nullptr) {
        if (scan != nullptr) {
            std::uint64_t probe = pos;
            if (std::string why = checkEnumChain(probe, *scan); !why.empty()) {
                return why;
            }
        }
        if (!b_.fits(pos, 10)) {
            return "table header beyond the file";
        }
        if (b_.u32(pos) != L::kEnumTableMarker) {
            return "table marker " + std::to_string(b_.u32(pos)) + " is not 2";
        }
        const std::uint16_t valueCount = b_.u16(pos + 4);
        const std::uint16_t indexSize = b_.u16(pos + 6);
        if (indexSize != L::kEnumIndexSize) {
            return "index size " + std::to_string(indexSize) + " is not 2";
        }
        out.offset = pos;
        out.index = b_.u16(pos + 8);
        pos += 10;
        for (std::uint32_t v = 0; v < valueCount; ++v) {
            if (!b_.fits(pos, 8)) {
                return "value beyond the file";
            }
            if (b_.u32(pos) != L::kEnumValueMarker) {
                return "value marker " + std::to_string(b_.u32(pos)) + " is not 3";
            }
            const std::uint16_t extended = b_.u16(pos + 4);
            const std::uint16_t itemSize = b_.u16(pos + 6);
            if (itemSize < 3) {
                return "item size " + std::to_string(itemSize) + " below 3";
            }
            if (!b_.fits(pos + 8, itemSize)) {
                return "value beyond the file";
            }
            LdEnumEntry e;
            e.value = b_.i16(pos + 8);
            e.label = text(pos + 10, static_cast<std::uint32_t>(itemSize - 2)).text;
            e.extended = extended != 0;
            pos += 8u + itemSize;
            if (e.extended) {
                if (!b_.fits(pos, L::kEnumSeverityBlockSize)) {
                    return "severity block beyond the file";
                }
                if (b_.u32(pos) != L::kEnumSeverityMarker) {
                    return "severity marker " + std::to_string(b_.u32(pos)) + " is not 6";
                }
                e.severity = b_.u16(pos + 8);
                pos += L::kEnumSeverityBlockSize;
            }
            out.entries.push_back(std::move(e));
        }
        return {};
    }

    // The structure of one table at `pos` without decoding labels (scan mode): markers, sizes and bounds.
    std::string checkEnumChain(std::uint64_t& pos, const ScanLimits& scan) const {
        if (!spend(&scan, 10) || pos + 10 > scan.end || b_.u32(pos) != L::kEnumTableMarker ||
            b_.u16(pos + 6) != L::kEnumIndexSize) {
            return "not a table";
        }
        const std::uint16_t valueCount = b_.u16(pos + 4);
        pos += 10;
        for (std::uint32_t v = 0; v < valueCount; ++v) {
            if (!spend(&scan, 8) || pos + 8 > scan.end || b_.u32(pos) != L::kEnumValueMarker) {
                return "not a value";
            }
            const std::uint16_t extended = b_.u16(pos + 4);
            const std::uint16_t itemSize = b_.u16(pos + 6);
            if (itemSize < 3 || itemSize > scan.maxItemSize || pos + 8 + itemSize > scan.end) {
                return "item size out of bounds";
            }
            pos += 8u + itemSize;
            if (extended != 0) {
                if (!spend(&scan, L::kEnumSeverityBlockSize) || pos + L::kEnumSeverityBlockSize > scan.end ||
                    b_.u32(pos) != L::kEnumSeverityMarker) {
                    return "not a severity block";
                }
                pos += L::kEnumSeverityBlockSize;
            }
        }
        return {};
    }

    // Adds a table; the first table with an index is the one channels resolve to (ld.enum_index_duplicate).
    void addEnumTable(LdEnumTable&& table) {
        const auto [it, inserted] = f_.enumByIndex_.try_emplace(table.index, f_.enums_.size());
        if (!inserted) {
            add(code::kEnumIndexDuplicate,
                "enumeration table at " + hex(table.offset) + " repeats index " +
                    std::to_string(table.index) + " of the table at " + hex(f_.enums_[it->second].offset) +
                    "; the first table is used",
                at(table.offset, std::nullopt, std::to_string(table.index)));
        }
        f_.enums_.push_back(std::move(table));
    }

    void parseEnumBlock() {
        const std::uint32_t p = f_.header_.enumPtr;
        if (p == 0) {
            return;
        }
        if (p < L::kHeaderSize || !b_.fits(p, 8) || b_.u32(p) != L::kEnumBlockMarker) {
            add(code::kEnumBlockMalformed,
                "enumeration block at " + hex(p) + " missing or without its marker", at(p));
            return;
        }
        const std::uint32_t tables = b_.u32(p + 4);
        std::uint64_t pos = p + 8;
        for (std::uint32_t t = 0; t < tables; ++t) {
            LdEnumTable table;
            const std::uint64_t start = pos;
            const std::string why = parseEnumTable(pos, table);
            if (!why.empty()) {
                add(code::kEnumBlockMalformed,
                    "enumeration table " + std::to_string(t) + " of " + std::to_string(tables) + ": " + why +
                        "; earlier tables kept",
                    at(start));
                return;
            }
            addEnumTable(std::move(table));
        }
    }

    // ---- R5-R7 channel records ----------------------------------------------------------------------------
    void walkChannels() {
        LdHeader& h = f_.header_;
        const std::uint64_t size = b_.size();
        if (h.dataPtr > size) {
            add(code::kDataPtrOutOfRange, "header data_ptr " + hex(h.dataPtr) + " is beyond the file",
                at(L::kHdrDataPtr));
        }
        if (h.metaPtr == size && h.dataPtr == size) {
            f_.headerOnly_ = true;
            add(code::kHeaderOnly, "meta_ptr and data_ptr equal the file size: no channel list",
                at(L::kHdrMetaPtr));
            return;
        }
        if (h.metaPtr < L::kHeaderSize || h.metaPtr >= size) {
            add(code::kMetaPtrOutOfRange, "header meta_ptr " + hex(h.metaPtr) + " is outside the file",
                at(L::kHdrMetaPtr));
            return;
        }
        const std::uint64_t maxRecords = size / L::kRecordSize;
        gapEventBudget_ = size / L::kGapEventSize;
        gapIntervalBudget_ = size / L::kGapEventSize;
        auxBudget_ = size;
        std::unordered_set<std::uint64_t> visited;
        std::uint64_t off = h.metaPtr;
        std::uint64_t prev = 0;
        std::uint32_t walked = 0;
        while (off != 0) {
            if (off < L::kHeaderSize || !b_.fits(off, L::kRecordSize)) {
                add(code::kEntryOutOfRange,
                    "channel record at " + hex(off) + " is outside the file; walk stopped", at(off));
                break;
            }
            if (!visited.insert(off).second) {
                add(code::kListCycle, "channel list revisits " + hex(off) + "; walk aborted", at(off));
                break;
            }
            if (walked >= maxRecords) {
                add(code::kListCycle,
                    "channel list longer than the " + std::to_string(maxRecords) +
                        " records that fit; walk aborted",
                    at(off));
                break;
            }
            ++walked;
            if (b_.u32(off + L::kRecPrev) != prev) {
                aggregate(code::kListPrevMismatch, "prev pointer does not name the previous record",
                          std::nullopt, off);
            }
            const std::uint32_t next = b_.u32(off + L::kRecNext);
            if (next != 0 && next < off) {
                aggregate(code::kListBackwards, "next pointer is lower than the current record", std::nullopt,
                          off);
            }
            // Legacy converter layout: records longer than 124 bytes with neither gap nor aux pointer.
            if (next > off + L::kRecordSize && b_.u32(off + L::kRecGapPtr) == 0 &&
                b_.u32(off + L::kRecAuxPtr) == 0) {
                legacyStride_ = true;
            }
            parseRecord(off);
            prev = off;
            off = next;
        }
        f_.recordsWalked_ = walked;
        if (h.channelCount != walked) {
            add(code::kCountMismatch,
                "header declares " + std::to_string(h.channelCount) + " channels, the list has " +
                    std::to_string(walked) + " records",
                at(L::kHdrChannelCount, std::nullopt, std::to_string(walked)));
        }
        if (legacyStride_ && h.metaPtr == L::kLegacyListOffset) {
            f_.dialect_ |= dialect::kConverterLegacy;
            add(code::kLegacyLayout, "legacy converter layout: channel list at 0x1336 with extended records",
                at(h.metaPtr));
        }
    }

    void parseRecord(std::uint64_t off) {
        LdChannel c;
        LdChannelEncoding& e = c.enc;
        const auto raw = b_.span(off, L::kRecordSize);
        std::copy(raw.begin(), raw.end(), e.raw.begin());
        e.recordOffset = off;
        e.prevPtr = b_.u32(off + L::kRecPrev);
        e.nextPtr = b_.u32(off + L::kRecNext);
        e.dataOffset = b_.u32(off + L::kRecData);
        e.storedCount = b_.u32(off + L::kRecCount);
        e.counter = b_.u16(off + L::kRecCounter);
        e.typeCode = b_.u16(off + L::kRecTypeCode);
        e.bytesPerSample = b_.u16(off + L::kRecWidth);
        e.rateHz = b_.u16(off + L::kRecRate);
        e.shift = b_.i16(off + L::kRecShift);
        e.mul = b_.i16(off + L::kRecMul);
        e.scale = b_.i16(off + L::kRecScale);
        e.dec = b_.i16(off + L::kRecDec);
        e.rawMax = b_.i32(off + L::kRecRawMax);
        e.rawMin = b_.i32(off + L::kRecRawMin);
        e.rangeA = b_.f32(off + L::kRecRangeA);
        e.rangeB = b_.f32(off + L::kRecRangeB);
        e.dpsByte = b_.u8(off + L::kRecDps);
        e.interpByte = b_.u8(off + L::kRecInterp);
        e.gapPtr = b_.u32(off + L::kRecGapPtr);
        e.auxPtr = b_.u32(off + L::kRecAuxPtr);

        const FieldText name = text(off + L::kRecName, L::kNameWidth);
        c.name32 = name.text;
        const std::optional<std::string> chName = c.name32;

        // M1 terminator (S-1 D4, 05 section 3.4): the last record of the list and all zero (rate 0, count 0,
        // type pair 0/0, empty name); not a channel, but counted in the header. Any other record with rate 0
        // (a named, never-logged channel included) cannot be timed and is skipped as ld.zero_rate.
        if (e.nextPtr == 0 && e.rateHz == 0 && e.storedCount == 0 && e.typeCode == 0 &&
            e.bytesPerSample == 0 && c.name32.empty()) {
            aggregate(code::kTerminatorEntry, "all-zero last record is the list terminator", std::nullopt,
                      off);
            return;
        }
        if (e.rateHz == 0) {
            add(code::kZeroRate, "channel '" + c.name32 + "' has rate 0; skipped", at(off, chName));
            return;
        }
        const TypeResolution tr = resolveType(e.typeCode, e.bytesPerSample);
        const std::string pair =
            "(" + std::to_string(e.typeCode) + ", " + std::to_string(e.bytesPerSample) + ")";
        if (!tr.type) {
            add(code::kUnknownDtype,
                "channel '" + c.name32 + "' has type pair " + pair + "; not decodable, skipped",
                at(off, chName, pair));
            return;
        }
        if (tr.unknown) {
            add(code::kUnknownDtype,
                "channel '" + c.name32 + "' has type pair " + pair + "; decoded as a raw signed integer",
                at(off, chName, pair));
            c.flags |= channel_flag::kDtypeGuessed;
        } else if (tr.guessed) {
            add(code::kDtypeGuessed, "channel '" + c.name32 + "' type pair " + pair + " decoded by a guess",
                at(off, chName, pair));
            c.flags |= channel_flag::kDtypeGuessed;
        }
        if (tr.bitfield) {
            c.flags |= channel_flag::kStatusBits;
        }
        c.sampleType = *tr.type;
        if (!name.terminated) {
            aggregate(code::kNameUnterminated, "32-byte name field without NUL (possible truncation)", chName,
                      off);
        }

        // Samples that fit (64-bit arithmetic: data_ptr + count * width can exceed 32 bits).
        const std::uint64_t width = sampleWidth(c.sampleType);
        std::uint64_t n = e.storedCount;
        if (n != 0) {
            const std::uint64_t avail = e.dataOffset <= b_.size() ? (b_.size() - e.dataOffset) / width : 0;
            if (n > avail) {
                add(code::kDataOutOfRange,
                    "channel '" + c.name32 + "' declares " + std::to_string(n) + " samples, " +
                        std::to_string(avail) + " fit in the file; clamped",
                    at(off, chName, std::to_string(avail)));
                n = avail;
            }
        } else {
            aggregate(code::kEmptyChannel, "channel has zero samples", chName, off);
        }
        c.sampleCount = n;

        // Scaling (05 section 3.6).
        c.scaling = makeScalingPlan(c.sampleType, e.shift, e.mul, e.scale, e.dec, f_.options_.shiftOrder);
        if (e.scale == 0) {
            aggregate(code::kZeroScale, "scale 0 treated as 1", chName, off);
        }
        if (e.mul == 0) {
            add(code::kZeroMul, "channel '" + c.name32 + "' has mul 0; treated as 1", at(off, chName));
        }
        if (e.scale < 0 || e.mul < 0) {
            aggregate(code::kNegativeScaling, "scale or mul is negative", chName, off);
        }
        if (e.shift != 0) {
            if (!shiftLogged_) {
                shiftLogged_ = true;
                add(code::kShiftNonzero, "channels with shift != 0 present (first named)", at(off, chName));
            }
            if (e.mul != 0 && e.mul != 1) {
                const std::string order(shiftOrderName(f_.options_.shiftOrder));
                add(code::kShiftMulOrder,
                    "channel '" + c.name32 + "' has shift " + std::to_string(e.shift) + " and mul " +
                        std::to_string(e.mul) + "; decoded " + order,
                    at(off, chName, order));
            }
        }
        const bool isFloat = isFloatSample(c.sampleType);
        if (isFloat && !c.scaling.identity) {
            add(code::kFloatScalingNonidentity,
                "float channel '" + c.name32 + "' has non-identity scaling; applied", at(off, chName));
        }

        // Storage dtype and kind (05 section 3.5).
        if (isFloat) {
            c.dtype = c.sampleType == SampleType::F64 ? store::Dtype::F64 : store::Dtype::F32;
            c.kind = store::SampleKind::Real;
        } else if (c.scaling.identity && c.sampleType != SampleType::I64 && c.sampleType != SampleType::U32) {
            c.dtype = store::Dtype::I32;
            c.kind = tr.bitfield ? store::SampleKind::Bitfield : store::SampleKind::Integer;
        } else if (c.scaling.identity) {
            // 8-byte integers and unsigned 32-bit values do not fit I32; F64 holds both exactly up to 2^53.
            c.dtype = store::Dtype::F64;
            c.kind = tr.bitfield ? store::SampleKind::Bitfield : store::SampleKind::Integer;
        } else {
            c.dtype = scaledNeedsF64(e, c.sampleType) ? store::Dtype::F64 : store::Dtype::F32;
            c.kind = store::SampleKind::Real;
        }

        unitRule(c, off);

        // Display decimals (05 section 3.6, E-20).
        if (e.dpsByte != 0 || e.dec == 0) {
            c.dps = e.dpsByte;
        } else {
            c.dps = std::max<int>(0, e.dec);
            aggregate(code::kDpsFromDec, "display decimals byte 0; dps taken from dec_places", chName, off);
        }

        // Display range (Q-W25): 0/0 means none; either slot order.
        if (!(e.rangeA == 0.0f && e.rangeB == 0.0f) && std::isfinite(e.rangeA) && std::isfinite(e.rangeB)) {
            c.displayRange = std::make_pair(std::min(e.rangeA, e.rangeB), std::max(e.rangeA, e.rangeB));
            c.flags |= channel_flag::kHasDisplayRange;
        }

        c.vendorId = std::to_string(e.counter);
        parseGapBlock(c);
        parseAuxBlock(c);
        c.loggedName = c.longName.empty() ? c.name32 : c.longName;

        if (c.name32 == "Beacon (Internal)" && e.typeCode == L::kTypeInt3 && e.bytesPerSample == 2 &&
            e.rateHz == 1) {
            add(code::kBeaconChannel, "converter beacon channel kept as an ordinary int16 channel",
                at(off, chName));
        }
        c.index = static_cast<std::uint32_t>(f_.channels_.size());
        f_.channels_.push_back(std::move(c));
    }

    // Unit rule (05 section 3.4, port specification R5).
    void unitRule(LdChannel& c, std::uint64_t off) {
        const bool cSeries = cSeriesRecords_;
        const auto field20 = b_.span(off + L::kRecShortName, L::kLongUnitWidth);
        const bool nulInFirst8 = std::find(field20.begin(), field20.begin() + L::kShortNameWidth,
                                           std::byte{0}) != field20.begin() + L::kShortNameWidth;
        const std::optional<std::string> chName = c.name32;
        if (!cSeries && !nulInFirst8) {
            c.unit = text(off + L::kRecShortName, L::kLongUnitWidth, chName).text; // (a) one 20-byte unit
            return;
        }
        const std::string unit48 = text(off + L::kRecUnit, L::kUnitWidth, chName).text;
        const std::string s40 = text(off + L::kRecShortName, L::kShortNameWidth, chName).text;
        if (!unit48.empty()) { // (b) C-series: short name + unit
            c.shortName = s40;
            c.unit = unit48;
            return;
        }
        if (s40.empty()) {
            return;
        }
        if (cSeries && !(f_.options_.unitResolves && f_.options_.unitResolves(s40))) {
            c.shortName = s40;
            aggregate(code::kUnitFromShortNameCandidate,
                      "short name without unit does not resolve as a unit; kept as the short name", chName,
                      off);
            return;
        }
        c.unit = s40; // (c) the unit sits in the short-name field
        c.flags |= channel_flag::kUnitFromShortName;
        aggregate(code::kUnitFromShortName, "unit taken from the 8-byte field at 0x40", chName, off);
    }

    // ---- R6 gap block -> sorted, merged, clamped invalid intervals ----------------------------------------
    // The events of one block as written: (entry, exit) pairs in file order, kOpenRun as the exit of an entry
    // without one. A block is parsed once however many records name it, and every file has two budgets: the
    // events parsed and the intervals materialised over all channels are each at most size / 6 (one 6-byte
    // event each), which an honest file never reaches (no file of the corpus shares a gap block). A block
    // shared by many records therefore costs O(file), not O(records x events).
    struct GapPairs {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
        bool unpaired = false;
        bool shiftedHint = false; // a reversed pair (entry == exit + 1) or an empty pair (entry == exit)
    };
    static constexpr std::uint64_t kOpenRun = std::numeric_limits<std::uint64_t>::max();

    // nullptr when the block is ignored (its events do not fit the remaining budget).
    std::shared_ptr<const GapPairs> gapPairs(std::uint64_t g, const std::optional<std::string>& chName) {
        if (const auto it = gapMemo_.find(g); it != gapMemo_.end()) {
            aggregate(code::kGapBlockShared, "several channel records name the same gap block; parsed once",
                      chName, g);
            return it->second;
        }
        std::uint64_t events = b_.u32(g + L::kGapCount);
        const std::uint64_t arr = b_.u32(g + L::kGapArrayPtr);
        const std::uint64_t avail =
            (arr >= L::kHeaderSize && arr <= b_.size()) ? (b_.size() - arr) / L::kGapEventSize : 0;
        if (events > avail) {
            add(code::kGapBlockMalformed,
                "gap block declares " + std::to_string(events) + " events, " + std::to_string(avail) + " fit",
                at(g, chName));
            events = avail;
        }
        if (events > gapEventBudget_) {
            add(code::kGapBlockMalformed,
                "gap block of " + std::to_string(events) +
                    " events exceeds the file's gap budget (each event of the file once); ignored",
                at(g, chName));
            gapMemo_.emplace(g, nullptr);
            return nullptr;
        }
        gapEventBudget_ -= events;
        auto out = std::make_shared<GapPairs>();
        bool open = false;
        std::uint64_t openAt = 0;
        bool badKind = false;
        for (std::uint64_t i = 0; i < events; ++i) {
            const std::uint64_t o = arr + i * L::kGapEventSize;
            const std::uint16_t kind = b_.u16(o);
            const std::uint64_t idx = b_.u32(o + 2);
            if (kind == L::kGapEnter) {
                if (open) {
                    out->unpaired = true; // entry while already invalid: the run started earlier
                } else {
                    open = true;
                    openAt = idx;
                }
            } else if (kind == L::kGapExit) {
                if (!open) {
                    out->unpaired = true; // exit without entry: nothing to close
                } else {
                    out->pairs.emplace_back(openAt, idx);
                    out->shiftedHint = out->shiftedHint || openAt == idx || openAt == idx + 1;
                    open = false;
                }
            } else {
                badKind = true;
            }
        }
        if (open) {
            out->unpaired = true;
            out->pairs.emplace_back(openAt, kOpenRun);
        }
        if (badKind) {
            add(code::kGapBlockMalformed, "gap event of a kind other than 0/1 ignored", at(g, chName));
        }
        std::shared_ptr<const GapPairs> block = std::move(out);
        gapMemo_.emplace(g, block);
        return block;
    }

    void parseGapBlock(LdChannel& c) {
        const LdChannelEncoding& e = c.enc;
        if (e.gapPtr == 0) {
            return;
        }
        const std::optional<std::string> chName = c.name32;
        const std::uint64_t g = e.gapPtr;
        if (g < L::kHeaderSize || !b_.fits(g, L::kGapHeaderSize)) {
            add(code::kGapBlockMalformed, "gap block at " + hex(g) + " does not fit in the file; ignored",
                at(g, chName));
            return;
        }
        if (b_.u16(g + L::kGapRate) != e.rateHz) {
            aggregate(code::kGapRateMismatch, "gap block rate differs from the channel rate", chName, g);
        }
        const auto block = gapPairs(g, chName);
        if (!block) {
            return;
        }
        if (block->pairs.size() > gapIntervalBudget_) {
            add(code::kGapBlockMalformed,
                "the file's gap budget is spent (gap blocks shared by many records); gap block ignored for "
                "this channel",
                at(g, chName));
            return;
        }
        gapIntervalBudget_ -= block->pairs.size();
        if (block->unpaired) {
            add(code::kGapUnpaired, "unpaired gap entry/exit (an open entry runs to the end of the data)",
                at(g, chName));
        }
        // Pair conventions (port specification section 1.8): the converter's C++ writer and M1 files write
        // (first invalid, first valid again) = [entry, exit); its Python writer wrote (first invalid + 1,
        // last invalid), which gives reversed pairs for one invalid sample and empty pairs for two. As
        // written, a reversed pair is read as [exit, entry), which is that one sample; ld.gapConvention
        // "shifted" reads every pair as [entry - 1, exit + 1).
        const bool shifted = f_.options_.gapConvention == GapConvention::Shifted;
        if (shifted) {
            aggregate(
                code::kGapConventionShifted,
                "gap pairs read with the shifted convention [entry - 1, exit + 1) (option ld.gapConvention)",
                chName, g);
        } else if (block->shiftedHint) {
            aggregate(
                code::kGapConventionShifted,
                "gap pairs look like the shifted convention (entry = first invalid + 1, exit = last "
                "invalid): read as written, a reversed pair as [exit, entry); the option ld.gapConvention "
                "\"shifted\" reads [entry - 1, exit + 1)",
                chName, g);
        }
        const std::uint64_t stored = e.storedCount;
        bool unsorted = false;
        bool outOfRange = false;
        std::vector<LdInterval> valid;
        valid.reserve(block->pairs.size());
        for (const auto& [entry, exit] : block->pairs) {
            LdInterval r;
            if (exit == kOpenRun) {
                r = {shifted && entry > 0 ? entry - 1 : entry, std::max(stored, entry)};
            } else if (shifted) {
                r = {entry > 0 ? entry - 1 : 0, exit + 1};
            } else {
                r = {entry, exit};
            }
            if (r.end < r.begin) {
                unsorted = true; // reversed pair: the samples between the two indexes
                std::swap(r.begin, r.end);
            }
            if (!valid.empty() && r.begin < valid.back().end) {
                unsorted = true;
            }
            if (r.begin > stored || r.end > stored) {
                outOfRange = true;
            }
            r.begin = std::min(r.begin, c.sampleCount);
            r.end = std::min(r.end, c.sampleCount);
            if (r.end > r.begin) {
                valid.push_back(r);
            }
        }
        std::sort(valid.begin(), valid.end(), [](const LdInterval& a, const LdInterval& b) {
            return a.begin < b.begin || (a.begin == b.begin && a.end < b.end);
        });
        for (const LdInterval& r : valid) {
            if (!c.invalid.empty() && r.begin <= c.invalid.back().end) {
                c.invalid.back().end = std::max(c.invalid.back().end, r.end);
            } else {
                c.invalid.push_back(r);
            }
        }
        if (unsorted) {
            add(code::kGapUnsorted, "gap intervals unsorted, overlapping or reversed; merged", at(g, chName));
        }
        if (outOfRange) {
            add(code::kGapOutOfRange, "gap index beyond the sample count; clamped", at(g, chName));
        }
    }

    // ---- R7 aux block: enumeration reference and long name
    // ---------------------------------------------------
    bool isArgType(std::uint64_t pos) const {
        if (!b_.fits(pos, 4)) {
            return false;
        }
        const std::uint32_t t = b_.u32(pos);
        return t == L::kAuxArgLongName || t == L::kAuxArgEnumRef;
    }

    void parseAuxBlock(LdChannel& c) {
        const std::uint64_t a = c.enc.auxPtr;
        if (a == 0) {
            return;
        }
        const std::optional<std::string> chName = c.name32;
        if (a < L::kHeaderSize || !b_.fits(a, 6)) {
            add(code::kAuxOutOfRange, "aux block at " + hex(a) + " is outside the file", at(a, chName));
            return;
        }
        const std::uint64_t content = b_.u32(a + L::kAuxContentPtr);
        if (content < L::kHeaderSize || !b_.fits(content, 8)) {
            add(code::kAuxOutOfRange, "aux content pointer " + hex(content) + " is outside the file",
                at(a, chName));
            return;
        }
        if (b_.u32(content) != L::kAuxContentMarker) {
            add(code::kAuxUnknownArg, "aux content marker " + std::to_string(b_.u32(content)) + " is not 1",
                at(content, chName));
            return;
        }
        std::uint32_t argc = b_.u32(content + 4);
        if (argc > kAuxMaxArgs) {
            add(code::kAuxOutOfRange,
                "aux block declares " + std::to_string(argc) + " arguments; the first " +
                    std::to_string(kAuxMaxArgs) + " are read",
                at(content, chName));
            argc = kAuxMaxArgs;
        }
        std::uint64_t pos = content + 8;
        for (std::uint32_t k = 0; k < argc; ++k) {
            if (!b_.fits(pos, L::kAuxArgHeaderSize)) {
                add(code::kAuxOutOfRange, "aux argument beyond the file", at(pos, chName));
                return;
            }
            const std::uint32_t type = b_.u32(pos);
            const std::uint16_t len = b_.u16(pos + 6);
            // Per-file budget: the aux bytes read over all records are at most the file size, so records
            // sharing one aux content cost O(file) (each argument is read at most once per record).
            const std::uint64_t cost = L::kAuxArgHeaderSize + std::uint64_t{len};
            if (cost > auxBudget_) {
                if (!auxBudgetLogged_) {
                    auxBudgetLogged_ = true;
                    add(code::kAuxOutOfRange,
                        "the file's aux budget is spent (aux blocks shared by many records); the aux blocks "
                        "of "
                        "this and later records are not read further",
                        at(pos, chName));
                }
                return;
            }
            auxBudget_ -= cost;
            std::uint64_t next = pos + L::kAuxArgHeaderSize + len;
            if (type == L::kAuxArgEnumRef) {
                if (len != L::kEnumIndexSize) {
                    add(code::kAuxUnknownArg, "enumeration reference with index size " + std::to_string(len),
                        at(pos, chName));
                    return;
                }
                if (!b_.fits(pos + L::kAuxArgHeaderSize, len)) {
                    add(code::kAuxOutOfRange, "enumeration reference beyond the file", at(pos, chName));
                    return;
                }
                c.enumRef = b_.u16(pos + L::kAuxArgHeaderSize);
            } else if (type == L::kAuxArgLongName) {
                if (len == 0) {
                    add(code::kAuxUnknownArg, "long name of length 0", at(pos, chName));
                    return;
                }
                if (!b_.fits(pos + L::kAuxArgHeaderSize, len)) {
                    add(code::kAuxOutOfRange, "long name beyond the file", at(pos, chName));
                    return;
                }
                c.longName = text(pos + L::kAuxArgHeaderSize, len, chName).text;
                // The converter writes one extra 0 byte after the long name; M1 files do not (E-17).
                if (k + 1 < argc && !isArgType(next) && b_.u8(next) == 0 && isArgType(next + 1)) {
                    ++next;
                    c.longNamePadByte = true;
                }
            } else {
                add(code::kAuxUnknownArg, "aux argument of type " + std::to_string(type) + "; scan stopped",
                    at(pos, chName, std::to_string(type)));
                return;
            }
            pos = next;
        }
    }

    // ---- enumeration resolution (by index, never by position) ---------------------------------------------
    // Returns false when the scan stopped because its work budget was spent.
    bool scanEnumTables(const std::vector<bool>& wanted, std::size_t wantedCount) {
        // Fallback when the 0x690 pointer is 0 (05 section 3.9): the region between the last channel record
        // and the first sample array, searched for the table grammar (u32 2, u16 count, u16 2, u16 index, u32
        // 3). Only tables that a channel references are kept, and the scan stops once all of them are found.
        // A candidate is first checked without decoding labels and must stay inside the region with labels of
        // at most 1 KiB; the bytes examined are bounded by a budget proportional to the region, so a crafted
        // region cannot make the probe quadratic.
        std::uint64_t lo = 0;
        std::uint64_t hi = b_.size();
        for (const LdChannel& c : f_.channels_) {
            lo = std::max(lo, c.enc.recordOffset + L::kRecordSize);
            if (c.sampleCount != 0) {
                hi = std::min(hi, c.enc.dataOffset);
            }
        }
        if (hi <= lo || wantedCount == 0) {
            return true;
        }
        hi = std::min(hi, lo + kEnumScanWindow);
        std::uint64_t budget = 4 * (hi - lo) + kEnumScanSlack;
        const ScanLimits limits{hi, kEnumScanMaxItem, &budget};
        std::vector<bool> found(wanted.size(), false);
        std::size_t foundCount = 0;
        for (std::uint64_t p = lo; p + 14 <= hi && foundCount < wantedCount; ++p) {
            if (!spend(&limits, 1)) {
                return false;
            }
            if (b_.u32(p) != L::kEnumTableMarker || b_.u16(p + 6) != L::kEnumIndexSize) {
                continue;
            }
            const std::uint16_t count = b_.u16(p + 4);
            if (count == 0 || count > 500 || b_.u32(p + 10) != L::kEnumValueMarker) {
                continue;
            }
            const std::uint16_t index = b_.u16(p + 8);
            LdEnumTable table;
            std::uint64_t pos = p;
            // The scan reuses the strict grammar; a table that does not parse is not a table.
            if (!parseEnumTable(pos, table, &limits).empty()) {
                if (budget == 0) {
                    return false;
                }
                continue;
            }
            if (wanted[index] && !found[index]) {
                found[index] = true;
                ++foundCount;
                addEnumTable(std::move(table));
            }
            p = pos - 1;
        }
        return true;
    }

    void resolveEnums() {
        std::vector<bool> wanted(std::size_t{1} << 16, false);
        std::size_t wantedCount = 0;
        for (const LdChannel& c : f_.channels_) {
            if (c.enumRef && !wanted[*c.enumRef]) {
                wanted[*c.enumRef] = true;
                ++wantedCount;
            }
        }
        if (wantedCount == 0) {
            return;
        }
        bool scanned = false;
        bool scanComplete = true;
        if (f_.header_.enumPtr == 0 && f_.options_.enumScanFallback) {
            scanComplete = scanEnumTables(wanted, wantedCount);
            scanned = true;
        }
        bool scanFailed = false;
        for (LdChannel& c : f_.channels_) {
            if (!c.enumRef) {
                continue;
            }
            const LdEnumTable* t = f_.findEnumTable(*c.enumRef);
            if (t == nullptr) {
                scanFailed = scanFailed || scanned;
                add(code::kEnumRefMissing,
                    "channel '" + c.name32 + "' references enumeration table " + std::to_string(*c.enumRef) +
                        ", which does not exist",
                    at(c.enc.recordOffset, c.name32, std::to_string(*c.enumRef)));
                continue;
            }
            if (c.dtype == store::Dtype::I32) {
                c.enumResolved = true;
                c.kind = store::SampleKind::Enum;
            } else {
                // Labels of a scaled or float channel would name raw values the channel does not show (the
                // DBC-style sentinels of converter outputs); the channel stays numeric and the table unused.
                const std::string why = isFloatSample(c.sampleType) ? "float"
                                        : !c.scaling.identity       ? "scaled"
                                                                    : "wide integer";
                add(code::kEnumRefIgnored,
                    "channel '" + c.name32 + "' references enumeration table " + std::to_string(*c.enumRef) +
                        " but is " + why + "; labels not applied",
                    at(c.enc.recordOffset, c.name32, why));
            }
        }
        if (scanFailed) {
            add(code::kEnumScanFailed,
                scanComplete ? "no enumeration block pointer and the scan did not find every referenced table"
                             : "no enumeration block pointer and the scan stopped at its work budget before "
                               "finding every referenced table",
                at(L::kHdrEnumPtr));
        }
    }

    // ---- interpolation, duplicate names ------------------------------------------------------------------
    void finishChannels() {
        const bool cSeries = cSeriesRecords_;
        for (LdChannel& c : f_.channels_) {
            const store::Interp kindDefault = store::Resampler::defaultFor(c.kind);
            switch (c.enc.interpByte) {
            case L::kInterpPrevious:
                c.interp = store::Interp::Previous;
                break;
            case L::kInterpNext:
                c.interp = store::Interp::Next;
                break;
            case L::kInterpLinear:
                c.interp = store::Interp::Linear;
                break;
            case L::kInterpClosest:
                if (cSeries) {
                    c.interp = kindDefault; // C-series write 0 for "unset" (to verify, 05 section 3.4)
                    aggregate(code::kInterpDefault, "interpolation byte 0 on a C-series file; kind default",
                              c.name32, c.enc.recordOffset);
                } else {
                    c.interp = store::Interp::Closest;
                }
                break;
            default:
                c.interp = kindDefault;
                aggregate(code::kInterpDefault,
                          "interpolation byte " + std::to_string(c.enc.interpByte) + "; kind default",
                          c.name32, c.enc.recordOffset);
                break;
            }
            if (c.kind == store::SampleKind::Enum) {
                c.interp = store::Interp::Previous;
            }
        }
        // Duplicate names: the later one gets " (2)", " (3)", ... (05 section 3.4).
        std::unordered_set<std::string> taken;
        for (const LdChannel& c : f_.channels_) {
            taken.insert(c.loggedName);
        }
        std::unordered_set<std::string> seen;
        // The next suffix to try per base name, so N channels of one name cost O(N), not O(N^2).
        std::unordered_map<std::string, int> nextSuffix;
        for (LdChannel& c : f_.channels_) {
            if (seen.insert(c.loggedName).second) {
                continue;
            }
            const std::string base = c.loggedName;
            int& k = nextSuffix.try_emplace(base, 2).first->second;
            std::string candidate;
            for (;; ++k) {
                candidate = base;
                candidate += " (";
                candidate += std::to_string(k);
                candidate += ')';
                if (taken.count(candidate) == 0) {
                    break;
                }
            }
            ++k;
            std::string message = "duplicate channel name '";
            message += base;
            message += "' renamed '";
            message += candidate;
            message += '\'';
            add(code::kDuplicateName, std::move(message), at(c.enc.recordOffset, candidate, base));
            c.loggedName = candidate;
            taken.insert(candidate);
            seen.insert(candidate);
        }
    }

    void finishFile() {
        Micros duration = 0;
        std::optional<std::uint64_t> lowestData;
        for (const LdChannel& c : f_.channels_) {
            duration = std::max(duration, c.rate().timeAt(static_cast<SampleIdx>(c.sampleCount), 0));
            if (c.sampleCount != 0 && (!lowestData || c.enc.dataOffset < *lowestData)) {
                lowestData = c.enc.dataOffset;
            }
        }
        f_.durationUs_ = duration;
        if (lowestData && *lowestData != f_.header_.dataPtr) {
            add(code::kDataPtrMismatch,
                "header data_ptr " + hex(f_.header_.dataPtr) + " differs from the lowest sample array " +
                    hex(*lowestData),
                at(L::kHdrDataPtr));
        }
        f_.stringEncoding_ = sawUtf8_ && sawCp1252_ ? "mixed"
                             : sawUtf8_             ? "utf-8"
                             : sawCp1252_           ? "cp1252"
                                                    : "ascii";
        add(code::kStringEncoding, "strings decoded as " + f_.stringEncoding_,
            at(0, std::nullopt, f_.stringEncoding_));
    }

    LdFile& f_;
    Bytes b_;
    std::map<std::string, Aggregate> aggregates_;
    std::vector<std::string> aggregateOrder_;
    bool sawUtf8_ = false;
    bool sawCp1252_ = false;
    bool shiftLogged_ = false;
    bool legacyStride_ = false;
    bool cSeriesRecords_ = false; // logger record layout (u16 0x040 == 0, event record beyond 0x10000)
    // Per-file work budgets of the gap and aux blocks (set before the walk) and the parsed gap blocks.
    std::uint64_t gapEventBudget_ = 0;
    std::uint64_t gapIntervalBudget_ = 0;
    std::uint64_t auxBudget_ = 0;
    bool auxBudgetLogged_ = false;
    std::unordered_map<std::uint64_t, std::shared_ptr<const GapPairs>> gapMemo_;
};

// ------------------------------------------------------------------------------------------------------------
namespace {

Result<std::shared_ptr<const LdFile>> finish(std::shared_ptr<LdFile> file, LdParser& parser) {
    if (auto r = parser.run(); !r) {
        return Failure{r.error()};
    }
    return std::shared_ptr<const LdFile>(std::move(file));
}

} // namespace

Result<std::shared_ptr<const LdFile>> LdFile::open(const std::filesystem::path& path, LdOptions options) {
    auto mapped = MappedFile::open(path);
    if (!mapped) {
        return Failure{mapped.error()};
    }
    return fromMapped(std::make_shared<const MappedFile>(std::move(*mapped)), std::move(options));
}

Result<std::shared_ptr<const LdFile>> LdFile::fromMapped(std::shared_ptr<const MappedFile> file,
                                                         LdOptions options) {
    if (!file) {
        return fail(ErrorCode::Invalid, "LdFile::fromMapped: no file");
    }
    auto f = std::make_shared<LdFile>(PrivateTag{});
    f->bytes_ = file->bytes();
    f->path_ = file->path();
    f->mapped_ = std::move(file);
    f->options_ = std::move(options);
    LdParser parser(*f);
    // The header walk reads mapped memory outside guarded(); a page fault there is as fatal as for any
    // other reader of a vanishing file. Chunk decodes are guarded (LdChunkProvider).
    return finish(std::move(f), parser);
}

Result<std::shared_ptr<const LdFile>> LdFile::fromBuffer(std::shared_ptr<const std::vector<std::byte>> bytes,
                                                         LdOptions options) {
    if (!bytes) {
        return fail(ErrorCode::Invalid, "LdFile::fromBuffer: no buffer");
    }
    auto f = std::make_shared<LdFile>(PrivateTag{});
    f->bytes_ = std::span<const std::byte>(*bytes);
    f->buffer_ = std::move(bytes);
    f->options_ = std::move(options);
    LdParser parser(*f);
    return finish(std::move(f), parser);
}

const LdEnumTable* LdFile::findEnumTable(std::uint16_t index) const noexcept {
    const auto it = enumByIndex_.find(index);
    return it == enumByIndex_.end() ? nullptr : &enums_[it->second];
}

Result<void> LdFile::decode(std::uint32_t ch, std::uint64_t first, std::uint64_t count, store::Dtype outDtype,
                            std::span<std::byte> out) const {
    if (ch >= channels_.size()) {
        return fail(ErrorCode::NotFound, "LdFile::decode: no channel " + std::to_string(ch));
    }
    const LdChannel& c = channels_[ch];
    if (first > c.sampleCount || count > c.sampleCount - first) {
        return fail(ErrorCode::Invalid, "LdFile::decode: range beyond the channel's " +
                                            std::to_string(c.sampleCount) + " samples");
    }
    if (outDtype == store::Dtype::I64 ||
        (outDtype == store::Dtype::I32 &&
         (!c.scaling.identity || isFloatSample(c.sampleType) || c.sampleType == SampleType::I64 ||
          c.sampleType == SampleType::U32))) {
        return fail(ErrorCode::Invalid, "LdFile::decode: output dtype cannot hold this channel");
    }
    if (out.size() < count * store::dtypeSize(outDtype)) {
        return fail(ErrorCode::Invalid, "LdFile::decode: output buffer too small");
    }
    if (count == 0) {
        return ok();
    }
    DecodeJob job;
    job.src = bytes_.data() + c.enc.dataOffset + first * sampleWidth(c.sampleType);
    job.count = count;
    job.type = c.sampleType;
    job.out = outDtype;
    job.dst = out.data();
    job.plan = &c.scaling;
    if (mapped_) {
        return mapped_->guarded(&runDecodeJob, &job);
    }
    runDecodeJob(&job);
    return ok();
}

Result<std::vector<double>> LdFile::decodeAsDouble(std::uint32_t ch, std::uint64_t first,
                                                   std::uint64_t count) const {
    if (ch >= channels_.size()) {
        return fail(ErrorCode::NotFound, "LdFile::decodeAsDouble: no channel " + std::to_string(ch));
    }
    const store::Dtype dt = channels_[ch].dtype;
    std::vector<std::byte> buf(static_cast<std::size_t>(count * store::dtypeSize(dt)));
    if (auto r = decode(ch, first, count, dt, buf); !r) {
        return Failure{r.error()};
    }
    std::vector<double> out(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < out.size(); ++i) {
        switch (dt) {
        case store::Dtype::F32:
            out[i] = static_cast<double>(loadAs<float>(buf.data() + 4 * i, Endian::Little));
            break;
        case store::Dtype::F64:
            out[i] = loadAs<double>(buf.data() + 8 * i, Endian::Little);
            break;
        case store::Dtype::I32:
            out[i] = static_cast<double>(loadAs<std::int32_t>(buf.data() + 4 * i, Endian::Little));
            break;
        case store::Dtype::I64:
            break;
        }
    }
    return out;
}

Result<std::vector<double>> LdFile::rawValues(std::uint32_t ch, std::uint64_t first,
                                              std::uint64_t count) const {
    if (ch >= channels_.size()) {
        return fail(ErrorCode::NotFound, "LdFile::rawValues: no channel " + std::to_string(ch));
    }
    const LdChannel& c = channels_[ch];
    if (first > c.sampleCount || count > c.sampleCount - first) {
        return fail(ErrorCode::Invalid, "LdFile::rawValues: range beyond the channel");
    }
    std::vector<double> out(static_cast<std::size_t>(count));
    if (count == 0) {
        return out;
    }
    const ScalingPlan identity; // identity scaling: stored values as they are
    DecodeJob job;
    job.src = bytes_.data() + c.enc.dataOffset + first * sampleWidth(c.sampleType);
    job.count = count;
    job.type = c.sampleType;
    job.out = store::Dtype::F64;
    job.dst = out.data();
    job.plan = &identity;
    if (mapped_) {
        if (auto r = mapped_->guarded(&runDecodeJob, &job); !r) {
            return Failure{r.error()};
        }
        return out;
    }
    runDecodeJob(&job);
    return out;
}

LdFile::RangeValidity LdFile::validity(std::uint32_t ch, std::uint64_t first,
                                       std::uint64_t count) const noexcept {
    if (ch >= channels_.size() || count == 0) {
        return RangeValidity::AllValid;
    }
    const std::vector<LdInterval>& iv = channels_[ch].invalid;
    const std::uint64_t last = first + count;
    // First interval that ends after `first`.
    const auto it = std::upper_bound(iv.begin(), iv.end(), first,
                                     [](std::uint64_t v, const LdInterval& r) { return v < r.end; });
    if (it == iv.end() || it->begin >= last) {
        return RangeValidity::AllValid;
    }
    if (it->begin <= first && it->end >= last) {
        return RangeValidity::AllInvalid;
    }
    return RangeValidity::Mixed;
}

void LdFile::clearInvalidBits(std::uint32_t ch, std::uint64_t first, std::uint64_t count,
                              std::span<std::uint64_t> words) const noexcept {
    if (ch >= channels_.size() || count == 0) {
        return;
    }
    const std::vector<LdInterval>& iv = channels_[ch].invalid;
    const std::uint64_t last = first + count;
    auto it = std::upper_bound(iv.begin(), iv.end(), first,
                               [](std::uint64_t v, const LdInterval& r) { return v < r.end; });
    for (; it != iv.end() && it->begin < last; ++it) {
        std::uint64_t b = std::max(it->begin, first) - first;
        const std::uint64_t e = std::min(it->end, last) - first;
        while (b < e) {
            const std::uint64_t w = b >> 6;
            if (w >= words.size()) {
                return;
            }
            const auto bit = static_cast<unsigned>(b & 63u);
            const std::uint64_t n = std::min<std::uint64_t>(64u - bit, e - b);
            const std::uint64_t mask = (n == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << n) - 1u)) << bit;
            words[static_cast<std::size_t>(w)] &= ~mask;
            b += n;
        }
    }
}

void LdFile::clearInvalidBytes(std::uint32_t ch, std::uint64_t first, std::uint64_t count,
                               std::span<std::uint8_t> bytes) const noexcept {
    if (ch >= channels_.size() || count == 0) {
        return;
    }
    const std::vector<LdInterval>& iv = channels_[ch].invalid;
    const std::uint64_t last = first + count;
    auto it = std::upper_bound(iv.begin(), iv.end(), first,
                               [](std::uint64_t v, const LdInterval& r) { return v < r.end; });
    for (; it != iv.end() && it->begin < last; ++it) {
        std::uint64_t b = std::max(it->begin, first) - first;
        const std::uint64_t e = std::min(it->end, last) - first;
        while (b < e) {
            const std::uint64_t k = b >> 3;
            if (k >= bytes.size()) {
                return;
            }
            const auto bit = static_cast<unsigned>(b & 7u);
            const auto n = static_cast<unsigned>(std::min<std::uint64_t>(8u - bit, e - b));
            const unsigned mask = ((1u << n) - 1u) << bit;
            bytes[static_cast<std::size_t>(k)] &= static_cast<std::uint8_t>(~mask);
            b += n;
        }
    }
}

void LdFile::writeInvalidNaN(std::uint32_t ch, std::uint64_t first, std::uint64_t count, store::Dtype dtype,
                             std::span<std::byte> out) const noexcept {
    if (ch >= channels_.size() || count == 0 || !store::isFloatDtype(dtype)) {
        return;
    }
    const std::vector<LdInterval>& iv = channels_[ch].invalid;
    const std::uint64_t last = first + count;
    auto it = std::upper_bound(iv.begin(), iv.end(), first,
                               [](std::uint64_t v, const LdInterval& r) { return v < r.end; });
    for (; it != iv.end() && it->begin < last; ++it) {
        const std::uint64_t b = std::max(it->begin, first) - first;
        const std::uint64_t e = std::min(it->end, last) - first;
        if (e * store::dtypeSize(dtype) > out.size()) {
            return;
        }
        if (dtype == store::Dtype::F32) {
            auto* p = reinterpret_cast<float*>(out.data());
            std::fill(p + b, p + e, std::numeric_limits<float>::quiet_NaN());
        } else {
            auto* p = reinterpret_cast<double*>(out.data());
            std::fill(p + b, p + e, std::numeric_limits<double>::quiet_NaN());
        }
    }
}

bool LdFile::validAt(std::uint32_t ch, std::uint64_t i) const noexcept {
    if (ch >= channels_.size() || i >= channels_[ch].sampleCount) {
        return false;
    }
    return validity(ch, i, 1) == RangeValidity::AllValid;
}

} // namespace esses::formats::ld
