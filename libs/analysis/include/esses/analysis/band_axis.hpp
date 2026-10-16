// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// BandAxis (04 section 7.3 shared sub-schema "BandAxis"; 02 section 6.4; D-019): the persisted definition of
// a band axis (histogram bins, scatter/histogram colour channels; later suspension_histogram and
// mixture_map). Only the definition is persisted, never the generated bands: mode "arbitrary" uses `bands`
// verbatim, "even" generates them from dataMin/dataMax and bandWidth (bandMode "width") or bandCount
// ("count") at load; lessMinBand/greaterMaxBand add open bands for samples outside [dataMin, dataMax].
// Numbers carry units ({v, unit}, D-010). Also the binning input shared by HistogramBinner and ScatterBinner
// (SampleSpan).
// The value types are final (M3 contract); fromJson/toJson/generateBands live in src/binning/.
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/base/time.hpp>
#include <esses/units/unit_registry.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::analysis {

struct UnitValue { // {v, unit} as persisted (D-010); "" = unitless
    double v = 0.0;
    std::string unit;
    friend bool operator==(const UnitValue&, const UnitValue&) = default;
};

enum class BandAxisMode : std::uint8_t { Even, Arbitrary };
enum class BandMode : std::uint8_t { Width, Count };

struct BandDef { // an arbitrary band
    UnitValue min;
    UnitValue max;
    std::uint32_t colour = 0x2040FFFF; // 0xRRGGBBAA
    friend bool operator==(const BandDef&, const BandDef&) = default;
};

struct BandAxis {
    BandAxisMode mode = BandAxisMode::Even;
    BandMode bandMode = BandMode::Width;
    std::optional<UnitValue> dataMin; // nullopt: auto (the channel's scale, 04 section 11.1 item 1)
    std::optional<UnitValue> dataMax;
    std::optional<UnitValue> bandWidth;
    int bandCount = 10;
    std::string palette = "rainbow";
    std::optional<std::uint32_t> colour1;
    std::optional<std::uint32_t> colour2;
    bool reverse = false;
    bool centred = false;
    bool lessMinBand = true;
    bool greaterMaxBand = true;
    bool continuous = false;
    std::vector<BandDef> bands; // mode Arbitrary
    JsonDoc rest;               // unknown members survive toJson()

    // A BandAxis object (every member optional; the defaults above). Invalid with context.detail (a JSON
    // pointer) on a wrong type, an unknown mode or bandMode, a bandCount outside 1..1000, a colour that is
    // not
    // "#rrggbb[aa]" or an unknown member of a band or {v, unit} value; unknown top-level members go to rest.
    static Result<BandAxis> fromJson(const JsonDoc& json);
    // Every member but the absent optional ones (dataMin/dataMax/bandWidth/colour1/colour2) and an empty
    // bands list; colours lower case, "#rrggbb" when opaque. fromJson(toJson()) == *this.
    JsonDoc toJson() const;
    friend bool operator==(const BandAxis&, const BandAxis&) = default;
};

// A generated band in the unit the caller asked for; the open bands carry lessMin/greaterMax.
struct Band {
    double lo = 0.0;
    double hi = 0.0;
    std::uint32_t colour = 0;
    bool lessMin = false;    // the open band [-inf, lo of the first regular band): lo == -inf
    bool greaterMax = false; // the open band [hi of the last regular band, +inf): hi == +inf
    friend bool operator==(const Band&, const Band&) = default;
};

// The bands of `axis` in `unit` (persisted values converted through `units`; null = values taken as written,
// as is a value whose unit or `unit` is "" or unknown to the registry; a band width converts by scale only).
// autoMin/autoMax replace a missing dataMin/dataMax. "even":
//   width  n = ceil((max - min) / width) full-width bands from min (the last may end past max; a last edge
//          within rounding of max is max); without a bandWidth, bandCount bands as in count mode
//   count  bandCount equal bands over [min, max]
//   centred  band k is centred on min + k * w: width mode floor((max - min) / width) + 1 bands, count mode
//          bandCount bands with w = (max - min) / (bandCount - 1) (ignored for one band)
//   colours  palette "twoColour" blends colour1 into colour2 (defaults #2040ff, #ff2020), "rainbow" and
//          any other name sweep the hue from blue to red; reverse runs the palette backwards
// "arbitrary": bands[] as written with their colours. lessMinBand / greaterMaxBand add the open bands
// [-inf, first lo) and [last hi, +inf) with the first / last band's colour. Invalid (context.detail a JSON
// pointer into the axis) when the bands are empty or not increasing (lo < hi, no overlap), min >= max, the
// width is not positive, more than 10000 bands would be generated, or a value's unit does not convert.
Result<std::vector<Band>> generateBands(const BandAxis& axis, double autoMin, double autoMax,
                                        const units::UnitRegistry* units, std::string_view unit);

// A time span a binner reads (a slot's visible span, cut per range so `trusted` is known).
struct SampleSpan {
    Micros start = 0;
    Micros end = 0;
    bool trusted = true;
};

} // namespace esses::analysis
