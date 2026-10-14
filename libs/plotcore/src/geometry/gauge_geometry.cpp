// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// GaugeNumericBody, GaugeBarBody and GaugeGeometry (04 sections 16-17; D-198). Both gauges read their one
// channel (SlotData::channels[0]) at the cursor for Main and for the reference overlay (the second visible
// slot: Overlay 0, else Overlay 1; MainAndReference) by the single conversion rule of the Values window: x_c
// = map_Main.xOf(Main.start + cursorT), t_k = map_k.timeAt(x_c). Layout (logical px x dpr): the caption (18,
// Chrome) and a 4 px margin around the content rectangle.
//   numeric  Static: background and the content frame (Panel hit). Dynamic: the value (TextStyleId::Gauge)
//            centred in the top 65 % of the content (all of it without a reference overlay), the display
//            unit after it (GaugeUnits) when showUnits, the reference overlay's value beneath in its key
//            colour. dynamicFont sizes the value through TextRun::scale so that the widest value the channel
//            can show (its SummaryPyramid extremes at the display decimals, digits as '8') fills the width or
//            the height, never below 6 pt of the scheme's gauge font; without it the scale is the body font's
//            point size over the scheme's gauge font (1 for no font). The first visible colour band holding
//            the value (in the band's unit, converted) colours the value text (bandMode text) or fills the
//            content behind it (bandMode background).
//   bar      Static: background, the scale strip (left of a vertical track, under a horizontal one; ticks
//            labelled with scaleDps decimals) when showScale, the colour bands (bandLines lines: a line
//            across the track at each finite band edge; fill: a 4 px strip beside the track over the band's
//            extent), the track (keyBackground, framed; Panel hit). Dynamic: the bar from the origin (min,
//            zero or the custom origin clamped into the scale, max) to the value in barColour (default the
//            channel's colour; bandMode bar recolours it by the band holding the value), the reference
//            overlay as a 2 px marker line across the track in its key colour, the value text at the bar end
//            (vertical: above it; horizontal: right of it; kept inside the track, on the component
//            background). Scale channel and auto: the Main channel's whole-column SummaryPyramid (the M1
//            autoscale stand-in, as the scatter's, D-196 (c)); manual: the limits converted to the display
//            unit.
//   Chrome   the caption (auto text: the channel), the pending stripe, the focus border.

#include "body_json.hpp"
#include "geometry_common.hpp"

#include <esses/plotcore/gauge_geometry.hpp>
#include <esses/store/resampler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace esses::plot {

namespace {

using nlohmann::json;

constexpr float kGaugeMarginLogical = 4.0F;
constexpr float kMainShare = 0.65F;   // of the content height when a reference value is shown
constexpr double kMinPointSize = 6.0; // 04 section 16 item 1
constexpr float kMaxScale = 40.0F;    // a sanity bound for TextRun::scale
constexpr float kBandStripLogical = 4.0F;
constexpr const char* kInvalidText = "-";
constexpr const char* kMissingText = "(missing)";

std::string enumOf(const json& j, const std::string& pointer, std::initializer_list<const char*> allowed) {
    const std::string& text = jsonio::asString(j, pointer);
    for (const char* a : allowed) {
        if (text == a) {
            return text;
        }
    }
    jsonio::violate(pointer, "unexpected value \"" + text + "\"");
}

int dpsOf(const json& j, const std::string& pointer) {
    const int d = jsonio::asInt32(j, pointer);
    if (d < 0 || d > 15) {
        jsonio::violate(pointer, "decimals are 0..15");
    }
    return d;
}

// The "font" member: null, or {family, pointSize 6..400, bold, italic}.
JsonDoc fontOf(const json& j, const std::string& pointer) {
    if (j.is_null()) {
        return JsonDoc{};
    }
    if (!j.is_object()) {
        jsonio::violate(pointer, "expected null or a font object");
    }
    jsonio::ObjectReader r(j, pointer);
    const std::string& family = jsonio::asString(r.required("family"), r.child("family"));
    if (family.empty()) {
        jsonio::violate(r.child("family"), "the family is not empty");
    }
    const double size = jsonio::asNumber(r.required("pointSize"), r.child("pointSize"));
    if (size < 6.0 || size > 400.0) {
        jsonio::violate(r.child("pointSize"), "the point size is 6..400");
    }
    if (const json* b = r.optional("bold")) {
        (void)jsonio::asBool(*b, r.child("bold"));
    }
    if (const json* i = r.optional("italic")) {
        (void)jsonio::asBool(*i, r.child("italic"));
    }
    Result<JsonDoc> doc = jsonio::toDoc(j);
    if (!doc) {
        jsonio::violate(pointer, doc.error().message);
    }
    return std::move(*doc);
}

std::optional<double> fontPointSize(const JsonDoc& font) {
    if (!font.isObject()) {
        return std::nullopt;
    }
    const json j = jsonio::fromDoc(font);
    const auto it = j.find("pointSize");
    if (it == j.end() || !it->is_number()) {
        return std::nullopt;
    }
    return it->get<double>();
}

} // namespace

// ---- GaugeNumericBody

Result<GaugeNumericBody> GaugeNumericBody::parse(const JsonDoc& body) {
    return jsonio::guardJson([&]() -> Result<GaugeNumericBody> {
        GaugeNumericBody out;
        out.rest = body;
        if (body.isNull()) {
            return out;
        }
        const json j = jsonio::fromDoc(body);
        if (!j.is_object()) {
            jsonio::violate("", "gauge_numeric body is not an object");
        }
        jsonio::ObjectReader r(j, "");
        if (const json* v = r.optional("channel")) {
            out.channel = jsonio::asString(*v, r.child("channel"));
        }
        if (const json* v = r.optional("displayUnit")) {
            out.displayUnit = jsonio::asString(*v, r.child("displayUnit"));
        }
        if (const json* v = r.optional("displayDps")) {
            out.displayDps = dpsOf(*v, r.child("displayDps"));
        }
        if (const json* v = r.optional("showUnits")) {
            out.showUnits = jsonio::asBool(*v, r.child("showUnits"));
        }
        if (const json* v = r.optional("dynamicFont")) {
            out.dynamicFont = jsonio::asBool(*v, r.child("dynamicFont"));
        }
        if (const json* v = r.optional("font")) {
            out.font = fontOf(*v, r.child("font"));
        }
        if (const json* v = r.optional("colourBands")) {
            out.colourBands = detail::readColourBands(*v, r.child("colourBands"));
        }
        if (const json* v = r.optional("bandMode")) {
            out.bandMode = enumOf(*v, r.child("bandMode"), {"text", "background"});
        }
        return out;
    });
}

JsonDoc GaugeNumericBody::toJson() const {
    const GaugeNumericBody d;
    json j = detail::mergeOver(rest);
    detail::put(j, "channel", channel, d.channel);
    detail::put(j, "displayUnit", displayUnit, d.displayUnit);
    if (displayDps) {
        j["displayDps"] = *displayDps;
    } else {
        j.erase("displayDps");
    }
    detail::put(j, "showUnits", showUnits, d.showUnits);
    detail::put(j, "dynamicFont", dynamicFont, d.dynamicFont);
    if (font.isObject()) {
        j["font"] = jsonio::fromDoc(font);
    } else if (j.contains("font")) {
        j["font"] = nullptr;
    }
    if (j.contains("colourBands") || !colourBands.empty()) {
        j["colourBands"] = detail::colourBandsJson(colourBands);
    }
    detail::put(j, "bandMode", bandMode, d.bandMode);
    return detail::docOf(j, rest);
}

// ---- GaugeBarBody

Result<GaugeBarBody> GaugeBarBody::parse(const JsonDoc& body) {
    return jsonio::guardJson([&]() -> Result<GaugeBarBody> {
        GaugeBarBody out;
        out.rest = body;
        if (body.isNull()) {
            return out;
        }
        const json j = jsonio::fromDoc(body);
        if (!j.is_object()) {
            jsonio::violate("", "gauge_bar body is not an object");
        }
        jsonio::ObjectReader r(j, "");
        if (const json* v = r.optional("channel")) {
            out.channel = jsonio::asString(*v, r.child("channel"));
        }
        if (const json* v = r.optional("displayUnit")) {
            out.displayUnit = jsonio::asString(*v, r.child("displayUnit"));
        }
        if (const json* v = r.optional("scale")) {
            out.scale = detail::readScaleSpec(*v, r.child("scale"));
        }
        if (const json* v = r.optional("orientation")) {
            out.orientation = enumOf(*v, r.child("orientation"), {"vertical", "horizontal"});
        }
        if (const json* v = r.optional("originStyle")) {
            out.originStyle = enumOf(*v, r.child("originStyle"), {"min", "zero", "max", "custom"});
        }
        if (const json* v = r.optional("customOrigin")) {
            out.customOrigin = detail::readUnitValue(*v, r.child("customOrigin"));
        }
        if (const json* v = r.optional("barColour")) {
            if (!v->is_null()) {
                out.barColour = detail::readColour(*v, r.child("barColour"));
            }
        }
        if (const json* v = r.optional("scaleDps")) {
            out.scaleDps = dpsOf(*v, r.child("scaleDps"));
        }
        if (const json* v = r.optional("showScale")) {
            out.showScale = jsonio::asBool(*v, r.child("showScale"));
        }
        if (const json* v = r.optional("colourBands")) {
            out.colourBands = detail::readColourBands(*v, r.child("colourBands"));
        }
        if (const json* v = r.optional("bandLines")) {
            out.bandLines = enumOf(*v, r.child("bandLines"), {"none", "lines", "fill"});
        }
        if (const json* v = r.optional("bandMode")) {
            out.bandMode = enumOf(*v, r.child("bandMode"), {"none", "bar"});
        }
        return out;
    });
}

JsonDoc GaugeBarBody::toJson() const {
    const GaugeBarBody d;
    json j = detail::mergeOver(rest);
    detail::put(j, "channel", channel, d.channel);
    detail::put(j, "displayUnit", displayUnit, d.displayUnit);
    if (j.contains("scale") || !(scale == d.scale)) {
        j["scale"] = detail::scaleSpecJson(scale);
    }
    detail::put(j, "orientation", orientation, d.orientation);
    detail::put(j, "originStyle", originStyle, d.originStyle);
    if (customOrigin) {
        j["customOrigin"] = detail::unitValueJson(*customOrigin);
    } else {
        j.erase("customOrigin");
    }
    if (barColour) {
        j["barColour"] = project::formatRgba(*barColour);
    } else if (j.contains("barColour")) {
        j["barColour"] = nullptr;
    }
    detail::put(j, "scaleDps", scaleDps, d.scaleDps);
    detail::put(j, "showScale", showScale, d.showScale);
    if (j.contains("colourBands") || !colourBands.empty()) {
        j["colourBands"] = detail::colourBandsJson(colourBands);
    }
    detail::put(j, "bandLines", bandLines, d.bandLines);
    detail::put(j, "bandMode", bandMode, d.bandMode);
    return detail::docOf(j, rest);
}

// ---- shared by the builders

namespace {

struct GaugeFrame {
    RectPx caption;
    RectPx content;
};

GaugeFrame gaugeFrame(const ComponentBuildInputs& in) {
    GaugeFrame f;
    const float dpr = geom::dprOf(in);
    const auto w = static_cast<float>(std::max(in.layout.widthPx, 0));
    const auto h = static_cast<float>(std::max(in.layout.heightPx, 0));
    f.caption = geom::captionRect(in);
    const float m = geom::snap(kGaugeMarginLogical * dpr);
    const float top = f.caption.h + m;
    f.content = RectPx{m, top, std::max(0.0F, w - 2.0F * m), std::max(0.0F, h - top - m)};
    return f;
}

// The value of the gauge's channel at the cursor in slot k (display units).
struct GaugeValue {
    enum class State : std::uint8_t { None, Missing, Pending, Invalid, Valid };
    State state = State::None;
    double v = 0.0;
};

const ResolvedChannel* gaugeChannel(const SlotData& slot, const std::string& name) {
    return geom::channelOf(slot, 0, name);
}

GaugeValue valueAt(const ComponentBuildInputs& in, std::size_t k, const std::string& channel) {
    GaugeValue out;
    if (k >= in.slots.size() || channel.empty()) {
        return out;
    }
    const SlotData& slot = in.slots[k];
    const ResolvedChannel* rc = gaugeChannel(slot, channel);
    if (rc == nullptr || !rc->descriptor) {
        out.state = GaugeValue::State::Missing;
        return out;
    }
    if (rc->status == model::ChannelStatus::Pending || !rc->series) {
        out.state = GaugeValue::State::Pending;
        return out;
    }
    const SlotData& main = in.slots.front();
    Micros t = main.range.start + in.view.cursorT;
    if (k > 0) {
        const DomainMapper mainMap = geom::mapperOf(in, main, nullptr);
        const DomainMapper map = geom::mapperOf(in, slot, nullptr);
        if (in.view.domain == Domain::Distance && (!mainMap.hasDistance() || !map.hasDistance())) {
            return out; // no distance data: no value (the banner's rule of time_distance)
        }
        t = map.timeAt(mainMap.xOf(t));
    }
    const store::ValueSample s = store::Resampler::sampleAt(*rc->series, t, rc->descriptor->interp);
    if (s.state == store::SampleState::Absent) {
        out.state = GaugeValue::State::Pending; // the chunk is not resident yet (the widget retries)
    } else if (s.state == store::SampleState::Invalid) {
        out.state = GaugeValue::State::Invalid;
    } else {
        out.state = GaugeValue::State::Valid;
        out.v = rc->toDisplay(s.value);
    }
    return out;
}

std::string valueText(const GaugeValue& v, int dps) {
    switch (v.state) {
    case GaugeValue::State::None:
        return {};
    case GaugeValue::State::Missing:
        return kMissingText;
    case GaugeValue::State::Pending:
        return geom::kPendingText;
    case GaugeValue::State::Invalid:
        return kInvalidText;
    case GaugeValue::State::Valid:
        return geom::formatValue(v.v, dps);
    }
    return {};
}

// The colour bands in the display unit of the channel (a band whose limits do not convert is skipped).
std::vector<ColourBand> bandsIn(const std::vector<ColourBand>& bands, const ResolvedChannel* rc,
                                const units::UnitRegistry* registry) {
    std::vector<ColourBand> out = bands;
    const std::string unit = rc != nullptr ? rc->displayUnit : std::string{};
    for (ColourBand& b : out) {
        const std::optional<double> lo = geom::unitValueIn(b.min, unit, rc, registry);
        const std::optional<double> hi = geom::unitValueIn(b.max, unit, rc, registry);
        if ((b.open != BandOpen::Below && !lo) || (b.open != BandOpen::Above && !hi)) {
            b.visible = false;
            continue;
        }
        b.min = analysis::UnitValue{lo.value_or(b.min.v), unit};
        b.max = analysis::UnitValue{hi.value_or(b.max.v), unit};
    }
    return out;
}

Rgba channelColour(const ComponentBuildInputs& in, std::size_t k, const ResolvedChannel* rc) {
    const int index =
        rc != nullptr && rc->descriptor && rc->descriptor->colourIndex >= 0 ? rc->descriptor->colourIndex : 0;
    return traceColour(geom::schemeOf(in), in.selection, in.slots[k].slot, index);
}

DrawList gaugeChrome(const ComponentBuildInputs& in, const std::string& channel, bool pending) {
    DrawList list = geom::newList(in);
    const float dpr = geom::dprOf(in);
    const geom::Colours colours(geom::schemeOf(in));
    const GaugeFrame f = gaugeFrame(in);
    geom::emitCaption(list, f.caption, geom::captionText(in, channel), colours, dpr);
    if (pending || geom::anyPending(in)) {
        geom::emitPendingStripe(list, RectPx{0.0F, f.caption.h, static_cast<float>(in.layout.widthPx), 0.0F},
                                colours, dpr);
    }
    geom::emitFocus(list, in, colours, dpr);
    return list;
}

// "-88.8" for a value range: the longer formatted extreme with its digits as '8' (stable font sizing).
std::string widestOf(double lo, double hi, int dps) {
    const std::string a = geom::formatValue(lo, dps);
    const std::string b = geom::formatValue(hi, dps);
    std::string w = a.size() >= b.size() ? a : b;
    for (char& c : w) {
        if (c >= '0' && c <= '9') {
            c = '8';
        }
    }
    return w;
}

GaugeNumericBody numericBodyOf(const ComponentBuildInputs& in) {
    Result<GaugeNumericBody> body = GaugeNumericBody::parse(in.spec.body);
    return body ? std::move(*body) : GaugeNumericBody{};
}

GaugeBarBody barBodyOf(const ComponentBuildInputs& in) {
    Result<GaugeBarBody> body = GaugeBarBody::parse(in.spec.body);
    return body ? std::move(*body) : GaugeBarBody{};
}

void addCaptionHit(ComponentBuild& build, const RectPx& caption) {
    if (caption.h > 0.0F) {
        HitResult hit;
        hit.kind = HitKind::Caption;
        build.hits.addRect(caption, hit);
    }
}

// One value line of the numeric gauge: the value (Gauge, `scale`) and, when `unit` is set, the unit after it
// (GaugeUnits), centred in `area` as a pair.
void emitValueLine(DrawList& list, const RectPx& area, const std::string& text, const std::string& unit,
                   float scale, Rgba colour, const ITextMetrics& metrics, float dpr) {
    const float unitW = unit.empty() ? 0.0F : metrics.advancePx(TextStyleId::GaugeUnits, unit, dpr);
    const float gap = unit.empty() ? 0.0F : metrics.advancePx(TextStyleId::GaugeUnits, " ", dpr);
    const float textW = metrics.advancePx(TextStyleId::Gauge, text, dpr) * scale;
    const float total = std::min(area.w, textW + gap + unitW);
    const float left = area.x + 0.5F * (area.w - total);
    const float cy = area.y + 0.5F * area.h;
    const std::size_t cmd = list.textRun(text, left, cy, TextStyleId::Gauge, TextAnchor::MiddleLeft, colour,
                                         std::max(1.0F, area.w - gap - unitW));
    list.texts[list.cmds[cmd].textIndex].scale = scale;
    if (!unit.empty()) {
        list.textRun(unit, std::min(left + textW + gap, area.x + area.w - unitW), cy, TextStyleId::GaugeUnits,
                     TextAnchor::MiddleLeft, colour);
    }
}

} // namespace

// ---- numeric

Result<ComponentBuild> GaugeGeometry::buildNumericStatic(const ComponentBuildInputs& inputs,
                                                         const CancelToken& cancel) {
    Result<GaugeNumericBody> parsed = GaugeNumericBody::parse(inputs.spec.body);
    if (!parsed) {
        return Failure{parsed.error()};
    }
    if (cancel.checkpoint()) {
        return fail(ErrorCode::Cancelled, "gauge build cancelled");
    }
    ComponentBuild build;
    build.viewVersion = inputs.view.version;
    build.list = geom::newList(inputs);
    const float dpr = geom::dprOf(inputs);
    const geom::Colours colours(geom::schemeOf(inputs));
    const GaugeFrame f = gaugeFrame(inputs);
    build.list.clear(colours.background);
    addCaptionHit(build, f.caption);
    if (f.content.w > 0.0F && f.content.h > 0.0F) {
        build.list.rectOutline(f.content, colours.frame, geom::lineWidthOf(dpr));
        HitResult panel;
        panel.kind = HitKind::Panel;
        panel.group = 0;
        build.hits.addRect(f.content, panel);
    }
    return build;
}

DrawList GaugeGeometry::buildNumericDynamic(const ComponentBuildInputs& inputs) {
    DrawList list = geom::newList(inputs);
    const GaugeNumericBody body = numericBodyOf(inputs);
    const GaugeFrame f = gaugeFrame(inputs);
    if (inputs.slots.empty() || body.channel.empty() || !(f.content.w > 0.0F) || !(f.content.h > 0.0F)) {
        return list;
    }
    const float dpr = geom::dprOf(inputs);
    const project::Scheme& scheme = geom::schemeOf(inputs);
    const ITextMetrics& metrics = geom::metricsOf(inputs);
    const geom::Colours colours(scheme);
    const ResolvedChannel* mainRc = gaugeChannel(inputs.slots.front(), body.channel);
    const int dps = body.displayDps.value_or(mainRc != nullptr ? mainRc->dps : 2);
    const GaugeValue main = valueAt(inputs, 0, body.channel);
    const bool withReference = inputs.slots.size() > 1;
    const std::string unit =
        body.showUnits && mainRc != nullptr && mainRc->descriptor ? mainRc->displayUnit : std::string{};

    // Colour bands (04 section 16 item 2).
    Rgba textColour = colours.foreground;
    if (main.state == GaugeValue::State::Valid && !body.colourBands.empty()) {
        const std::vector<ColourBand> bands = bandsIn(body.colourBands, mainRc, inputs.units.get());
        if (const std::optional<std::size_t> b = colourBandOf(bands, main.v)) {
            if (body.bandMode == "background") {
                list.filledRect(f.content, bands[*b].colour);
            } else {
                textColour = bands[*b].colour;
            }
        }
    }

    // The value's size: the widest value the channel shows (its summary extremes), else this one.
    const float pad = geom::snap(2.0F * dpr);
    const RectPx mainArea{f.content.x + pad, f.content.y + pad, std::max(0.0F, f.content.w - 2.0F * pad),
                          std::max(0.0F, (withReference ? kMainShare : 1.0F) * f.content.h - 2.0F * pad)};
    const std::string text = valueText(main, dps);
    std::string widest = text;
    if (mainRc != nullptr && mainRc->series) {
        const geom::SummaryRange sr = geom::summaryRange(*mainRc, mainRc->toDisplay);
        if (sr.covered && sr.range.valid()) {
            std::string w = widestOf(sr.range.vMin, sr.range.vMax, dps);
            if (w.size() >= widest.size()) {
                widest = std::move(w);
            }
        }
    }
    const float unitW = unit.empty() ? 0.0F : metrics.advancePx(TextStyleId::GaugeUnits, unit, dpr);
    const float gap = unit.empty() ? 0.0F : metrics.advancePx(TextStyleId::GaugeUnits, " ", dpr);
    const float valueW1 = std::max(1.0F, metrics.advancePx(TextStyleId::Gauge, widest, dpr));
    const float lineH1 = std::max(1.0F, metrics.lineHeightPx(TextStyleId::Gauge, dpr));
    const double basePt = std::max(1.0, scheme.font("gauge").pointSize);
    const auto minScale = static_cast<float>(kMinPointSize / basePt);
    float scale = 1.0F;
    if (body.dynamicFont) {
        const float fit = std::min(std::max(0.0F, mainArea.w - unitW - gap) / valueW1, mainArea.h / lineH1);
        scale = std::clamp(fit, minScale, kMaxScale);
    } else if (const std::optional<double> pt = fontPointSize(body.font)) {
        scale = std::clamp(static_cast<float>(*pt / basePt), 0.1F, kMaxScale);
    }
    if (!text.empty()) {
        emitValueLine(list, mainArea, text, main.state == GaugeValue::State::Valid ? unit : std::string{},
                      scale, textColour, metrics, dpr);
    }

    // The reference overlay beneath in its key colour.
    if (withReference) {
        const GaugeValue ref = valueAt(inputs, 1, body.channel);
        const std::string refText = valueText(ref, dps);
        const RectPx refArea{mainArea.x, f.content.y + kMainShare * f.content.h, mainArea.w,
                             std::max(0.0F, (1.0F - kMainShare) * f.content.h - pad)};
        if (!refText.empty() && refArea.h > 0.0F) {
            const float refFit =
                std::min(std::max(0.0F, refArea.w - unitW - gap) / valueW1, refArea.h / lineH1);
            const float refScale = std::clamp(std::min(refFit, 0.5F * scale), minScale, kMaxScale);
            const Rgba refColour = channelColour(inputs, 1, gaugeChannel(inputs.slots[1], body.channel));
            emitValueLine(list, refArea, refText,
                          ref.state == GaugeValue::State::Valid ? unit : std::string{}, refScale, refColour,
                          metrics, dpr);
        }
    }
    return list;
}

DrawList GaugeGeometry::buildNumericChrome(const ComponentBuildInputs& inputs, bool pending) {
    return gaugeChrome(inputs, numericBodyOf(inputs).channel, pending);
}

// ---- bar

namespace {

struct BarFrame {
    GaugeFrame frame;
    bool vertical = true;
    RectPx track;         // the bar's area
    RectPx scaleStrip;    // tick labels (w == 0 without showScale)
    RectPx bandStrip;     // bandLines fill (w == 0 otherwise)
    float textTop = 0.0F; // vertical: the value text may rise to here (the line above the track)
    geom::Scale scale;    // display units; invalid while the summary is incomplete
    double origin = 0.0;
    const ResolvedChannel* rc = nullptr;
    std::vector<ColourBand> bands; // display units
};

double clampTo(const geom::Scale& s, double v) {
    return std::clamp(v, s.vMin, s.vMax);
}

geom::Scale barScale(const ComponentBuildInputs& in, const GaugeBarBody& body, const ResolvedChannel* rc) {
    geom::Scale s;
    if (rc == nullptr || !rc->descriptor) {
        return s;
    }
    if (body.scale.mode == ScaleMode::Manual && body.scale.min && body.scale.max) {
        const std::optional<double> lo =
            geom::unitValueIn(*body.scale.min, rc->displayUnit, rc, in.units.get());
        const std::optional<double> hi =
            geom::unitValueIn(*body.scale.max, rc->displayUnit, rc, in.units.get());
        if (lo && hi) {
            s.unite(*lo, *hi);
        }
    }
    if (!s.valid() && rc->series) {
        const geom::SummaryRange sr = geom::summaryRange(*rc, rc->toDisplay);
        if (sr.covered) {
            s = sr.range;
        }
    }
    if (s.valid()) {
        s.widenDegenerate();
    }
    return s;
}

BarFrame barFrame(const ComponentBuildInputs& in, const GaugeBarBody& body) {
    BarFrame b;
    b.frame = gaugeFrame(in);
    b.vertical = body.orientation != "horizontal";
    const float dpr = geom::dprOf(in);
    const ITextMetrics& metrics = geom::metricsOf(in);
    const RectPx& c = b.frame.content;
    b.rc = in.slots.empty() ? nullptr : gaugeChannel(in.slots.front(), body.channel);
    b.scale = barScale(in, body, b.rc);
    if (b.rc != nullptr && b.rc->descriptor) {
        b.bands = bandsIn(body.colourBands, b.rc, in.units.get());
    }
    if (b.scale.valid()) {
        if (body.originStyle == "max") {
            b.origin = b.scale.vMax;
        } else if (body.originStyle == "zero") {
            b.origin = clampTo(b.scale, 0.0);
        } else if (body.originStyle == "custom" && body.customOrigin) {
            const std::optional<double> o =
                geom::unitValueIn(*body.customOrigin, b.rc != nullptr ? b.rc->displayUnit : std::string{},
                                  b.rc, in.units.get());
            b.origin = clampTo(b.scale, o.value_or(b.scale.vMin));
        } else {
            b.origin = b.scale.vMin;
        }
    }

    const float lineH = metrics.lineHeightPx(TextStyleId::KeyValue, dpr);
    const float axisH = metrics.lineHeightPx(TextStyleId::Axis, dpr);
    const float tickLen = 3.0F * dpr;
    const float gap = geom::snap(2.0F * dpr);
    const float bandW =
        body.bandLines == "fill" && !body.colourBands.empty() ? geom::snap(kBandStripLogical * dpr) : 0.0F;
    float labelW = 0.0F;
    if (body.showScale && b.scale.valid()) {
        labelW = std::max(
            metrics.advancePx(TextStyleId::Axis, geom::formatValue(b.scale.vMin, body.scaleDps), dpr),
            metrics.advancePx(TextStyleId::Axis, geom::formatValue(b.scale.vMax, body.scaleDps), dpr));
    }
    if (b.vertical) {
        const float scaleW =
            labelW > 0.0F ? std::min(geom::snap(labelW + tickLen + 6.0F * dpr), 0.5F * c.w) : 0.0F;
        const float top = c.y + geom::snap(lineH + gap);
        // The track keeps half an axis line free at both ends so that the end labels fit.
        const float inset = scaleW > 0.0F ? geom::snap(0.5F * axisH) : 0.0F;
        const float trackX = c.x + scaleW + (bandW > 0.0F ? bandW + gap : 0.0F);
        b.track = RectPx{trackX, top + inset, std::max(0.0F, c.x + c.w - trackX),
                         std::max(0.0F, c.y + c.h - top - 2.0F * inset)};
        b.scaleStrip = scaleW > 0.0F ? RectPx{c.x, b.track.y, scaleW, b.track.h} : RectPx{};
        b.bandStrip = bandW > 0.0F ? RectPx{trackX - gap - bandW, b.track.y, bandW, b.track.h} : RectPx{};
        b.textTop = c.y;
    } else {
        const float scaleH = labelW > 0.0F ? geom::snap(axisH + tickLen + gap) : 0.0F;
        const float inset = labelW > 0.0F ? geom::snap(0.5F * labelW) : 0.0F;
        const float bottom = c.y + c.h - scaleH - (bandW > 0.0F ? bandW + gap : 0.0F);
        b.track = RectPx{c.x + inset, c.y, std::max(0.0F, c.w - 2.0F * inset), std::max(0.0F, bottom - c.y)};
        b.scaleStrip = scaleH > 0.0F ? RectPx{b.track.x, c.y + c.h - scaleH, b.track.w, scaleH} : RectPx{};
        b.bandStrip =
            bandW > 0.0F ? RectPx{b.track.x, b.track.y + b.track.h + gap, b.track.w, bandW} : RectPx{};
        b.textTop = b.track.y;
    }
    return b;
}

// Device px along the track of a value: y (vertical, top = max) or x (horizontal, left = min).
float posOf(const BarFrame& b, double v) {
    const double span = b.scale.vMax - b.scale.vMin;
    const double f = span > 0.0 ? (clampTo(b.scale, v) - b.scale.vMin) / span : 0.0;
    if (b.vertical) {
        return b.track.y + static_cast<float>((1.0 - f) * static_cast<double>(b.track.h));
    }
    return b.track.x + static_cast<float>(f * static_cast<double>(b.track.w));
}

void emitBarScale(DrawList& list, const BarFrame& b, const GaugeBarBody& body, const ITextMetrics& metrics,
                  Rgba colour, float dpr) {
    if (!(b.scaleStrip.w > 0.0F) || !(b.scaleStrip.h > 0.0F)) {
        return;
    }
    AxisSpec spec;
    spec.kind = AxisKind::Value;
    spec.vertical = b.vertical;
    spec.lo = b.vertical ? b.scale.vMax : b.scale.vMin;
    spec.hi = b.vertical ? b.scale.vMin : b.scale.vMax;
    spec.lengthPx = b.vertical ? b.track.h : b.track.w;
    spec.decimals = body.scaleDps;
    std::vector<AxisTick> ticks = AxisLayout::ticks(spec, metrics, dpr);
    for (AxisTick& t : ticks) {
        t.label = geom::formatValue(t.value, body.scaleDps);
    }
    if (b.vertical) {
        const float half = 0.5F * metrics.lineHeightPx(TextStyleId::Axis, dpr);
        ticks = geom::keepVertical(std::move(ticks), 0.0F, -half, b.track.h + half);
    } else {
        const RectPx& c = b.frame.content;
        ticks = geom::keepHorizontal(std::move(ticks), metrics, dpr, c.x - b.track.x, c.x + c.w - b.track.x);
    }
    AxisLayout::emit(list, spec, ticks, b.scaleStrip, colour, dpr);
}

void emitBarBands(DrawList& list, const BarFrame& b, const GaugeBarBody& body, float lineW) {
    for (const ColourBand& band : b.bands) {
        if (!band.visible) {
            continue;
        }
        const double lo = band.open == BandOpen::Below ? b.scale.vMin : band.min.v;
        const double hi = band.open == BandOpen::Above ? b.scale.vMax : band.max.v;
        if (body.bandLines == "fill" && b.bandStrip.w > 0.0F) {
            const float p0 = posOf(b, lo);
            const float p1 = posOf(b, hi);
            const RectPx r = b.vertical
                                 ? RectPx{b.bandStrip.x, std::min(p0, p1), b.bandStrip.w, std::abs(p1 - p0)}
                                 : RectPx{std::min(p0, p1), b.bandStrip.y, std::abs(p1 - p0), b.bandStrip.h};
            if (r.w > 0.0F && r.h > 0.0F) {
                list.filledRect(r, band.colour);
            }
        } else if (body.bandLines == "lines") {
            std::vector<float> segs;
            const std::array<std::pair<double, bool>, 2> edges{
                {{lo, band.open != BandOpen::Below}, {hi, band.open != BandOpen::Above}}};
            for (const auto& [edge, finite] : edges) {
                if (!finite || edge < b.scale.vMin || edge > b.scale.vMax) {
                    continue;
                }
                const float p = std::round(posOf(b, edge));
                if (b.vertical) {
                    segs.insert(segs.end(), {b.track.x, p, b.track.x + b.track.w, p});
                } else {
                    segs.insert(segs.end(), {p, b.track.y, p, b.track.y + b.track.h});
                }
            }
            if (!segs.empty()) {
                list.lines(segs.data(), segs.size() / 2, band.colour, 2.0F * lineW);
            }
        }
    }
}

} // namespace

Result<ComponentBuild> GaugeGeometry::buildBarStatic(const ComponentBuildInputs& inputs,
                                                     const CancelToken& cancel) {
    Result<GaugeBarBody> parsed = GaugeBarBody::parse(inputs.spec.body);
    if (!parsed) {
        return Failure{parsed.error()};
    }
    if (cancel.checkpoint()) {
        return fail(ErrorCode::Cancelled, "gauge build cancelled");
    }
    const GaugeBarBody& body = *parsed;
    ComponentBuild build;
    build.viewVersion = inputs.view.version;
    DrawList& list = build.list;
    list = geom::newList(inputs);
    const float dpr = geom::dprOf(inputs);
    const ITextMetrics& metrics = geom::metricsOf(inputs);
    const geom::Colours colours(geom::schemeOf(inputs));
    const float lineW = geom::lineWidthOf(dpr);
    const BarFrame b = barFrame(inputs, body);
    list.clear(colours.background);
    addCaptionHit(build, b.frame.caption);
    if (!(b.track.w > 0.0F) || !(b.track.h > 0.0F)) {
        return build;
    }
    list.filledRect(b.track, colours.keyBackground);
    if (b.scale.valid()) {
        emitBarScale(list, b, body, metrics, colours.foreground, dpr);
        emitBarBands(list, b, body, lineW); // 04 section 17 item 3
    }
    list.rectOutline(b.track, colours.frame, lineW);
    HitResult panel;
    panel.kind = HitKind::Panel;
    panel.group = 0;
    build.hits.addRect(b.track, panel);
    if (b.scaleStrip.w > 0.0F) {
        HitResult axis;
        axis.kind = HitKind::Axis;
        axis.group = 0;
        build.hits.addRect(b.scaleStrip, axis);
    }
    return build;
}

DrawList GaugeGeometry::buildBarDynamic(const ComponentBuildInputs& inputs) {
    DrawList list = geom::newList(inputs);
    const GaugeBarBody body = barBodyOf(inputs);
    if (inputs.slots.empty() || body.channel.empty()) {
        return list;
    }
    const BarFrame b = barFrame(inputs, body);
    if (!(b.track.w > 0.0F) || !(b.track.h > 0.0F)) {
        return list;
    }
    const float dpr = geom::dprOf(inputs);
    const ITextMetrics& metrics = geom::metricsOf(inputs);
    const geom::Colours colours(geom::schemeOf(inputs));
    const float lineW = geom::lineWidthOf(dpr);
    const int dps = b.rc != nullptr ? b.rc->dps : 0;
    const GaugeValue main = valueAt(inputs, 0, body.channel);
    const float pad = geom::snap(2.0F * dpr);
    const bool drawn = main.state == GaugeValue::State::Valid && b.scale.valid();

    if (drawn) {
        Rgba colour = body.barColour.value_or(channelColour(inputs, 0, b.rc));
        if (body.bandMode == "bar") {
            if (const std::optional<std::size_t> band = colourBandOf(b.bands, main.v)) {
                colour = b.bands[*band].colour;
            }
        }
        const float p0 = posOf(b, b.origin);
        const float p1 = posOf(b, main.v);
        const RectPx bar = b.vertical ? RectPx{b.track.x, std::min(p0, p1), b.track.w, std::abs(p1 - p0)}
                                      : RectPx{std::min(p0, p1), b.track.y, std::abs(p1 - p0), b.track.h};
        if (bar.w > 0.0F && bar.h > 0.0F) {
            list.filledRect(bar, colour);
        }
    }
    // The reference overlay as a marker line across the track.
    if (inputs.slots.size() > 1 && b.scale.valid()) {
        const GaugeValue ref = valueAt(inputs, 1, body.channel);
        if (ref.state == GaugeValue::State::Valid) {
            const float p = std::round(posOf(b, ref.v));
            const std::array<float, 4> seg =
                b.vertical ? std::array<float, 4>{b.track.x, p, b.track.x + b.track.w, p}
                           : std::array<float, 4>{p, b.track.y, p, b.track.y + b.track.h};
            list.lines(seg.data(), 2, channelColour(inputs, 1, gaugeChannel(inputs.slots[1], body.channel)),
                       2.0F * lineW);
        }
    }
    // The value at the bar end.
    const std::string text = valueText(main, dps);
    if (!text.empty()) {
        if (b.vertical) {
            const float h = metrics.lineHeightPx(TextStyleId::KeyValue, dpr);
            const float end = drawn ? posOf(b, main.v) : b.track.y;
            const float y =
                std::clamp(end - pad, b.textTop + h, std::max(b.textTop + h, b.track.y + b.track.h));
            list.textRun(text, b.track.x + 0.5F * b.track.w, y, TextStyleId::KeyValue,
                         TextAnchor::BottomCentre, colours.foreground, b.track.w, 0, colours.background);
        } else {
            const float w = metrics.advancePx(TextStyleId::KeyValue, text, dpr);
            const float end = drawn ? posOf(b, main.v) : b.track.x;
            const float x = std::clamp(end + pad, b.track.x, std::max(b.track.x, b.track.x + b.track.w - w));
            list.textRun(text, x, b.track.y + 0.5F * b.track.h, TextStyleId::KeyValue, TextAnchor::MiddleLeft,
                         colours.foreground, b.track.w, 0, colours.background);
        }
    }
    return list;
}

DrawList GaugeGeometry::buildBarChrome(const ComponentBuildInputs& inputs, bool pending) {
    return gaugeChrome(inputs, barBodyOf(inputs).channel, pending);
}

} // namespace esses::plot
