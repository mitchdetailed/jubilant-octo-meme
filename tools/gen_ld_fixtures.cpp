// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-gen-fixtures (08 section 1, 05 section 9.1, D-033, D-040, D-041): writes the synthetic .ld (+ .ldx)
// fixture families through the motec.ld IWriter (an in-memory DataSource with an LdSourceEncoding per
// channel), then, unless --inputs-only, runs `esses-cli fixture add --oracle ref-decode` so the expected
// channel values come from tools/ld_ref_decode.py and never from the Esses reader (D-041).
//
// Families: every (dtype_a, dtype) pair (dtype_a 0x00, 0x03-0x08 x widths 1, 2, 4, 8), multi-rate, shift != 0
// with mul != 1 in both orders, scaling edge cases, aux-chained records (long names; enumeration references,
// the 212-byte record + aux case), gap blocks, truncated names, a header channel count off by one, device
// events, a header-only file, a zero-rate record, a truncated data array, non-finite floats, the dialects of
// docs/formats/ld.md (m1, c-series, converter-legacy, pc-layout) and the public sim/converter writers'
// fingerprints (sim-gotzl, sim-mi2), and synthetic-acc-dialect (05 section 9.1, 08 section 6.7). What the
// IWriter cannot express (a wrong header count, a zero rate, a truncated array, another writer's header
// constants) is applied afterwards as documented byte edits, named in each family's README.md.
//
// Every value comes from integer arithmetic or exact IEEE operations (+, -, *, /, conversions), never from
// libm, so the inputs are bit-identical on every platform and fixture.json's sha256 pins them. Integer
// channels whose samples are invalid (gap blocks) hold the previous valid value on disk; their expected rows
// are marked invalid from the generator's own declaration (fixture-1 oracle.validity "strict"), since the
// oracle does not decode gap blocks.
//
// Usage:
//   esses-gen-fixtures [--out fixtures/motec.ld] [--family NAME ...] [--list]
//   esses-gen-fixtures --inputs-only --out <dir> [--fixtures <dir> --manifest <file>]
//   esses-gen-fixtures --large <file.ld> [--large-mb 500]
#include "cli_common.hpp"
#include "expect_channels.hpp"
#include "process.hpp"

#include <esses/base/config.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/rate.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/ld_importer.hpp>
#include <esses/model/model.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/store/series.hpp>

#include <CLI/CLI11.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(ESSES_OS_WINDOWS)
#include <io.h>

#include <fcntl.h>
#endif

namespace {

using namespace esses;
namespace fs = std::filesystem;
using formats::LdEncoding;
using model::Detail;
using model::Provenance;

constexpr std::uint16_t kInt0 = 0x00;
constexpr std::uint16_t kInt3 = 0x03;
constexpr std::uint16_t kRaw4 = 0x04;
constexpr std::uint16_t kInt5 = 0x05;
constexpr std::uint16_t kInt6 = 0x06;
constexpr std::uint16_t kFloat = 0x07;
constexpr std::uint16_t kDouble = 0x08;
constexpr std::uint8_t kInterpPrevious = 1;
constexpr std::uint8_t kInterpLinear = 3;
constexpr std::uint16_t kPinnedDeviceVersion = 100; // pc-layout identity version, independent of the release

// ---- the family model ---------------------------------------------------------------------------------

struct GenChannel {
    std::string name;
    LdEncoding enc;
    std::vector<double> values; // engineering values in the vendor unit
    std::vector<bool> valid;    // empty: every sample valid
    store::SampleKind kind = store::SampleKind::Real;
    std::optional<std::uint32_t> enumTable;
};

struct Family {
    std::string name;
    std::string summary;               // README and fixture.json source
    std::string notes;                 // fixture.json notes: what the family exercises
    std::string dialect = "pc-layout"; // writer option ld.dialect
    std::vector<GenChannel> channels;
    std::vector<Detail> details;
    std::vector<model::Marker> markers; // device events (Logged) and companion markers (Companion)
    std::vector<model::EnumTable> enumTables;
    std::function<void(std::vector<std::byte>&)> patch; // byte edits after the writer
    std::string patchNote;
    std::optional<std::string> ldxText; // a hand-written .ldx replacing the writer's
    std::string importOptions;          // JSON; "" = the default {"ld":{"shiftOrder":"afterMul"}}
    std::vector<std::string> compared;  // oracle subset; empty = every channel
};

LdEncoding encoding(std::string name, std::uint16_t code, std::uint16_t width, std::uint16_t rate) {
    LdEncoding e;
    e.name = std::move(name);
    e.typeCode = code;
    e.width = width;
    e.rateHz = rate;
    return e;
}

bool isFloatPair(std::uint16_t code, std::uint16_t width) {
    return (code == kFloat && (width == 2 || width == 4 || width == 8)) || (code == kDouble && width == 8);
}

GenChannel intChannel(std::string name, std::uint16_t code, std::uint16_t width, std::uint16_t rate,
                      std::vector<double> raws) {
    GenChannel g;
    g.name = name;
    g.enc = encoding(std::move(name), code, width, rate);
    g.enc.interpByte = kInterpPrevious;
    g.values = std::move(raws);
    g.kind = store::SampleKind::Integer;
    return g;
}

GenChannel floatChannel(std::string name, std::uint16_t code, std::uint16_t width, std::uint16_t rate,
                        std::vector<double> values) {
    GenChannel g;
    g.name = name;
    g.enc = encoding(std::move(name), code, width, rate);
    g.enc.interpByte = kInterpLinear;
    g.enc.dpsByte = 3;
    g.values = std::move(values);
    return g;
}

double powerOf10(int n) { // exact for n <= 22
    double p = 1.0;
    for (int i = 0; i < n; ++i) {
        p *= 10.0;
    }
    return p;
}

// The afterMul engineering value of a raw count (the inverse of the writer's quantisation).
double afterMulValue(double raw, std::int16_t shift, std::int16_t mul, std::int16_t scale, std::int16_t dec) {
    const double s = scale == 0 ? 1.0 : static_cast<double>(scale);
    const double m = mul == 0 ? 1.0 : static_cast<double>(mul);
    if (dec >= 0) {
        return raw * m / (s * powerOf10(dec)) + static_cast<double>(shift);
    }
    return raw * powerOf10(-dec) * m / s + static_cast<double>(shift);
}

GenChannel scaledChannel(std::string name, std::uint16_t code, std::uint16_t width, std::uint16_t rate,
                         std::int16_t shift, std::int16_t mul, std::int16_t scale, std::int16_t dec,
                         const std::vector<double>& raws) {
    GenChannel g = isFloatPair(code, width) ? floatChannel(std::move(name), code, width, rate, {})
                                            : intChannel(std::move(name), code, width, rate, {});
    g.kind = store::SampleKind::Real;
    g.enc.interpByte = kInterpLinear;
    g.enc.shift = shift;
    g.enc.mul = mul;
    g.enc.scale = scale;
    g.enc.dec = dec;
    g.enc.dpsByte = static_cast<std::uint8_t>(dec > 0 ? dec : 0);
    for (const double r : raws) {
        g.values.push_back(afterMulValue(r, shift, mul, scale, dec));
    }
    return g;
}

std::vector<double> ramp(std::int64_t n, std::int64_t start, std::int64_t step) {
    std::vector<double> v;
    v.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        v.push_back(static_cast<double>(start + i * step));
    }
    return v;
}

// Extremes of a signed integer of `width` bytes (2^53 - 1 for width 8, so every value is an exact double),
// then a ramp across the range. nonNegative: 0 .. max only (dtype_a 0x04, read unsigned by Esses and signed
// by the oracle; the high bit stays clear so both agree, see the family notes).
std::vector<double> intPattern(std::uint16_t width, bool nonNegative, std::size_t n) {
    const std::int64_t hi =
        width >= 8 ? (std::int64_t{1} << 53) - 1 : (std::int64_t{1} << (8 * width - 1)) - 1;
    const std::int64_t lo = nonNegative ? 0 : (width >= 8 ? -hi : -hi - 1);
    std::vector<double> v = {static_cast<double>(lo),
                             static_cast<double>(lo + 1),
                             static_cast<double>(hi),
                             static_cast<double>(hi - 1),
                             0.0,
                             1.0};
    if (!nonNegative) {
        v.push_back(-1.0);
        v.push_back(-2.0);
    }
    const std::size_t extra = n > v.size() ? n - v.size() : 0;
    const std::int64_t step = (hi - lo) / static_cast<std::int64_t>(extra + 1);
    for (std::size_t k = 1; k <= extra; ++k) {
        v.push_back(static_cast<double>(lo + step * static_cast<std::int64_t>(k)));
    }
    return v;
}

std::vector<double> floatPattern(std::uint16_t code, std::uint16_t width, std::size_t n) {
    std::vector<double> v;
    if (code == kFloat && width == 2) {
        v = {0.0,
             0.5,
             -0.5,
             1.5,
             -2.25,
             1024.0,
             -1024.0,
             65504.0,
             -65504.0,
             0.00006103515625,
             0.000000059604644775390625,
             3.125};
    } else if (code == kFloat && width == 4) {
        v = {0.0,
             -0.0,
             1.5,
             -2.25,
             3.4028234663852886e38,
             -3.4028234663852886e38,
             1.1754943508222875e-38,
             1.401298464324817e-45,
             16777216.0,
             0.1,
             -273.15};
    } else {
        v = {0.0,
             1.5,
             -2.25,
             1.7976931348623157e308,
             -1.7976931348623157e308,
             2.2250738585072014e-308,
             5e-324,
             0.1,
             -33.8688197,
             151.2093,
             9007199254740993.0};
    }
    for (std::size_t k = 0; v.size() < n; ++k) {
        v.push_back(static_cast<double>(k) * 0.25 - 4.0);
    }
    return v;
}

// ---- details, markers, enumeration tables -----------------------------------------------------------------

Detail text(std::string id, std::string value, Provenance prov = Provenance::Logged) {
    return Detail{std::move(id), model::DetailValue(std::move(value)), prov, std::nullopt};
}

std::vector<Detail> baseDetails(const std::string& family) {
    return {text("Log Date", "11/10/2026"),
            text("Log Time", "10:00:00"),
            text("Driver", "Synthetic Driver"),
            text("Vehicle Id", "SYN-01"),
            text("Venue", "Synthetic Circuit"),
            text("Session", "Fixture"),
            text("Short Comment", family.substr(0, 63))};
}

model::Marker companionMarker(model::MarkerClass cls, Micros t, std::string group, std::string name,
                              std::uint32_t flags, std::uint32_t ordinal = 0) {
    return model::makeLoggedMarker(cls, t, std::move(group), std::move(name), {}, flags, ordinal,
                                   Provenance::Companion);
}

model::EnumTable enumTable(std::uint32_t id, std::vector<model::EnumEntry> entries) {
    model::EnumTable t;
    t.id = model::EnumTableId{id};
    t.entries = std::move(entries);
    return t;
}

// ---- byte edits -----------------------------------------------------------------------------------------

std::uint32_t rd32(const std::vector<std::byte>& b, std::size_t off) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
        v = (v << 8) | static_cast<std::uint32_t>(b.at(off + static_cast<std::size_t>(i)));
    }
    return v;
}

std::uint16_t rd16(const std::vector<std::byte>& b, std::size_t off) {
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(b.at(off)) |
                                      (static_cast<std::uint32_t>(b.at(off + 1)) << 8));
}

void wr16(std::vector<std::byte>& b, std::size_t off, std::uint16_t v) {
    b.at(off) = static_cast<std::byte>(v & 0xFF);
    b.at(off + 1) = static_cast<std::byte>(v >> 8);
}

void wr32(std::vector<std::byte>& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.at(off + static_cast<std::size_t>(i)) = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}

// Channel record offsets in list order (header meta pointer at 0x008, next pointer at record + 0x04).
std::vector<std::size_t> recordOffsets(const std::vector<std::byte>& b) {
    std::vector<std::size_t> out;
    std::size_t off = rd32(b, 0x008);
    while (off != 0 && off + 0x7C <= b.size() && out.size() < 65536) {
        out.push_back(off);
        off = rd32(b, off + 0x04);
    }
    return out;
}

void pinDeviceVersion(std::vector<std::byte>& b) {
    wr16(b, 0x052, kPinnedDeviceVersion);
}

// ---- the families ---------------------------------------------------------------------------------------

std::string hex2(std::uint16_t v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned>(v));
    return buf;
}

std::vector<Family> dtypeFamilies() {
    std::vector<Family> out;
    for (const std::uint16_t code : {kInt0, kInt3, kRaw4, kInt5, kInt6, kFloat, kDouble}) {
        for (const std::uint16_t width :
             {std::uint16_t{1}, std::uint16_t{2}, std::uint16_t{4}, std::uint16_t{8}}) {
            Family f;
            f.name = "synthetic-dtype-" + hex2(code) + "-" + std::to_string(width);
            const std::string pair = "(0x" + hex2(code) + ", " + std::to_string(width) + ")";
            f.summary = "synthetic: the (dtype_a, dtype) pair " + pair +
                        " with identity and non-identity scaling, written by esses-gen-fixtures through the "
                        "motec.ld writer";
            f.details = baseDetails(f.name);
            const bool isFloat = isFloatPair(code, width);
            const bool nonNegative = code == kRaw4;
            if (isFloat) {
                f.channels.push_back(
                    floatChannel("Raw " + pair, code, width, 10, floatPattern(code, width, 40)));
                f.channels.push_back(
                    scaledChannel("Scaled " + pair, code, width, 10, 0, 1, 2, 0, ramp(40, -16, 1)));
                f.notes = "float pair: extremes, subnormals and signed zero; the scaled channel has scale 2 "
                          "(ld.float_scaling_nonidentity)";
            } else {
                f.channels.push_back(
                    intChannel("Raw " + pair, code, width, 10, intPattern(width, nonNegative, 40)));
                f.channels.push_back(scaledChannel("Scaled " + pair, code, width, 10, 0, 3, 4, 1,
                                                   nonNegative ? ramp(41, 0, 3) : ramp(41, -60, 3)));
                f.notes =
                    "integer pair: the signed extremes of the width (width 8: +-(2^53 - 1)) and a ramp; the "
                    "scaled channel has mul 3, scale 4, dec 1";
                if (code == kRaw4) {
                    f.notes += "; dtype_a 0x04 is decoded unsigned by Esses and signed by the oracle, so the "
                               "values keep the high bit clear (unsettled: no real file has 0x04, "
                               "docs/formats/ld.md)";
                }
            }
            f.patch = pinDeviceVersion;
            f.patchNote = "device version pinned to 100 at 0x052";
            out.push_back(std::move(f));
        }
    }
    return out;
}

// Values of a periodic triangle wave from integer arithmetic only: period p samples, amplitude a / d.
std::vector<double> triangle(std::size_t n, std::int64_t period, std::int64_t amplitude, double divisor,
                             double offset = 0.0) {
    std::vector<double> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::int64_t k = static_cast<std::int64_t>(i) % period;
        const std::int64_t half = period / 2;
        const std::int64_t tri = k < half ? k : period - k;
        const std::int64_t level = tri * amplitude / std::max<std::int64_t>(half, 1);
        v.push_back(static_cast<double>(level) / divisor + offset);
    }
    return v;
}

Family multirate() {
    Family f;
    f.name = "synthetic-multirate";
    f.summary =
        "synthetic: 14 channels at rates from 1 Hz to 1 kHz (including 3, 7, 33 and 333 Hz, whose periods "
        "are not whole microseconds) with different lengths";
    f.notes =
        "timeAt(i) = floor((2 i 10^6 + rate) / (2 rate)); the layer lasts as long as its longest channel";
    f.details = baseDetails(f.name);
    const std::uint16_t rates[] = {1, 2, 3, 5, 7, 10, 20, 33, 50, 100, 200, 333, 500, 1000};
    for (const std::uint16_t r : rates) {
        const std::size_t seconds = r == 1000 ? 9 : (r == 3 ? 11 : 10);
        const std::size_t n = static_cast<std::size_t>(r) * seconds + (r == 333 ? 1 : 0);
        if (r == 7) {
            f.channels.push_back(
                intChannel("Rate 7 Hz Counter", kInt3, 2, r, ramp(static_cast<std::int64_t>(n), -30, 1)));
        } else {
            f.channels.push_back(floatChannel("Rate " + std::to_string(r) + " Hz", kFloat, 4, r,
                                              triangle(n, std::max<std::int64_t>(r, 2) * 2, 1000, 8.0)));
        }
    }
    f.patch = pinDeviceVersion;
    f.patchNote = "device version pinned to 100 at 0x052";
    return f;
}

std::vector<GenChannel> shiftMulChannels() {
    std::vector<GenChannel> c;
    {
        // The S-1 logger case: raw counts of 0.1 deg C read as deg F (shift 32, mul 9, scale 5, dec 1).
        auto g = scaledChannel("Gbox Oil Temp", kInt3, 2, 2, 32, 9, 5, 1, ramp(60, 240, 13));
        g.enc.unit = "F";
        g.enc.dpsByte = 1;
        c.push_back(std::move(g));
    }
    {
        auto g = scaledChannel("Neg Shift", kInt3, 2, 10, -40, 2, 1, 0, ramp(50, -25, 2));
        g.enc.unit = "C";
        c.push_back(std::move(g));
    }
    {
        auto g = scaledChannel("Neg Mul", kInt5, 4, 10, 10, -3, 2, 1, ramp(50, -1000, 41));
        g.enc.unit = "kPa";
        c.push_back(std::move(g));
    }
    {
        auto g = scaledChannel("Dec Neg", kInt3, 2, 5, 5, 7, 1, -1, ramp(30, -15, 1));
        g.enc.unit = "rpm";
        c.push_back(std::move(g));
    }
    return c;
}

Family shiftMul(bool before) {
    Family f;
    f.name = before ? "synthetic-shift-mul-before" : "synthetic-shift-mul-after";
    f.summary =
        std::string("synthetic: channels with shift != 0 and mul != 1 (including the deg C -> deg F logger "
                    "case) read with ld.shiftOrder ") +
        (before ? "beforeMul" : "afterMul");
    f.notes = std::string("the same input bytes as synthetic-shift-mul-") + (before ? "after" : "before") +
              "; expectations from the oracle's " + (before ? "beforeMul" : "afterMul") +
              " dump; afterMul is the default (D-037)";
    f.details = baseDetails("synthetic-shift-mul");
    f.channels = shiftMulChannels();
    if (before) {
        f.importOptions = R"({"ld":{"shiftOrder":"beforeMul"}})";
    }
    f.patch = pinDeviceVersion;
    f.patchNote = "device version pinned to 100 at 0x052";
    return f;
}

Family scalingEdge() {
    Family f;
    f.name = "synthetic-scaling-edge";
    f.summary =
        "synthetic: scale 0 and mul 0 (each read as 1), a negative scale, dec 5 and dec 7 (F64 storage)";
    f.notes = "ld.zero_scale, ld.zero_mul (Warning), ld.negative_scaling; dec >= 5 keeps F64";
    f.details = baseDetails(f.name);
    f.channels.push_back(scaledChannel("Zero Scale", kInt3, 2, 10, 0, 1, 0, 1, ramp(30, -15, 1)));
    f.channels.push_back(scaledChannel("Zero Mul", kInt3, 2, 10, 2, 0, 2, 0, ramp(30, -15, 1)));
    f.channels.push_back(scaledChannel("Negative Scale", kInt3, 2, 10, 0, 1, -4, 0, ramp(30, -15, 1)));
    {
        auto g = scaledChannel("GPS Latitude", kInt5, 4, 10, 0, 1, 1, 7, ramp(40, -338765432, 12345));
        g.enc.unit = "deg";
        f.channels.push_back(std::move(g));
    }
    f.channels.push_back(scaledChannel("Fine Scale", kInt5, 4, 10, 0, 1, 1, 5, ramp(40, 123456789, -98765)));
    f.patch = pinDeviceVersion;
    f.patchNote = "device version pinned to 100 at 0x052";
    return f;
}

std::vector<model::EnumEntry> severityEntries() {
    return {{-2, "Not Fitted", 0}, {-1, "Unknown", 1}, {0, "Stopped", 0},
            {1, "Cranking", 1},    {2, "Running", 0},  {3, "Overheat", 3}};
}

Family auxLongNames() {
    Family f;
    f.name = "synthetic-aux-long-names";
    f.summary =
        "synthetic: channel names longer than the 32-byte field (an aux long name each), one cut inside a "
        "multi-byte UTF-8 sequence, and a long name followed by an enumeration reference";
    f.notes =
        "aux blocks after the records (record + aux, never a fixed stride); names are read from the aux "
        "block, the 32-byte field holds the cut prefix";
    f.details = baseDetails(f.name);
    f.channels.push_back(floatChannel("A channel name far longer than thirty-two bytes", kFloat, 4, 10,
                                      triangle(50, 20, 100, 4.0)));
    f.channels.push_back(floatChannel("Temp\xC3\xA9rature d'huile bo\xC3\xAEte de vitesses", kFloat, 4, 10,
                                      triangle(50, 10, 80, 2.0, 20.0)));
    {
        auto g = intChannel("Engine State With A Long Descriptive Name", kInt3, 2, 10, ramp(30, -2, 0));
        for (std::size_t i = 0; i < g.values.size(); ++i) {
            const std::int64_t state = static_cast<std::int64_t>(i) / 5 - 2;
            g.values[i] = static_cast<double>(state);
        }
        g.enc.enumRef = std::uint16_t{3};
        g.enumTable = 3;
        g.kind = store::SampleKind::Enum;
        f.channels.push_back(std::move(g));
    }
    f.channels.push_back(intChannel("Short", kInt3, 2, 10, ramp(20, 0, 1)));
    f.enumTables = {enumTable(3, severityEntries())};
    f.patch = pinDeviceVersion;
    f.patchNote = "device version pinned to 100 at 0x052";
    return f;
}

Family auxEnum212() {
    Family f;
    f.name = "synthetic-aux-enum-212";
    f.summary =
        "synthetic: channels whose aux block holds only an enumeration reference, so each record plus its "
        "aux block is exactly 212 bytes (the i3rs 'M1 record' case), and an enumeration block with "
        "severities and the values -1 and -2";
    f.notes = "the oracle reads the 212-byte record's enum fields and scans for the tables; Esses links them "
              "through the aux block (05 section 3.9)";
    f.details = baseDetails(f.name);
    for (std::uint32_t t = 1; t <= 3; ++t) {
        auto g = intChannel("Enum Channel " + std::to_string(t), kInt3, 2, 5, {});
        for (int i = 0; i < 30; ++i) {
            g.values.push_back(static_cast<double>((i % 6) - 2));
        }
        g.enc.enumRef = static_cast<std::uint16_t>(t);
        g.enumTable = t;
        g.kind = store::SampleKind::Enum;
        f.channels.push_back(std::move(g));
    }
    f.enumTables = {enumTable(1, severityEntries()),
                    enumTable(2, {{0, "Off", 0}, {1, "On", 0}, {-1, "Fault", 3}}),
                    enumTable(3, {{-2, "Minus Two", 2},
                                  {-1, "Minus One", 2},
                                  {0, "Zero", 0},
                                  {1, "One", 0},
                                  {2, "Two", 0},
                                  {3, "Three", 1}})};
    f.patch = pinDeviceVersion;
    f.patchNote = "device version pinned to 100 at 0x052";
    return f;
}

std::vector<bool> validMask(std::size_t n, const std::vector<std::pair<std::size_t, std::size_t>>& invalid) {
    std::vector<bool> v(n, true);
    for (const auto& [a, b] : invalid) {
        for (std::size_t i = a; i < b && i < n; ++i) {
            v[i] = false;
        }
    }
    return v;
}

Family gapBlocks() {
    Family f;
    f.name = "synthetic-gap-blocks";
    f.summary =
        "synthetic: gap blocks (invalid sample ranges [entry, exit)) at the start, middle and end of float "
        "and integer channels, adjacent ranges, and a channel invalid throughout";
    f.notes =
        "float gaps are stored as NaN, integer gaps hold the previous valid value (05 section 5 item 5); the "
        "expected validity of integer gaps comes from the generator's declaration (oracle.validity strict)";
    f.details = baseDetails(f.name);
    {
        auto g = floatChannel("Lambda", kFloat, 4, 20, triangle(200, 40, 100, 400.0, 0.75));
        g.valid = validMask(200, {{10, 20}, {90, 100}});
        f.channels.push_back(std::move(g));
    }
    {
        auto g = intChannel("Throttle Raw", kInt3, 2, 10, ramp(100, 0, 7));
        g.valid = validMask(100, {{0, 5}, {95, 100}});
        f.channels.push_back(std::move(g));
    }
    {
        auto g = intChannel("Wheel Count", kInt5, 4, 10, ramp(100, 100000, -3001));
        g.valid = validMask(100, {{30, 40}, {40, 45}, {70, 71}});
        f.channels.push_back(std::move(g));
    }
    {
        auto g = scaledChannel("Oil Pressure", kInt3, 2, 10, 0, 1, 10, 0, ramp(100, 0, 5));
        g.valid = validMask(100, {{50, 60}});
        f.channels.push_back(std::move(g));
    }
    {
        auto g = intChannel("Never Valid", kInt3, 2, 1, ramp(10, 1, 1));
        g.valid = validMask(10, {{0, 10}});
        f.channels.push_back(std::move(g));
    }
    f.patch = pinDeviceVersion;
    f.patchNote = "device version pinned to 100 at 0x052";
    return f;
}

Family truncatedNames() {
    Family f;
    f.name = "synthetic-truncated-names";
    f.summary =
        "synthetic: names that fill the 32-byte field exactly (no NUL), as sim exporters cut them, in "
        "ASCII, cp1252 and UTF-8";
    f.notes = "ld.name_unterminated; strings decode as UTF-8 when valid, else cp1252 (ld.string_encoding)";
    f.details = baseDetails(f.name);
    f.channels.push_back(floatChannel("Engine Coolant Temperature Sens1", kFloat, 4, 10, ramp(20, 60, 1)));
    f.channels.push_back(floatChannel("Brake Pressure Front Left Calipr", kFloat, 4, 10, ramp(20, 0, 3)));
    // O-umlaut is one cp1252 byte: 32 bytes on disk.
    f.channels.push_back(
        floatChannel("\xC3\x96ltemperatur Getriebe Ausgang Re", kFloat, 4, 10, ramp(20, 40, 2)));
    // Greek delta exists only in UTF-8: the name is written as UTF-8.
    f.channels.push_back(
        floatChannel("\xCE\x94 Speed Front Rear Axle Diffrnc", kFloat, 4, 10, ramp(20, -10, 1)));
    f.patch = pinDeviceVersion;
    f.patchNote = "device version pinned to 100 at 0x052";
    return f;
}

Family countMismatch() {
    Family f;
    f.name = "synthetic-count-mismatch";
    f.summary = "synthetic: the header channel count at 0x056 says one channel more than the list holds";
    f.notes = "ld.count_mismatch; the reader trusts the walk, never the count (05 section 14 item 5)";
    f.details = baseDetails(f.name);
    f.channels.push_back(intChannel("Gear", kInt3, 2, 10, ramp(30, 0, 0)));
    f.channels.push_back(floatChannel("Ground Speed", kFloat, 4, 10, triangle(30, 30, 200, 1.0)));
    f.channels.push_back(intChannel("Lap Number", kInt5, 4, 1, ramp(3, 1, 1)));
    f.patch = [](std::vector<std::byte>& b) {
        pinDeviceVersion(b);
        wr16(b, 0x056, static_cast<std::uint16_t>(rd16(b, 0x056) + 1));
    };
    f.patchNote =
        "u16 at 0x056 (the channel count) incremented by one after the writer; device version pinned";
    return f;
}

Family deviceEvents() {
    Family f;
    f.name = "synthetic-device-events";
    f.summary =
        "synthetic: a log-event block (header pointer 0x688) with codes 0 to 5, 17 and 52, two events at "
        "the same time and a restart stored out of time order";
    f.notes =
        "device events are markers of class DeviceEvent in group DeviceEvents, ordered by (time, stored "
        "order); labels for codes 0 to 5, 'Event <code>' otherwise";
    f.details = baseDetails(f.name);
    f.channels.push_back(floatChannel("Battery Volts", kFloat, 4, 10, triangle(100, 50, 30, 10.0, 12.0)));
    f.channels.push_back(intChannel("Logging State", kInt3, 2, 1, ramp(10, 0, 0)));
    const std::pair<std::uint32_t, Micros> events[] = {{0, 0},         {3, 0},         {17, 1'000'000},
                                                       {1, 2'500'000}, {4, 9'999'999}, {52, 3'000'000},
                                                       {2, 4'000'000}, {5, 5'000'000}};
    for (const auto& [code, t] : events) {
        f.markers.push_back(model::makeDeviceEventMarker(code, t, 0));
    }
    model::assignLoggedIds(f.markers);
    f.patch = pinDeviceVersion;
    f.patchNote = "device version pinned to 100 at 0x052";
    return f;
}

Family headerOnly() {
    Family f;
    f.name = "synthetic-header-only";
    f.summary =
        "synthetic: a header and detail records with no channel list (meta and data pointers at the end of "
        "the file)";
    f.notes =
        "a valid empty source with Warning ld.header_only; the oracle reports ld.meta_ptr_out_of_range and "
        "decodes no channel";
    f.details = baseDetails(f.name);
    f.details.push_back(text("Event", "Header Only Event"));
    f.patch = pinDeviceVersion;
    f.patchNote = "device version pinned to 100 at 0x052";
    return f;
}

Family m1Dialect() {
    Family f;
    f.name = "synthetic-m1-dialect";
    f.dialect = "m1";
    f.summary =
        "synthetic: the m1 dialect (device type M1, a terminator record ending the list, units at 0x40), "
        "with a GPS date in dtype_a 0x06, an F64 latitude, an enumeration and a deg C -> deg F channel";
    f.notes = "the terminator record is not a channel (ld.terminator_entry); 0x06/4 is a plain integer";
    f.details = baseDetails(f.name);
    {
        auto g = intChannel("Engine State", kInt3, 2, 20, {0, 1, 2, 2, 2, 3, 2, 0, -1, -2});
        g.enc.enumRef = std::uint16_t{3};
        g.enumTable = 3;
        g.kind = store::SampleKind::Enum;
        f.channels.push_back(std::move(g));
    }
    f.channels.push_back(intChannel("GPS Date", kInt6, 4, 1, {111026, 111026, 111026, 121026}));
    f.channels.push_back(intChannel("GPS Sats Used", kInt6, 4, 1, {7, 8, 9, 9}));
    {
        auto g = floatChannel("GPS Latitude", kDouble, 8, 10, {-33.4471, -33.4472, -33.4473, -33.44735});
        g.enc.unit = "deg";
        g.enc.dpsByte = 7;
        f.channels.push_back(std::move(g));
    }
    {
        auto g = scaledChannel("Gbox Oil Temp", kInt3, 2, 10, 32, 9, 5, 1, {238, 239, 240, 245});
        g.enc.unit = "F";
        f.channels.push_back(std::move(g));
    }
    f.enumTables = {enumTable(3, severityEntries())};
    return f;
}

Family cSeries() {
    Family f;
    f.name = "synthetic-c-series";
    f.dialect = "c-series";
    f.summary =
        "synthetic: the c-series logger layout (device type C125, a 64 KiB configuration region before the "
        "event record, short names at 0x40 and units at 0x48)";
    f.notes =
        "unit rule (b)/(c): a unit in the short-name field with no unit is the unit; otherwise the short "
        "name stays a short name";
    f.details = baseDetails(f.name);
    {
        auto g = scaledChannel("Gbox Oil Temp", kInt3, 2, 2, 32, 9, 5, 1, {238, 240, 250, 260});
        g.enc.shortName = "F";
        f.channels.push_back(std::move(g));
    }
    {
        auto g = intChannel("Engine Speed", kInt3, 2, 100, ramp(100, 800, 10));
        g.enc.shortName = "RPM";
        g.enc.unit = "rpm";
        f.channels.push_back(std::move(g));
    }
    {
        auto g = scaledChannel("Throttle Pos", kInt3, 2, 50, 0, 1, 10, 0, ramp(50, 0, 20));
        g.enc.shortName = "TPS";
        f.channels.push_back(std::move(g));
    }
    return f;
}

Family converterLegacy() {
    Family f;
    f.name = "synthetic-converter-legacy";
    f.dialect = "converter-legacy";
    f.summary =
        "synthetic: the legacy converter layout (channel list at 0x1336, extended records, no venue or "
        "vehicle records)";
    f.notes = "Info ld.legacy_layout; no long-name recovery";
    f.details = baseDetails(f.name);
    f.channels.push_back(floatChannel("Ground Speed", kFloat, 4, 20, triangle(100, 50, 250, 1.0)));
    f.channels.push_back(intChannel("Gear", kInt0, 2, 10, {0, 1, 1, 2, 2, 3, 3, 4, 4, 5}));
    f.channels.push_back(intChannel("Engine Speed", kInt5, 4, 50, ramp(50, 1000, 97)));
    f.patch = pinDeviceVersion;
    f.patchNote = "device version pinned to 100 at 0x052";
    return f;
}

// docs/formats/ld.md "Producer fingerprints": the public gotzl-style writer defaults (device ADL, serial
// 0x1F44, version 420, u16 0xADB0 at 0x054, counters 0x2EE1 + index), int16 / int32 / float / half pairs,
// names cut at 32 bytes.
Family simGotzl() {
    Family f;
    f.name = "synthetic-sim-gotzl";
    f.dialect = "sim-exporter";
    f.summary =
        "synthetic: the sim-exporter dialect with the public gotzl-style writer's header defaults (device "
        "ADL, serial 0x1F44, version 420, 0xADB0 at 0x054, counters 0x2EE1 + index)";
    f.notes =
        "parameterised from docs/formats/ld.md Producer fingerprints; no code or template of any exporter "
        "was used (D-040)";
    f.details = baseDetails(f.name);
    {
        auto g = intChannel("RPMS", kInt3, 2, 20, ramp(40, 1000, 100));
        g.enc.unit = "rpm";
        f.channels.push_back(std::move(g));
    }
    {
        auto g = floatChannel("SPEED", kFloat, 4, 20, triangle(40, 20, 200, 1.0));
        g.enc.unit = "km/h";
        f.channels.push_back(std::move(g));
    }
    {
        auto g = floatChannel("BRAKE_TEMP_LF", kFloat, 2, 10, ramp(20, 300, 16));
        g.enc.unit = "C";
        f.channels.push_back(std::move(g));
    }
    {
        auto g = intChannel("DISTANCE_TRAVELLED_SINCE_START_M", kInt5, 4, 10, ramp(20, 0, 55));
        g.enc.unit = "m";
        f.channels.push_back(std::move(g));
    }
    for (std::size_t i = 0; i < f.channels.size(); ++i) {
        f.channels[i].enc.counter = static_cast<std::uint16_t>(0x2EE1 + i);
    }
    return f;
}

// docs/formats/ld.md: the public motec-i2 writer defaults (u32 0x00010064 at 0x05A; 0 at 0x040).
Family simMi2() {
    Family f;
    f.name = "synthetic-sim-mi2";
    f.summary = "synthetic: a PC-software layout carrying the public motec-i2 writer's header defaults (u32 "
                "0x00010064 at 0x05A, u16 0 at 0x040)";
    f.notes = "parameterised from docs/formats/ld.md Producer fingerprints; dialect pc-layout,sim-exporter";
    f.details = baseDetails(f.name);
    {
        auto g = floatChannel("Speed", kFloat, 4, 50, triangle(100, 50, 300, 1.0));
        g.enc.unit = "km/h";
        f.channels.push_back(std::move(g));
    }
    {
        auto g = intChannel("Gear", kInt3, 2, 10, {1, 2, 3, 4, 5, 6, 5, 4, 3, 2});
        f.channels.push_back(std::move(g));
    }
    f.patch = [](std::vector<std::byte>& b) {
        pinDeviceVersion(b);
        wr16(b, 0x040, 0);
        wr32(b, 0x05A, 0x00010064u);
    };
    f.patchNote =
        "u16 at 0x040 set to 0 and u32 at 0x05A to 0x00010064 after the writer; device version pinned";
    return f;
}

Family zeroRate() {
    Family f;
    f.name = "synthetic-zero-rate";
    f.summary = "synthetic: the middle of three records has rate 0 but samples";
    f.notes =
        "Esses does not make it a channel (ld.zero_rate); the oracle decodes it untimed, and fixture verify "
        "reports it as excluded by the reader";
    f.details = baseDetails(f.name);
    f.channels.push_back(floatChannel("Before", kFloat, 4, 10, ramp(20, 0, 1)));
    f.channels.push_back(floatChannel("Untimed", kFloat, 4, 10, ramp(20, 100, 1)));
    f.channels.push_back(floatChannel("After", kFloat, 4, 10, ramp(20, 200, 1)));
    f.patch = [](std::vector<std::byte>& b) {
        pinDeviceVersion(b);
        const auto recs = recordOffsets(b);
        if (recs.size() >= 2) {
            wr16(b, recs[1] + 0x16, 0);
        }
    };
    f.patchNote =
        "u16 rate at record + 0x16 of the second record set to 0 after the writer; device version pinned";
    return f;
}

Family truncatedData() {
    Family f;
    f.name = "synthetic-truncated-data";
    f.summary = "synthetic: the file ends half way through the last sample array";
    f.notes = "ld.data_out_of_range: the count is clamped to the samples present (reader and oracle)";
    f.details = baseDetails(f.name);
    f.channels.push_back(floatChannel("Complete", kFloat, 4, 10, ramp(40, 0, 1)));
    f.channels.push_back(intChannel("Cut Short", kInt3, 2, 10, ramp(40, 1000, 3)));
    f.patch = [](std::vector<std::byte>& b) {
        pinDeviceVersion(b);
        std::size_t lastData = 0, lastBytes = 0;
        for (const std::size_t r : recordOffsets(b)) {
            const std::size_t data = rd32(b, r + 0x08);
            if (data >= lastData) {
                lastData = data;
                lastBytes = static_cast<std::size_t>(rd32(b, r + 0x0C)) * rd16(b, r + 0x14);
            }
        }
        if (lastData > 0 && lastBytes > 2) {
            b.resize(lastData + lastBytes / 2 + 1); // also leaves an odd byte: a partial sample
        }
    };
    f.patchNote = "file cut after half of the last sample array plus one byte; device version pinned";
    return f;
}

Family nonFinite() {
    Family f;
    f.name = "synthetic-nonfinite";
    f.summary = "synthetic: float channels holding NaN, +-infinity, signed zeros and subnormals";
    f.notes =
        "a NaN sample is invalid (S-1 D13); infinities are values; the oracle marks every non-finite value "
        "invalid in its CSV, which fixture verify reads as 'not finite'";
    f.details = baseDetails(f.name);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    f.channels.push_back(
        floatChannel("With NaN", kFloat, 4, 10, {0.0, 1.0, nan, 3.0, nan, nan, 6.0, -0.0, 8.0}));
    f.channels.push_back(floatChannel("With Inf", kFloat, 4, 10, {inf, 1.0, -inf, 3.0, 1e-45, -1e-45}));
    f.channels.push_back(floatChannel("Double NaN", kDouble, 8, 10, {nan, 2.0, 4.9e-324, -inf, 1e308}));
    f.patch = pinDeviceVersion;
    f.patchNote = "device version pinned to 100 at 0x052";
    return f;
}

Family ldxCompanion() {
    Family f;
    f.name = "synthetic-ldx-companion";
    f.summary = "synthetic: an .ldx companion with markers of every class, an unknown marker class, details "
                "overriding the .ld (Driver) and unknown elements at several levels";
    f.notes =
        "unknown classes stay opaque (ldx.unknown_marker_class), unknown elements are kept "
        "(ldx.unknown_element); companion details win over the .ld (D-015); marker times keep fractions "
        "of a microsecond";
    f.details = baseDetails(f.name);
    f.channels.push_back(intChannel("Lap Number", kInt5, 4, 1, ramp(25, 0, 0)));
    f.channels.push_back(floatChannel("Ground Speed", kFloat, 4, 10, triangle(250, 50, 300, 2.0)));
    f.ldxText =
        "<?xml version=\"1.0\"?>\n"
        "<LDXFile Locale=\"English_United Kingdom.1252\" DefaultLocale=\"C\" Version=\"1.6\">\n"
        " <Layers>\n"
        "  <Layer>\n"
        "   <MarkerBlock>\n"
        "    <MarkerGroup Name=\"Beacons\" Index=\"3\">\n"
        "     <Marker Version=\"100\" ClassName=\"BCN\" Name=\"Manual.1\" Flags=\"77\" "
        "Time=\"5000000.00\"/>\n"
        "     <Marker Version=\"100\" ClassName=\"SPLTBCN\" Name=\"Split.1\" Flags=\"5\" "
        "Time=\"9000000.00\"/>\n"
        "     <Marker Version=\"100\" ClassName=\"BCN\" Name=\"Manual.2\" Flags=\"77\" "
        "Time=\"15000000.25\"/>\n"
        "     <Marker Version=\"100\" ClassName=\"IGRDBCN\" Name=\"Ignored\" Flags=\"77\" "
        "Time=\"17000000\"/>\n"
        "     <Marker Version=\"100\" ClassName=\"SOR\" Name=\"Start of run\" Flags=\"1\" "
        "Time=\"5500000.00\"/>\n"
        "     <Unknown1 a='1'>text<b/></Unknown1>\n"
        "    </MarkerGroup>\n"
        "    <MarkerGroup Name=\"Stages\" Index=\"4\">\n"
        "     <Marker Version=\"100\" ClassName=\"STGSTRT\" Name=\"Stage 1\" Flags=\"0\" Time=\"2000000\"/>\n"
        "     <Marker Version=\"100\" ClassName=\"STGEND\" Name=\"Stage 1\" Flags=\"0\" Time=\"20000000\"/>\n"
        "     <Marker Version=\"100\" ClassName=\"RESET\" Name=\"Reset\" Flags=\"0\" Time=\"21000000\"/>\n"
        "    </MarkerGroup>\n"
        "   </MarkerBlock>\n"
        "   <LayerExtra kind=\"x\"/>\n"
        "  </Layer>\n"
        "  <Details>\n"
        "   <String Id=\"Total Laps\" Value=\"2\"/>\n"
        "   <String Id=\"Fastest Time\" Value=\"0:10.000\"/>\n"
        "   <String Id=\"Driver\" Value=\"Companion Driver\"/>\n"
        "   <Note>keep &amp; me</Note>\n"
        "  </Details>\n"
        " </Layers>\n"
        " <Custom attr=\"spaced\">\n"
        "   <Deep><Deeper x=\"1\"/></Deep>\n"
        " </Custom>\n"
        "</LDXFile>\n";
    f.patch = pinDeviceVersion;
    f.patchNote =
        "the .ldx is hand-written (unknown elements cannot come from a DataSource); device version pinned";
    return f;
}

// 08 section 6.7: the sim-exporter dialect with ACC-style channel names and units, BCN beacons for an out
// lap, twelve flying laps and an in lap, a one-hour session with 1 kHz channels. The oracle compares the slow
// channels (the fast ones would make multi-megabyte expectations); every pair is covered by the dtype
// families.
Family accDialect() {
    Family f;
    f.name = "synthetic-acc-dialect";
    f.dialect = "sim-exporter";
    f.summary =
        "synthetic: the sim-exporter dialect with ACC-style channel names and units, an .ldx with BCN "
        "beacons for an out lap, 12 flying laps and an in lap, and a one-hour session with 1 kHz channels";
    f.notes =
        "08 section 6.7 / D-127: public stand-in for a real ACC export (which stays private); the oracle "
        "compares GEAR and FUEL (channelsCompared), the 1 kHz channels are covered by the dtype families";
    f.details = baseDetails(f.name);
    f.details.push_back(text("Total Laps", "14", Provenance::Companion));
    constexpr std::size_t kSeconds = 3600;
    const auto fast = [&](std::string name, std::string unit, std::int64_t period, std::int64_t amp,
                          double div, double off) {
        auto g =
            floatChannel(std::move(name), kFloat, 4, 1000, triangle(kSeconds * 1000, period, amp, div, off));
        g.enc.unit = std::move(unit);
        return g;
    };
    f.channels.push_back(fast("SPEED", "km/h", 270'000, 28'000, 100.0, 0.0));
    f.channels.push_back(fast("STEERANGLE", "deg", 9'000, 360, 1.0, -180.0));
    {
        auto g = floatChannel("RPMS", kFloat, 4, 100, triangle(kSeconds * 100, 3'000, 7'000, 1.0, 1'500.0));
        g.enc.unit = "rpm";
        f.channels.push_back(std::move(g));
    }
    {
        std::vector<double> gear;
        gear.reserve(kSeconds * 2);
        for (std::size_t i = 0; i < kSeconds * 2; ++i) {
            gear.push_back(static_cast<double>(1 + (i / 7) % 6));
        }
        auto g = intChannel("GEAR", kInt3, 2, 2, std::move(gear));
        f.channels.push_back(std::move(g));
    }
    {
        constexpr std::int64_t kFuelStart = 7040; // 110 l in 1/64 l steps
        std::vector<double> fuel;
        fuel.reserve(kSeconds);
        for (std::size_t i = 0; i < kSeconds; ++i) {
            fuel.push_back(static_cast<double>(kFuelStart - static_cast<std::int64_t>(i)) / 64.0);
        }
        auto g = floatChannel("FUEL", kFloat, 4, 1, std::move(fuel));
        g.enc.unit = "l";
        f.channels.push_back(std::move(g));
    }
    // Beacons: out lap 130 s, 12 flying laps of 270 s, in lap to the end.
    std::uint32_t ordinal = 0;
    for (int lap = 0; lap <= 12; ++lap) {
        const Micros t = (130 + static_cast<Micros>(lap) * 270) * 1'000'000;
        f.markers.push_back(companionMarker(model::MarkerClass::BCN, t, "Beacons",
                                            "Manual." + std::to_string(lap + 1), 77, ordinal));
    }
    f.compared = {"GEAR", "FUEL"};
    return f;
}

std::vector<Family> allFamilies() {
    std::vector<Family> out = dtypeFamilies();
    out.push_back(multirate());
    out.push_back(shiftMul(false));
    out.push_back(shiftMul(true));
    out.push_back(scalingEdge());
    out.push_back(auxLongNames());
    out.push_back(auxEnum212());
    out.push_back(gapBlocks());
    out.push_back(truncatedNames());
    out.push_back(countMismatch());
    out.push_back(deviceEvents());
    out.push_back(headerOnly());
    out.push_back(m1Dialect());
    out.push_back(cSeries());
    out.push_back(converterLegacy());
    out.push_back(simGotzl());
    out.push_back(simMi2());
    out.push_back(zeroRate());
    out.push_back(truncatedData());
    out.push_back(nonFinite());
    out.push_back(ldxCompanion());
    out.push_back(accDialect());
    return out;
}

// ---- writing
// -----------------------------------------------------------------------------------------------

std::shared_ptr<model::DataSource> buildSource(const Family& f, store::ChunkCache& cache) {
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128(std::string_view("esses-gen-fixtures/" + f.name));
    src->prov.importerId = "esses.gen";
    model::DataLayer layer;
    store::ColumnOptions co;
    co.cache = &cache;
    Micros duration = 0;
    for (const GenChannel& g : f.channels) {
        model::Channel c;
        c.desc.name = g.name;
        c.desc.loggedName = g.name;
        c.desc.id = model::channelIdLogged(src->id, "Normal", g.name, "esses.gen");
        c.desc.kind = g.kind;
        c.desc.unit = units::kNoUnit;
        c.desc.quantity = units::kUnitless;
        const Rate rate{g.enc.rateHz, 1};
        c.desc.rate = rate;
        c.desc.dps = g.enc.dpsByte;
        c.desc.interp = g.enc.interpByte == kInterpPrevious ? store::Interp::Previous : store::Interp::Linear;
        if (g.enumTable) {
            c.desc.enums = model::EnumTableId{*g.enumTable};
        }
        c.desc.sourceEncoding = std::make_shared<formats::LdSourceEncoding>(g.enc);
        store::Column col(store::InMemoryProvider::fromF64(g.values, g.valid, "gen:" + g.name), co);
        c.desc.dtype = col.dtype();
        duration = std::max(duration, rate.timeAt(static_cast<store::SampleIdx>(g.values.size()), 0));
        c.series = store::UniformSeries{rate, 0, std::move(col)};
        layer.addChannel(std::move(c));
    }
    layer.duration = duration;
    layer.markers = f.markers;
    model::sortMarkers(layer.markers);
    layer.enumTables = f.enumTables;
    src->layers.push_back(std::move(layer));
    src->details = f.details;
    return src;
}

Result<void> writeInputs(const Family& f, const fs::path& dir, store::ChunkCache& cache) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const char* stale : {"input.ld", "input.ldx"}) {
        fs::remove(dir / stale, ec);
    }
    const auto src = buildSource(f, cache);
    auto writer = formats::makeLdWriter(nullptr);
    formats::ExportView view;
    view.source = src;
    formats::WriterOptions wo;
    auto opts = model::OpaqueJson::parse(R"({"ld":{"dialect":")" + f.dialect + R"("}})");
    if (!opts) {
        return Failure{opts.error()};
    }
    wo.options = std::move(opts).value();
    auto rep = writer->write(dir / "input.ld", view, wo, {}, {});
    cache.clear();
    if (!rep) {
        return Failure{rep.error()};
    }
    for (const auto& n : rep->notes) {
        if (n.code == "ld.channel_skipped" || n.code == "ld.value_clamped") {
            return fail(ErrorCode::Internal, f.name + ": the writer reported " + n.code + ": " + n.message);
        }
    }
    if (f.patch) {
        auto bytes = cli::readFile(dir / "input.ld");
        if (!bytes) {
            return Failure{bytes.error()};
        }
        std::vector<std::byte> b(bytes->size());
        std::memcpy(b.data(), bytes->data(), bytes->size());
        f.patch(b);
        if (auto w = cli::writeFile(dir / "input.ld",
                                    std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
            !w) {
            return w;
        }
    }
    if (f.ldxText) {
        if (auto w = cli::writeFile(dir / "input.ldx", *f.ldxText); !w) {
            return w;
        }
    }
    return ok();
}

std::string readme(const Family& f) {
    std::string s = "# " + f.name + "\n\n";
    s += f.summary + ".\n\n";
    s += "- Exercises: " + f.notes + ".\n";
    s += "- Written by `esses-gen-fixtures --family " + f.name +
         "` through the `motec.ld` writer (dialect option `" + f.dialect +
         "`); licence CC0-1.0 (our own output).\n";
    if (!f.patchNote.empty()) {
        s += "- Byte edits after the writer: " + f.patchNote + ".\n";
    }
    if (f.ldxText) {
        s += "- The `.ldx` companion is hand-written by the generator.\n";
    }
    bool ldx = f.ldxText.has_value();
    for (const model::Marker& m : f.markers) {
        ldx = ldx || m.prov == Provenance::Companion;
    }
    for (const Detail& d : f.details) {
        ldx = ldx || d.prov == Provenance::Companion;
    }
    s += "- `input.ld`";
    s += ldx ? " and `input.ldx` are" : " is";
    s +=
        " not committed: `esses-gen-fixtures --inputs-only` regenerates them bit-identically (`fixture.json` "
        "pins the sha256).\n";
    s += "- `expected.channels/` comes from `tools/ld_ref_decode.py` (D-041), never from the Esses reader";
    bool gaps = false;
    for (const GenChannel& g : f.channels) {
        gaps = gaps || !g.valid.empty();
    }
    if (gaps) {
        s += "; the valid column of integer gap ranges comes from the generator's declaration";
    }
    s += ". `expected.probe.json`, `expected.markers.json` and `expected.laps.json` are `esses-cli` output "
         "(regression goldens).\n";
    return s;
}

std::string exeName(std::string base) {
#if defined(ESSES_OS_WINDOWS)
    base += ".exe";
#endif
    return base;
}

struct Options {
    fs::path out = "fixtures/motec.ld";
    std::vector<std::string> families;
    bool inputsOnly = false;
    bool list = false;
    std::string cli;
    std::string python;
    std::string refDecode;
    std::string fixturesDir;
    std::string manifest;
    std::string large;
    std::uint64_t largeMb = 500;
};

int addExpectations(const Options& o, const Family& f, const fs::path& dir) {
    cli::ProcessRequest req;
    req.program = o.cli.empty() ? cli::u8(cli::executableDirectory() / cli::p8(exeName("esses-cli"))) : o.cli;
    req.args = {"fixture",    "add",          cli::u8(dir / "input.ld"),
                "--format",   "motec.ld",     "--oracle",
                "ref-decode", "--out",        cli::u8(dir),
                "--name",     f.name,         "--source",
                f.summary,    "--provenance", "synthetic",
                "--licence",  "CC0-1.0",      "--notes",
                f.notes,      "--validity",   "strict",
                "--quiet"};
    if (!f.importOptions.empty()) {
        req.args.insert(req.args.end(), {"--options", f.importOptions});
    }
    if (!f.compared.empty()) {
        std::string list;
        for (const auto& c : f.compared) {
            list += (list.empty() ? "" : ",") + c;
        }
        req.args.insert(req.args.end(), {"--channels", list});
    }
    if (!o.python.empty()) {
        req.args.insert(req.args.end(), {"--python", o.python});
    }
    if (!o.refDecode.empty()) {
        req.args.insert(req.args.end(), {"--ref-decode", o.refDecode});
    }
    auto rc = cli::runProcess(req);
    if (!rc) {
        std::cerr << "esses-gen-fixtures: " << rc.error().toString() << "\n";
        return 2;
    }
    if (*rc != 0) {
        std::cerr << "esses-gen-fixtures: fixture add failed for " << f.name << " (exit " << *rc << ")\n";
        return 1;
    }
    // Integer gap ranges: the oracle sees the held values; the generator declares them invalid.
    for (std::size_t k = 0; k < f.channels.size(); ++k) {
        const GenChannel& g = f.channels[k];
        if (g.valid.empty()) {
            continue;
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> invalid;
        for (std::size_t i = 0; i < g.valid.size();) {
            if (g.valid[i]) {
                ++i;
                continue;
            }
            const std::size_t a = i;
            while (i < g.valid.size() && !g.valid[i]) {
                ++i;
            }
            invalid.emplace_back(a, i);
        }
        std::error_code ec;
        for (fs::directory_iterator it(dir / "expected.channels", ec), end; !ec && it != end;
             it.increment(ec)) {
            const auto parsed = cli::parseExpectedFileName(cli::u8(it->path().filename()));
            if (!parsed || parsed->first != k) {
                continue;
            }
            auto text = cli::readFile(it->path());
            if (!text) {
                std::cerr << "esses-gen-fixtures: " << text.error().toString() << "\n";
                return 2;
            }
            if (auto w = cli::writeFile(it->path(), cli::markInvalid(*text, invalid)); !w) {
                std::cerr << "esses-gen-fixtures: " << w.error().toString() << "\n";
                return 2;
            }
        }
    }
    if (auto w = cli::writeFile(dir / "README.md", readme(f)); !w) {
        std::cerr << "esses-gen-fixtures: " << w.error().toString() << "\n";
        return 2;
    }
    return 0;
}

// AC-M0-12: a ~size MB pc-layout file for the probe timing (one hour; 1 kHz int16 and 500 Hz float channels,
// slower channels to a realistic channel count, BCN beacons in an .ldx).
int writeLarge(const fs::path& path, std::uint64_t megabytes) {
    Family f;
    f.name = "synthetic-large";
    f.details = baseDetails(f.name);
    constexpr std::size_t kSeconds = 3600;
    const std::uint64_t target = megabytes * 1'000'000;
    std::uint64_t bytes = 0;
    std::size_t k = 0;
    // 120 slow channels (10-50 Hz) first, then fast channels until the target size.
    for (; k < 120; ++k) {
        const std::uint16_t rate = k % 2 == 0 ? 10 : 50;
        auto g = intChannel("Slow Channel " + std::to_string(k), kInt3, 2, rate,
                            triangle(kSeconds * rate, 40 + static_cast<std::int64_t>(k), 1000, 1.0));
        bytes += g.values.size() * 2;
        f.channels.push_back(std::move(g));
    }
    while (bytes < target) {
        const bool isFloat = k % 3 == 0;
        const std::uint16_t rate = isFloat ? 500 : 1000;
        auto g = isFloat
                     ? floatChannel("Fast Float " + std::to_string(k), kFloat, 4, rate,
                                    triangle(kSeconds * rate, 997 + static_cast<std::int64_t>(k), 4000, 16.0))
                     : intChannel("Fast Int " + std::to_string(k), kInt3, 2, rate,
                                  triangle(kSeconds * rate, 1013 + static_cast<std::int64_t>(k), 30000, 1.0));
        bytes += g.values.size() * (isFloat ? 4u : 2u);
        f.channels.push_back(std::move(g));
        ++k;
    }
    for (int lap = 0; lap <= 12; ++lap) {
        f.markers.push_back(companionMarker(model::MarkerClass::BCN,
                                            (130 + static_cast<Micros>(lap) * 270) * 1'000'000, "Beacons",
                                            "Manual." + std::to_string(lap + 1), 77));
    }
    store::ChunkCache cache;
    const auto t0 = std::chrono::steady_clock::now();
    const auto src = buildSource(f, cache);
    auto writer = formats::makeLdWriter(nullptr);
    formats::ExportView view;
    view.source = src;
    auto rep = writer->write(path, view, {}, {}, {});
    if (!rep) {
        std::cerr << "esses-gen-fixtures: " << rep.error().toString() << "\n";
        return 2;
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "esses-gen-fixtures: wrote " << cli::u8(path) << " (" << rep->bytes << " bytes, "
              << f.channels.size() << " channels) in " << s << " s\n";
    return 0;
}

int run(const std::vector<std::string>& args) {
    CLI::App app{"esses-gen-fixtures: synthetic .ld/.ldx fixture families (05 section 9.1)"};
    Options o;
    std::string out = "fixtures/motec.ld";
    app.add_option("--out", out, "directory of the family directories (default fixtures/motec.ld)");
    app.add_option("--family", o.families, "generate only these families (repeatable)");
    app.add_flag("--inputs-only", o.inputsOnly, "write input.ld/.ldx only (no oracle, no expectations)");
    app.add_flag("--list", o.list, "list the family names");
    app.add_option("--cli", o.cli, "esses-cli to run fixture add with (default: beside this executable)");
    app.add_option("--python", o.python, "Python for the oracle");
    app.add_option("--ref-decode", o.refDecode, "tools/ld_ref_decode.py");
    app.add_option("--fixtures", o.fixturesDir,
                   "with --manifest: the fixture directories the inputs belong to");
    app.add_option("--manifest", o.manifest,
                   "write a fixture verify manifest mapping --fixtures dirs to the inputs");
    app.add_option("--large", o.large, "write the AC-M0-12 timing file to this path instead");
    app.add_option("--large-mb", o.largeMb, "size of the --large file in MB (default 500)");
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (const std::string& a : args) {
        argv.push_back(a.c_str());
    }
    try {
        app.parse(static_cast<int>(argv.size()), argv.data());
    } catch (const CLI::ParseError& e) {
        return app.exit(e) == 0 ? 0 : 2;
    }
    o.out = cli::p8(out);
    if (!o.large.empty()) {
        return writeLarge(cli::p8(o.large), o.largeMb);
    }
    std::vector<Family> families = allFamilies();
    if (o.list) {
        for (const Family& f : families) {
            std::cout << f.name << "\n";
        }
        return 0;
    }
    if (!o.families.empty()) {
        std::vector<Family> chosen;
        for (const std::string& n : o.families) {
            const auto it =
                std::find_if(families.begin(), families.end(), [&](const Family& f) { return f.name == n; });
            if (it == families.end()) {
                std::cerr << "esses-gen-fixtures: no family '" << n << "' (--list)\n";
                return 2;
            }
            chosen.push_back(*it);
        }
        families = std::move(chosen);
    }
    store::ChunkCache cache;
    nlohmann::json manifest = nlohmann::json{{"fixtures", nlohmann::json::array()}};
    int rc = 0;
    for (const Family& f : families) {
        const fs::path dir = o.out / cli::p8(f.name);
        if (auto w = writeInputs(f, dir, cache); !w) {
            std::cerr << "esses-gen-fixtures: " << f.name << ": " << w.error().toString() << "\n";
            return 2;
        }
        if (!o.manifest.empty()) {
            const fs::path fixtureDir =
                (o.fixturesDir.empty() ? o.out : cli::p8(o.fixturesDir)) / cli::p8(f.name);
            manifest["fixtures"].push_back(
                nlohmann::json{{"dir", cli::u8(fs::absolute(fixtureDir))},
                               {"input", cli::u8(fs::absolute(dir / "input.ld"))}});
        }
        if (!o.inputsOnly) {
            const int r = addExpectations(o, f, dir);
            if (r != 0) {
                rc = std::max(rc, r);
            }
            std::cout << (r == 0 ? "generated " : "FAILED    ") << f.name << "\n";
        }
    }
    if (!o.manifest.empty()) {
        auto text = cli::canonicalText(manifest);
        if (!text) {
            std::cerr << "esses-gen-fixtures: " << text.error().toString() << "\n";
            return 2;
        }
        if (auto w = cli::writeFile(cli::p8(o.manifest), *text); !w) {
            std::cerr << "esses-gen-fixtures: " << w.error().toString() << "\n";
            return 2;
        }
    }
    if (o.inputsOnly) {
        std::cout << "esses-gen-fixtures: wrote the inputs of " << families.size() << " families to "
                  << cli::u8(o.out) << "\n";
    }
    return rc;
}

} // namespace

#if defined(ESSES_OS_WINDOWS)
int wmain(int argc, wchar_t** argv) {
    (void)_setmode(_fileno(stdout), _O_BINARY);
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.push_back(esses::utf8::fromWide(argv[i]));
    }
    try {
        return run(args);
    } catch (const std::exception& e) {
        std::cerr << "esses-gen-fixtures: internal error: " << e.what() << "\n";
        return 2;
    }
}
#else
int main(int argc, char** argv) {
    try {
        return run(std::vector<std::string>(argv, argv + argc));
    } catch (const std::exception& e) {
        std::cerr << "esses-gen-fixtures: internal error: " << e.what() << "\n";
        return 2;
    }
}
#endif
