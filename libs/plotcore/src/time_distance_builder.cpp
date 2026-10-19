// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TimeDistanceBuilder (04 section 8): the body, the layout and the three layers. The M1 parts (groups and
// panels, scales, traces, axes, key labels and values, cursor, datum line, banner) are unchanged so the M1
// goldens hold; the M3 parts (bands, overlapped mode, scale lines, annotations, the status strip, the
// variance panel, the offset axis, the scrollbars, the Lo/Hi and splitter handles, the key's datum and
// measurement columns, the crosshair) are drawn only when shown.

#include "geometry/geometry_common.hpp"
#include "text_format.hpp"

#include <esses/analysis/variance_table.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/axis_layout.hpp>
#include <esses/plotcore/selection_snapshot.hpp>
#include <esses/plotcore/time_distance_builder.hpp>
#include <esses/plotcore/trace_builder.hpp>
#include <esses/project/rgba.hpp>
#include <esses/store/resampler.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>

namespace esses::plot {

namespace {

// A hit region's result (HitResult grew in M3; the M1 regions set only these members).
HitResult hitOf(HitKind kind, int group, int trace = -1, int slot = -1) {
    HitResult r;
    r.kind = kind;
    r.group = group;
    r.trace = trace;
    r.slot = slot;
    return r;
}

HitResult handleOf(HitKind kind, HandleKind handle, int group = -1, int slot = -1) {
    HitResult r;
    r.kind = kind;
    r.handleKind = handle;
    r.group = group;
    r.slot = slot;
    return r;
}

using nlohmann::json;

// ---- body JSON
// ----------------------------------------------------------------------------------------------

struct BodyError {
    std::string pointer;
    std::string message;
};

template <class T>
bool readAs(const json& obj, const char* key, T& out, const std::string& ptr, BodyError& err) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return true;
    }
    try {
        out = it->get<T>();
        return true;
    } catch (const json::exception&) {
        err = BodyError{ptr + "/" + key, "unexpected type"};
        return false;
    }
}

bool readNumber(const json& obj, const char* key, double& out, const std::string& ptr, BodyError& err) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return true;
    }
    if (!it->is_number()) {
        err = BodyError{ptr + "/" + key, "expected a number"};
        return false;
    }
    out = it->get<double>();
    return true;
}

// A string member that must be one of `allowed` (the schema's enum).
bool readEnum(const json& obj, const char* key, std::string& out, std::initializer_list<const char*> allowed,
              const std::string& ptr, BodyError& err) {
    std::string value = out;
    if (!readAs(obj, key, value, ptr, err)) {
        return false;
    }
    if (std::none_of(allowed.begin(), allowed.end(), [&](const char* a) { return value == a; })) {
        err = BodyError{ptr + "/" + key, "unexpected value"};
        return false;
    }
    out = std::move(value);
    return true;
}

// {"v": 12.5, "unit": "km/h"} or a bare number.
bool readValueWithUnit(const json& obj, const char* key, std::optional<double>& value, std::string& unit,
                       const std::string& ptr, BodyError& err) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return true;
    }
    if (it->is_number()) {
        value = it->get<double>();
        return true;
    }
    if (it->is_object()) {
        const auto v = it->find("v");
        if (v == it->end() || !v->is_number()) {
            err = BodyError{ptr + "/" + key + "/v", "expected a number"};
            return false;
        }
        value = v->get<double>();
        if (const auto u = it->find("unit"); u != it->end() && u->is_string()) {
            unit = u->get<std::string>();
        }
        return true;
    }
    err = BodyError{ptr + "/" + key, "expected {v, unit}"};
    return false;
}

bool readColour(const json& obj, const char* key, std::optional<Rgba>& out, const std::string& ptr,
                BodyError& err) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return true;
    }
    std::optional<Rgba> c;
    if (it->is_string()) {
        c = project::parseRgba(it->get<std::string>());
    }
    if (!c) {
        err = BodyError{ptr + "/" + key, R"(expected a colour "#rrggbb" or "#rrggbbaa")"};
        return false;
    }
    out = c;
    return true;
}

bool readScaleLine(const json& lines, const char* key, bool& enabled, std::optional<Rgba>& colour,
                   const std::string& ptr, BodyError& err) {
    const auto it = lines.find(key);
    if (it == lines.end() || !it->is_object()) {
        return true;
    }
    const std::string p = ptr + "/" + key;
    return readAs(*it, "enabled", enabled, p, err) && readColour(*it, "colour", colour, p, err);
}

std::string_view scaleModeName(ScaleMode m) noexcept {
    switch (m) {
    case ScaleMode::Channel:
        return "channel";
    case ScaleMode::Auto:
        return "auto";
    case ScaleMode::Manual:
        return "manual";
    }
    return "channel";
}

// Writes `value` at obj[key] when the member exists or the value differs from its default.
template <class T> void put(json& obj, const char* key, const T& value, const T& defaultValue) {
    if (obj.contains(key) || !(value == defaultValue)) {
        obj[key] = value;
    }
}

// The schema's unitValue: {"v", "unit"} ("" for a unitless value).
json valueWithUnit(double v, const std::string& unit) {
    json o = json::object();
    o["v"] = v;
    o["unit"] = unit;
    return o;
}

json childOf(const json& obj, const char* key) {
    const auto it = obj.find(key);
    return it != obj.end() && it->is_object() ? *it : json::object();
}

// A scale line written over its old object: enabled when it existed or differs, colour when set (removed when
// the typed member was cleared).
void putScaleLine(json& lines, const char* key, bool enabled, bool defaultEnabled,
                  const std::optional<Rgba>& colour) {
    json line = childOf(lines, key);
    put(line, "enabled", enabled, defaultEnabled);
    if (colour) {
        line["colour"] = project::formatRgba(*colour);
    } else {
        line.erase("colour");
    }
    if (lines.contains(key) || !line.empty()) {
        lines[key] = std::move(line);
    }
}

// ---- scheme roles
// -------------------------------------------------------------------------------------------

struct Colours {
    Rgba background, keyBackground, foreground, foreground2, frame, grid, cursor, datum;
    Rgba captionBackground, captionForeground, focus, pending;
    Rgba zoom, rangeMark, sectionCorner, sectionStraight, annotation, deviceEvent, varianceZero;

    explicit Colours(const project::Scheme& s)
        : background(s.role("cptBackground", 0xFFFFFFFFU)),
          keyBackground(s.role("keyBackground", 0xF4F4F4FFU)),
          foreground(s.role("cptForeground", 0x202020FFU)),
          foreground2(s.role("cptForeground2", 0x707070FFU)), frame(s.role("cptFrame", 0xA0A0A0FFU)),
          grid(s.role("dataGrid", 0xE6E6E6FFU)), cursor(s.role("cursorMain", 0xFF0000FFU)),
          datum(s.role("cursorDatum", 0xC09000FFU)),
          captionBackground(s.role("cptCaptionBackground", 0xE8E8E8FFU)),
          captionForeground(s.role("cptCaptionForeground", 0x202020FFU)),
          focus(s.role("cptFocusBorder", 0x2060FFFFU)), pending(s.role("pendingHatch", 0xC8C8C8FFU)),
          zoom(s.role("cursorZoom", 0x1A73E8FFU)), rangeMark(s.role("rangeMark", 0xDCE6F4FFU)),
          sectionCorner(s.role("sectionCorner", 0xFFD9B0FFU)),
          sectionStraight(s.role("sectionStraight", 0xD6E4FFFFU)),
          annotation(s.role("annotationMark", 0x2060FFFFU)), deviceEvent(s.role("deviceEvent", 0xC04000FFU)),
          varianceZero(s.role("varianceZero", 0x808080FFU)) {}
};

const project::Scheme& schemeOf(const ComponentBuildInputs& in) {
    static const project::Scheme kEmpty;
    return in.scheme ? *in.scheme : kEmpty;
}

const ITextMetrics& metricsOf(const ComponentBuildInputs& in) {
    static const FixedTextMetrics kFallback;
    return in.metrics ? *in.metrics : kFallback;
}

float dprOf(const ComponentBuildInputs& in) {
    return in.layout.dpr > 0.0F ? in.layout.dpr : 1.0F;
}

float snap(float v) {
    return std::round(v);
}

DrawList newList(const ComponentBuildInputs& in) {
    DrawList list;
    list.widthPx = in.layout.widthPx;
    list.heightPx = in.layout.heightPx;
    list.dpr = dprOf(in);
    return list;
}

TimeDistanceBody bodyOf(const ComponentBuildInputs& in) {
    Result<TimeDistanceBody> body = TimeDistanceBody::parse(in.spec.body);
    return body ? std::move(*body) : TimeDistanceBody{};
}

// ---- channels and slots
// -------------------------------------------------------------------------------------

struct FlatTrace {
    int group = 0;
    int index = 0; // flat channel index (channelNames order)
    const TdTrace* trace = nullptr;
};

std::vector<FlatTrace> flatTraces(const TimeDistanceBody& body) {
    std::vector<FlatTrace> out;
    int index = 0;
    for (std::size_t g = 0; g < body.groups.size(); ++g) {
        for (const TdTrace& t : body.groups[g].traces) {
            out.push_back(FlatTrace{static_cast<int>(g), index++, &t});
        }
    }
    return out;
}

const ResolvedChannel* channelOf(const SlotData& slot, int index, std::string_view name) {
    const auto i = static_cast<std::size_t>(index);
    if (i < slot.channels.size() && slot.channels[i].name == name) {
        return &slot.channels[i];
    }
    for (const ResolvedChannel& c : slot.channels) {
        if (c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

int colourIndexOf(const ResolvedChannel* rc, int flatIndex) {
    if (rc != nullptr && rc->descriptor && rc->descriptor->colourIndex >= 0) {
        return rc->descriptor->colourIndex;
    }
    return flatIndex;
}

Slot slotOf(const ComponentBuildInputs& in, std::size_t k) {
    if (k < in.slots.size()) {
        return in.slots[k].slot;
    }
    return Slot{};
}

std::pair<Rate, Micros> rateOf(const store::Series& series) {
    if (const auto* u = std::get_if<store::UniformSeries>(&series)) {
        return {u->rate, u->t0};
    }
    return {Rate{}, 0};
}

DomainMapper mapperOf(const ComponentBuildInputs& in, const SlotData& slot, const store::Series* series) {
    Rate rate;
    Micros t0 = 0;
    if (series != nullptr) {
        std::tie(rate, t0) = rateOf(*series);
    }
    return {in.view.domain, slot.range, slot.outing, slot.chain, rate, t0};
}

bool slotHasDistance(const SlotData& slot) {
    return slot.chain != nullptr && slot.chain->anyIndexed();
}

bool slotDrawsX(const ComponentBuildInputs& in, const SlotData& slot) {
    return in.view.domain == Domain::Time || slotHasDistance(slot);
}

std::string labelOf(const TdTrace& trace, const ResolvedChannel* main) {
    if (main == nullptr || !main->descriptor) {
        return trace.channel + " (missing)";
    }
    if (main->displayUnit.empty()) {
        return trace.channel;
    }
    return trace.channel + " [" + main->displayUnit + "]";
}

int dpsOf(const TdTrace& trace, const ResolvedChannel* rc) {
    if (trace.dps) {
        return *trace.dps;
    }
    return rc != nullptr ? rc->dps : 2;
}

std::string valueTemplate(int dps) {
    std::string s = "-88888";
    if (dps > 0) {
        s += '.';
        s.append(static_cast<std::size_t>(std::min(dps, 6)), '8');
    }
    return s;
}

std::string sourceName(const SlotData& slot) {
    if (slot.source && !slot.source->prov.path.empty()) {
        const std::u8string u8 = slot.source->prov.path.filename().u8string();
        return {u8.begin(), u8.end()};
    }
    return slot.slot.range.name;
}

// The source's short name in the range band: the file name without its extension.
std::string sourceShortName(const SlotData& slot) {
    if (slot.source && !slot.source->prov.path.empty()) {
        const std::u8string u8 = slot.source->prov.path.stem().u8string();
        return {u8.begin(), u8.end()};
    }
    return {};
}

// The flat index of the active channel (else the first trace; -1 without traces).
int activeIndexOf(const ComponentBuildInputs& in, const std::vector<FlatTrace>& flat) {
    for (const FlatTrace& ft : flat) {
        if (ft.trace->channel == in.spec.activeChannel) {
            return ft.index;
        }
    }
    return flat.empty() ? -1 : 0;
}

// The key colour of a slot's range (the variance trace, the offset sub-axis, a range band cell; 06
// section 5.3 item 3): Main red, Overlay 0 its role, Overlay k overlayPalette[k - 1].
Rgba keyColourOf(const ComponentBuildInputs& in, const std::vector<FlatTrace>& /*flat*/, std::size_t k) {
    return rangeKeyColour(schemeOf(in), slotOf(in, k));
}

// The layer of a slot in its source.
const model::DataLayer* layerOf(const SlotData& slot) {
    return slot.source != nullptr ? slot.source->layer(slot.layer) : nullptr;
}

// The DistanceIndex of the lap holding the slot's range start (the variance panel's lap, D-023).
std::shared_ptr<const analysis::DistanceIndex> lapIndexOf(const SlotData& slot) {
    if (slot.chain == nullptr) {
        return nullptr;
    }
    for (const LapPiece& p : slot.chain->pieces()) {
        if (p.start <= slot.range.start && slot.range.start < p.end) {
            return p.idx;
        }
    }
    return nullptr;
}

// Whether the slot's range spans more than one lap (an outing, a run of laps): the variance panel compares
// one lap with another, so such a range gets the banner instead of a comparison of its first lap (D-201).
bool spansLaps(const SlotData& slot) {
    if (slot.chain == nullptr) {
        return false;
    }
    for (const LapPiece& p : slot.chain->pieces()) {
        if (p.start <= slot.range.start && slot.range.start < p.end) {
            return slot.range.end > p.end;
        }
    }
    return false;
}

// ---- layout
// -------------------------------------------------------------------------------------------------

constexpr float kCaptionLogical = 18.0F;
constexpr float kXAxisLogical = 18.0F;
constexpr float kAxisStripLogical = 36.0F;
constexpr float kTopMarginLogical = 6.0F;
constexpr float kRightMarginLogical = 4.0F;
constexpr float kStatusRowLogical = 10.0F;   // 04 section 8.10 item 1
constexpr float kOffsetRowLogical = 12.0F;   // 04 section 8.1: 2 x 12 px
constexpr float kScrollbarLogical = 12.0F;   // 04 section 8.1
constexpr float kVScrollLogical = 6.0F;      // the vertical scrollbar inside the axis strip
constexpr float kHandleLogical = 7.0F;       // Lo/Hi handles
constexpr float kSplitterHalfLogical = 3.0F; // the panel splitter's hit half height
constexpr std::size_t kDefaultStatusRows = 8;

// The status channels of the body (04 section 8.10 item 1), resolved in the Main slot: a trace of that name,
// else the Main source's layer channel. Without a list: the Main layer's Bitfield and Bool channels (at most
// kDefaultStatusRows; the profiles' status patterns do not exist yet).
std::vector<TdStatusChannel> statusChannelsOf(const ComponentBuildInputs& in, const TimeDistanceBody& body,
                                              std::size_t traceCount) {
    std::vector<TdStatusChannel> out;
    if (!body.showStatusStrip || in.slots.empty()) {
        return out;
    }
    const project::Scheme& scheme = schemeOf(in);
    const SlotData& main = in.slots.front();
    const model::DataLayer* layer = layerOf(main);
    const auto add = [&](const std::string& name) {
        TdStatusChannel sc;
        sc.name = name;
        if (const ResolvedChannel* rc = channelOf(main, -1, name)) {
            sc.descriptor = rc->descriptor;
            if (rc->status != model::ChannelStatus::Pending) {
                sc.series = rc->series;
            }
        } else if (layer != nullptr) {
            if (const model::Channel* ch = layer->find(name)) {
                sc.descriptor = ch->desc;
                if (ch->status == model::ChannelStatus::Ok) {
                    sc.series = ch->series;
                }
            }
        }
        const int colourIndex = sc.descriptor && sc.descriptor->colourIndex >= 0
                                    ? sc.descriptor->colourIndex
                                    : static_cast<int>(traceCount + out.size());
        sc.colour = traceColour(scheme, in.selection, main.slot, colourIndex);
        out.push_back(std::move(sc));
    };
    if (!body.statusChannels.empty()) {
        for (const std::string& name : body.statusChannels) {
            if (!name.empty()) {
                add(name);
            }
        }
        return out;
    }
    if (layer != nullptr) {
        for (const model::Channel& ch : layer->channels) {
            if (out.size() >= kDefaultStatusRows) {
                break;
            }
            if (ch.desc.kind == store::SampleKind::Bitfield || ch.desc.kind == store::SampleKind::Bool) {
                add(ch.desc.name);
            }
        }
    }
    return out;
}

int referenceSlotOf(const ComponentBuildInputs& in) {
    const std::optional<model::RangeRef> ref = in.selection.referenceOverlay();
    if (!ref) {
        return -1;
    }
    for (std::size_t k = 1; k < in.slots.size(); ++k) {
        if (in.slots[k].slot.range == *ref) {
            return static_cast<int>(k);
        }
    }
    return -1;
}

bool anyOverlay(const ComponentBuildInputs& in) {
    return in.slots.size() > 1;
}

} // namespace

// ---- TimeDistanceBody
// ---------------------------------------------------------------------------------------

Result<TimeDistanceBody> TimeDistanceBody::parse(const model::OpaqueJson& body) {
    TimeDistanceBody out;
    out.rest = body;
    if (body.isNull()) {
        return out;
    }
    json j;
    try {
        j = json::parse(body.compact());
    } catch (const json::exception& e) {
        return fail(ErrorCode::Format, std::string("time_distance body: ") + e.what());
    }
    if (!j.is_object()) {
        ErrorContext ctx;
        ctx.detail = "";
        return fail(ErrorCode::Invalid, "time_distance body is not an object", ctx);
    }
    BodyError err;
    const auto invalid = [&err]() -> Failure {
        ErrorContext ctx;
        ctx.detail = err.pointer;
        return fail(ErrorCode::Invalid, "time_distance body: " + err.message + " at " + err.pointer, ctx);
    };
    if (const auto d = j.find("display"); d != j.end() && d->is_object()) {
        auto lineWidth = static_cast<double>(out.lineWidth);
        auto pointSize = static_cast<double>(out.pointSize);
        if (!readAs(*d, "traceStyle", out.traceStyle, "/display", err) ||
            !readNumber(*d, "lineWidth", lineWidth, "/display", err) ||
            !readNumber(*d, "pointSize", pointSize, "/display", err) ||
            !readAs(*d, "showGrid", out.showGrid, "/display", err) ||
            !readAs(*d, "showMarkers", out.showMarkers, "/display", err) ||
            !readAs(*d, "showBands", out.showBands, "/display", err) ||
            !readEnum(*d, "bandMode", out.bandMode, {"sections", "none"}, "/display", err) ||
            !readAs(*d, "bandGroup", out.bandGroup, "/display", err) ||
            !readEnum(*d, "overlapMode", out.overlapMode, {"panels", "overlapped"}, "/display", err) ||
            !readEnum(*d, "cursorStyle", out.cursorStyle, {"line", "crosshair", "none"}, "/display", err) ||
            !readAs(*d, "cursorSnap", out.cursorSnap, "/display", err) ||
            !readAs(*d, "showStatusStrip", out.showStatusStrip, "/display", err) ||
            !readAs(*d, "statusChannels", out.statusChannels, "/display", err) ||
            !readAs(*d, "showDeviceEvents", out.showDeviceEvents, "/display", err)) {
            return invalid();
        }
        out.lineWidth = static_cast<float>(lineWidth);
        out.pointSize = static_cast<float>(pointSize);
        if (const auto k = d->find("key"); k != d->end() && k->is_object()) {
            if (!readAs(*k, "mode", out.keyMode, "/display/key", err) ||
                !readAs(*k, "pos", out.keyPos, "/display/key", err) ||
                !readAs(*k, "values", out.keyValues, "/display/key", err) ||
                !readAs(*k, "stats", out.keyStats, "/display/key", err)) {
                return invalid();
            }
        }
        if (const auto v = d->find("variance"); v != d->end() && v->is_object()) {
            TdVariance& var = out.variance;
            std::optional<double> vMin;
            std::optional<double> vMax;
            std::string minUnit = var.min.unit;
            std::string maxUnit = var.max.unit;
            if (!readAs(*v, "show", var.show, "/display/variance", err) ||
                !readNumber(*v, "heightFrac", var.heightFrac, "/display/variance", err) ||
                !readAs(*v, "autoScale", var.autoScale, "/display/variance", err) ||
                !readValueWithUnit(*v, "min", vMin, minUnit, "/display/variance", err) ||
                !readValueWithUnit(*v, "max", vMax, maxUnit, "/display/variance", err)) {
                return invalid();
            }
            if (vMin) {
                var.min = analysis::UnitValue{*vMin, minUnit};
                var.hasMin = true;
            }
            if (vMax) {
                var.max = analysis::UnitValue{*vMax, maxUnit};
                var.hasMax = true;
            }
        }
        if (const auto a = d->find("axis"); a != d->end() && a->is_object()) {
            if (!readAs(*a, "showX", out.showXAxis, "/display/axis", err) ||
                !readAs(*a, "showY", out.showYAxis, "/display/axis", err) ||
                !readAs(*a, "showXScrollbar", out.showXScrollbar, "/display/axis", err) ||
                !readAs(*a, "showYScrollbar", out.showYScrollbar, "/display/axis", err)) {
                return invalid();
            }
        }
    }
    if (const auto groups = j.find("groups"); groups != j.end()) {
        if (!groups->is_array()) {
            err = BodyError{"/groups", "expected an array"};
            return invalid();
        }
        for (std::size_t gi = 0; gi < groups->size(); ++gi) {
            const json& gj = (*groups)[gi];
            const std::string ptr = "/groups/" + std::to_string(gi);
            if (!gj.is_object()) {
                err = BodyError{ptr, "expected an object"};
                return invalid();
            }
            TdGroup g;
            std::string mode = "channel";
            if (!readAs(gj, "name", g.name, ptr, err) || !readAs(gj, "scaleMode", mode, ptr, err) ||
                !readNumber(gj, "lo", g.lo, ptr, err) || !readNumber(gj, "hi", g.hi, ptr, err)) {
                return invalid();
            }
            g.scaleMode = mode == "manual" ? ScaleMode::Manual
                                           : (mode == "auto" ? ScaleMode::Auto : ScaleMode::Channel);
            if (const auto edges = gj.find("edges"); edges != gj.end()) {
                if (!edges->is_array() || edges->size() != 2 || !(*edges)[0].is_number() ||
                    !(*edges)[1].is_number()) {
                    err = BodyError{ptr + "/edges", "expected [top, bottom]"};
                    return invalid();
                }
                g.edgeTop = (*edges)[0].get<double>();
                g.edgeBottom = (*edges)[1].get<double>();
            }
            std::string minUnit;
            std::string maxUnit;
            if (!readValueWithUnit(gj, "scaleMin", g.scaleMin, minUnit, ptr, err) ||
                !readValueWithUnit(gj, "scaleMax", g.scaleMax, maxUnit, ptr, err)) {
                return invalid();
            }
            g.scaleUnit = !minUnit.empty() ? minUnit : maxUnit;
            if (const auto lines = gj.find("lines"); lines != gj.end() && lines->is_object()) {
                const std::string lptr = ptr + "/lines";
                if (!readScaleLine(*lines, "zero", g.zeroLine, g.zeroColour, lptr, err) ||
                    !readScaleLine(*lines, "min", g.minLine.enabled, g.minLine.colour, lptr, err) ||
                    !readScaleLine(*lines, "max", g.maxLine.enabled, g.maxLine.colour, lptr, err)) {
                    return invalid();
                }
            }
            if (const auto traces = gj.find("traces"); traces != gj.end()) {
                if (!traces->is_array()) {
                    err = BodyError{ptr + "/traces", "expected an array"};
                    return invalid();
                }
                for (std::size_t ti = 0; ti < traces->size(); ++ti) {
                    const json& tj = (*traces)[ti];
                    const std::string tptr = ptr + "/traces/" + std::to_string(ti);
                    if (!tj.is_object()) {
                        err = BodyError{tptr, "expected an object"};
                        return invalid();
                    }
                    TdTrace t;
                    if (!readAs(tj, "channel", t.channel, tptr, err) ||
                        !readAs(tj, "visible", t.visible, tptr, err) ||
                        !readAs(tj, "displayUnit", t.displayUnit, tptr, err)) {
                        return invalid();
                    }
                    if (const auto dps = tj.find("dps"); dps != tj.end() && !dps->is_null()) {
                        if (!dps->is_number_integer()) {
                            err = BodyError{tptr + "/dps", "expected an integer"};
                            return invalid();
                        }
                        t.dps = dps->get<int>();
                    }
                    g.traces.push_back(std::move(t));
                }
            }
            out.groups.push_back(std::move(g));
        }
    }
    return out;
}

model::OpaqueJson TimeDistanceBody::toJson() const {
    json j = json::object();
    if (!rest.isNull()) {
        try {
            j = json::parse(rest.compact());
        } catch (const json::exception&) {
            j = json::object();
        }
        if (!j.is_object()) {
            j = json::object();
        }
    }
    const TimeDistanceBody defaults;
    {
        json display = j.contains("display") && j["display"].is_object() ? j["display"] : json::object();
        put(display, "traceStyle", traceStyle, defaults.traceStyle);
        put(display, "lineWidth", static_cast<double>(lineWidth), static_cast<double>(defaults.lineWidth));
        put(display, "pointSize", static_cast<double>(pointSize), static_cast<double>(defaults.pointSize));
        put(display, "showGrid", showGrid, defaults.showGrid);
        put(display, "showMarkers", showMarkers, defaults.showMarkers);
        put(display, "showBands", showBands, defaults.showBands);
        put(display, "bandMode", bandMode, defaults.bandMode);
        put(display, "bandGroup", bandGroup, defaults.bandGroup);
        put(display, "overlapMode", overlapMode, defaults.overlapMode);
        put(display, "cursorStyle", cursorStyle, defaults.cursorStyle);
        put(display, "cursorSnap", cursorSnap, defaults.cursorSnap);
        put(display, "showStatusStrip", showStatusStrip, defaults.showStatusStrip);
        put(display, "statusChannels", statusChannels, defaults.statusChannels);
        put(display, "showDeviceEvents", showDeviceEvents, defaults.showDeviceEvents);
        json key = display.contains("key") && display["key"].is_object() ? display["key"] : json::object();
        put(key, "mode", keyMode, defaults.keyMode);
        put(key, "pos", keyPos, defaults.keyPos);
        put(key, "values", keyValues, defaults.keyValues);
        put(key, "stats", keyStats, defaults.keyStats);
        if (!key.empty()) {
            display["key"] = key;
        }
        json var = childOf(display, "variance");
        put(var, "show", variance.show, defaults.variance.show);
        put(var, "heightFrac", variance.heightFrac, defaults.variance.heightFrac);
        put(var, "autoScale", variance.autoScale, defaults.variance.autoScale);
        if (var.contains("min") || variance.hasMin || !(variance.min == defaults.variance.min)) {
            var["min"] = valueWithUnit(variance.min.v, variance.min.unit);
        }
        if (var.contains("max") || variance.hasMax || !(variance.max == defaults.variance.max)) {
            var["max"] = valueWithUnit(variance.max.v, variance.max.unit);
        }
        if (display.contains("variance") || !var.empty()) {
            display["variance"] = var;
        }
        json axis =
            display.contains("axis") && display["axis"].is_object() ? display["axis"] : json::object();
        put(axis, "showX", showXAxis, defaults.showXAxis);
        put(axis, "showY", showYAxis, defaults.showYAxis);
        put(axis, "showXScrollbar", showXScrollbar, defaults.showXScrollbar);
        put(axis, "showYScrollbar", showYScrollbar, defaults.showYScrollbar);
        if (!axis.empty()) {
            display["axis"] = axis;
        }
        if (!display.empty() || j.contains("display")) {
            j["display"] = display;
        }
    }
    if (!groups.empty() || j.contains("groups")) {
        const json oldGroups = j.contains("groups") && j["groups"].is_array() ? j["groups"] : json::array();
        json newGroups = json::array();
        for (std::size_t gi = 0; gi < groups.size(); ++gi) {
            const TdGroup& g = groups[gi];
            json gj = gi < oldGroups.size() && oldGroups[gi].is_object() ? oldGroups[gi] : json::object();
            const TdGroup gd;
            put(gj, "name", g.name, gd.name);
            put(gj, "scaleMode", std::string(scaleModeName(g.scaleMode)),
                std::string(scaleModeName(gd.scaleMode)));
            if (gj.contains("edges") || g.edgeTop != gd.edgeTop || g.edgeBottom != gd.edgeBottom) {
                gj["edges"] = json::array({g.edgeTop, g.edgeBottom});
            }
            put(gj, "lo", g.lo, gd.lo);
            put(gj, "hi", g.hi, gd.hi);
            const bool bareNumbers = gj.contains("scaleMin") && gj["scaleMin"].is_number();
            if (g.scaleMin) {
                gj["scaleMin"] = bareNumbers ? json(*g.scaleMin) : valueWithUnit(*g.scaleMin, g.scaleUnit);
            } else {
                gj.erase("scaleMin");
            }
            if (g.scaleMax) {
                gj["scaleMax"] = bareNumbers ? json(*g.scaleMax) : valueWithUnit(*g.scaleMax, g.scaleUnit);
            } else {
                gj.erase("scaleMax");
            }
            json lines = childOf(gj, "lines");
            putScaleLine(lines, "zero", g.zeroLine, gd.zeroLine, g.zeroColour);
            putScaleLine(lines, "min", g.minLine.enabled, gd.minLine.enabled, g.minLine.colour);
            putScaleLine(lines, "max", g.maxLine.enabled, gd.maxLine.enabled, g.maxLine.colour);
            if (gj.contains("lines") || !lines.empty()) {
                gj["lines"] = std::move(lines);
            }
            const json oldTraces =
                gj.contains("traces") && gj["traces"].is_array() ? gj["traces"] : json::array();
            json newTraces = json::array();
            for (std::size_t ti = 0; ti < g.traces.size(); ++ti) {
                const TdTrace& t = g.traces[ti];
                json tj = ti < oldTraces.size() && oldTraces[ti].is_object() ? oldTraces[ti] : json::object();
                const TdTrace td;
                tj["channel"] = t.channel;
                put(tj, "visible", t.visible, td.visible);
                put(tj, "displayUnit", t.displayUnit, td.displayUnit);
                if (t.dps) {
                    tj["dps"] = *t.dps;
                } else {
                    tj.erase("dps");
                }
                newTraces.push_back(std::move(tj));
            }
            if (!newTraces.empty() || gj.contains("traces")) {
                gj["traces"] = std::move(newTraces);
            }
            newGroups.push_back(std::move(gj));
        }
        j["groups"] = std::move(newGroups);
    }
    Result<model::OpaqueJson> out = model::OpaqueJson::parse(j.dump());
    if (out) {
        return std::move(*out);
    }
    return rest;
}

std::vector<std::string> TimeDistanceBody::channelNames() const {
    std::vector<std::string> names;
    for (const TdGroup& g : groups) {
        for (const TdTrace& t : g.traces) {
            names.push_back(t.channel);
        }
    }
    return names;
}

// ---- layout
// -------------------------------------------------------------------------------------------------

TdLayout TimeDistanceBuilder::layoutOf(const ComponentBuildInputs& inputs) {
    TdLayout lay;
    const float dpr = dprOf(inputs);
    lay.dpr = dpr;
    const auto w = static_cast<float>(std::max(inputs.layout.widthPx, 0));
    const auto h = static_cast<float>(std::max(inputs.layout.heightPx, 0));
    lay.bounds = RectPx{0.0F, 0.0F, w, h};
    const TimeDistanceBody body = bodyOf(inputs);
    const project::Scheme& scheme = schemeOf(inputs);
    const ITextMetrics& metrics = metricsOf(inputs);

    float top = 0.0F;
    if (inputs.spec.caption.show) {
        lay.caption = RectPx{0.0F, 0.0F, w, std::min(h, snap(kCaptionLogical * dpr))};
        top = lay.caption.h;
    }
    const float xAxisH = body.showXAxis ? snap(kXAxisLogical * dpr) : 0.0F;
    const float axisW = body.showYAxis ? snap(kAxisStripLogical * dpr) : 0.0F;
    const float topMargin = snap(kTopMarginLogical * dpr);
    const float rightMargin = snap(kRightMarginLogical * dpr);
    const float scrollH = body.showXScrollbar ? snap(kScrollbarLogical * dpr) : 0.0F;
    const float offsetRowH = inputs.view.offsetAxis ? snap(kOffsetRowLogical * dpr) : 0.0F;
    lay.overlapped = body.overlapped();
    lay.referenceSlot = referenceSlotOf(inputs);

    // Key rows: one per visible trace, values per visible slot (templates here; Dynamic fills them); with a
    // datum the datum and delta cells, with measurements (M) one measurement line per slot (templates here;
    // the Static build fills them).
    const bool showKey = body.keyMode != "none";
    lay.keyColumns.values = body.keyValues;
    lay.keyColumns.datum = body.keyValues && inputs.view.datumT.has_value();
    lay.keyColumns.measurements = body.keyStats;
    const bool defaultColumns = !lay.keyColumns.datum && !lay.keyColumns.measurements;
    const std::vector<FlatTrace> flat = flatTraces(body);
    std::vector<KeyRow> allRows;
    std::vector<std::vector<KeyRow>> groupRows(body.groups.size());
    std::vector<std::vector<int>> groupTraces(body.groups.size());
    for (const FlatTrace& ft : flat) {
        if (!ft.trace->visible) {
            continue;
        }
        const ResolvedChannel* mainRc =
            inputs.slots.empty() ? nullptr : channelOf(inputs.slots.front(), ft.index, ft.trace->channel);
        KeyRow row;
        row.label = labelOf(*ft.trace, mainRc);
        row.missing = mainRc == nullptr || !mainRc->descriptor;
        row.group = ft.group;
        row.trace = ft.index;
        row.active = ft.trace->channel == inputs.spec.activeChannel;
        const int colourIndex = colourIndexOf(mainRc, ft.index);
        row.colour = traceColour(scheme, inputs.selection, slotOf(inputs, 0), colourIndex);
        const std::string tmpl = valueTemplate(dpsOf(*ft.trace, mainRc));
        const std::size_t lines = std::max<std::size_t>(1, inputs.slots.size());
        for (std::size_t k = 0; k < lines; ++k) {
            const ResolvedChannel* rc =
                k < inputs.slots.size() ? channelOf(inputs.slots[k], ft.index, ft.trace->channel) : nullptr;
            const Rgba c =
                traceColour(scheme, inputs.selection, slotOf(inputs, k), colourIndexOf(rc, ft.index));
            if (body.keyValues) {
                row.slotValues.push_back(KeyValue{tmpl, c});
                if (lay.keyColumns.datum) {
                    row.datumValues.push_back(KeyValue{tmpl, c});
                    row.deltaValues.push_back(KeyValue{tmpl, c});
                }
            }
            if (lay.keyColumns.measurements && k < inputs.slots.size()) {
                KeyMeasurement m;
                m.min = KeyValue{tmpl, c};
                m.max = KeyValue{tmpl, c};
                m.avg = KeyValue{tmpl, c};
                row.measurements.push_back(std::move(m));
            }
        }
        allRows.push_back(row);
        groupRows[static_cast<std::size_t>(ft.group)].push_back(row);
        groupTraces[static_cast<std::size_t>(ft.group)].push_back(ft.index);
    }
    const float rowH = metrics.lineHeightPx(TextStyleId::Key, dpr);
    KeyGeometry all;
    float keyW = 0.0F;
    if (showKey) {
        all = defaultColumns ? KeyLayout::layout(allRows, metrics, w, rowH, dpr)
                             : KeyLayout::layout(allRows, metrics, w, rowH, dpr, lay.keyColumns);
        keyW = snap(all.widthPx);
    }
    const bool keyRight = body.keyPos == "right";
    const float plotTop = top;
    const float traceX = (keyRight ? 0.0F : keyW) + axisW;
    const float traceRight = keyRight ? w - keyW - rightMargin : w - rightMargin;
    const float traceW = std::max(0.0F, traceRight - traceX);

    // The bands over the trace area (04 section 8.1), under the caption.
    lay.bandFlags.showRanges = body.showBands;
    lay.bandFlags.showSections = body.showBands && body.bandMode == "sections";
    lay.bandFlags.showMarkers = body.showMarkers;
    lay.bands = BandLayout::layout(lay.bandFlags, RectPx{traceX, plotTop, traceW, 0.0F}, dpr);
    const float bandsH = lay.bands.heightPx;

    // The bottom stack: x axis, offset axis, scrollbar; above them the variance panel and the status strip.
    const float bottomFixed = xAxisH + 2.0F * offsetRowH + scrollH;
    const float plotBottom = std::max(plotTop, h - bottomFixed);
    const float traceTop = std::min(plotBottom, plotTop + bandsH + topMargin);
    const float available = plotBottom - traceTop;
    lay.statusChannels = statusChannelsOf(inputs, body, flat.size());
    lay.statusRowHeight = snap(kStatusRowLogical * dpr);
    const float statusH = std::min(std::floor(0.5F * available),
                                   lay.statusRowHeight * static_cast<float>(lay.statusChannels.size()));
    const bool varianceShown = body.variance.show && anyOverlay(inputs);
    const float varianceH =
        varianceShown ? snap(static_cast<float>(std::clamp(body.variance.heightFrac, 0.0, 0.9)) * available)
                      : 0.0F;
    const float traceBottom = std::max(traceTop, plotBottom - statusH - varianceH);

    lay.keyColumn =
        keyW > 0.0F ? RectPx{keyRight ? w - keyW : 0.0F, plotTop, keyW, traceBottom - plotTop} : RectPx{};
    lay.traceArea = RectPx{traceX, traceTop, traceW, traceBottom - traceTop};
    lay.traceWidthPx = static_cast<int>(lay.traceArea.w);
    if (statusH > 0.0F) {
        lay.statusStrip = RectPx{traceX, traceBottom, traceW, statusH};
    }
    if (varianceH > 0.0F) {
        lay.variance = RectPx{traceX, traceBottom + statusH, traceW, varianceH};
        lay.varianceAxis = RectPx{traceX - axisW, lay.variance.y, axisW, varianceH};
    }
    lay.xAxis = xAxisH > 0.0F ? RectPx{traceX, plotBottom, lay.traceArea.w, xAxisH} : RectPx{};
    if (offsetRowH > 0.0F) {
        lay.offsetMain = RectPx{traceX, plotBottom + xAxisH, traceW, offsetRowH};
        lay.offsetReference = RectPx{traceX, plotBottom + xAxisH + offsetRowH, traceW, offsetRowH};
    }
    if (scrollH > 0.0F) {
        lay.scrollbar = RectPx{traceX, h - scrollH, traceW, scrollH};
    }

    // The active group: the active channel's, else the first.
    const std::size_t n = body.groups.size();
    for (const FlatTrace& ft : flat) {
        if (ft.trace->channel == inputs.spec.activeChannel) {
            lay.activeGroup = ft.group;
            break;
        }
    }
    if (lay.activeGroup < 0 && n > 0) {
        lay.activeGroup = 0;
    }

    // Panels by edges (equal shares when every group keeps the default [0, 1]); overlapped: each group's
    // [lo, hi] region of the whole area.
    const bool allDefault = std::all_of(body.groups.begin(), body.groups.end(), [](const TdGroup& g) {
        return g.edgeTop == 0.0 && g.edgeBottom == 1.0;
    });
    float overlappedKeyY = plotTop;
    const float handle = snap(kHandleLogical * dpr);
    for (std::size_t g = 0; g < n; ++g) {
        double e0 = std::clamp(lay.overlapped ? body.groups[g].lo : body.groups[g].edgeTop, 0.0, 1.0);
        double e1 = std::clamp(lay.overlapped ? body.groups[g].hi : body.groups[g].edgeBottom, 0.0, 1.0);
        if (!lay.overlapped && allDefault && n > 1) {
            e0 = static_cast<double>(g) / static_cast<double>(n);
            e1 = static_cast<double>(g + 1) / static_cast<double>(n);
        }
        if (e1 < e0) {
            std::swap(e0, e1);
        }
        TdGroupLayout gl;
        gl.group = static_cast<int>(g);
        const float y0 = lay.traceArea.y + snap(static_cast<float>(e0) * lay.traceArea.h);
        const float y1 = lay.traceArea.y + snap(static_cast<float>(e1) * lay.traceArea.h);
        gl.panel = RectPx{lay.traceArea.x, y0, lay.traceArea.w, y1 - y0};
        gl.axisStrip = RectPx{traceX - axisW, y0, axisW, y1 - y0};
        gl.rows = std::move(groupRows[g]);
        gl.traces = std::move(groupTraces[g]);
        if (showKey) {
            gl.keyGeometry = defaultColumns
                                 ? KeyLayout::layout(gl.rows, metrics, w, rowH, dpr)
                                 : KeyLayout::layout(gl.rows, metrics, w, rowH, dpr, lay.keyColumns);
            gl.keyGeometry.widthPx = keyW;
            gl.keyGeometry.labelWidthPx = all.labelWidthPx + (keyW - all.widthPx);
            gl.keyGeometry.valueWidthPx = all.valueWidthPx;
            gl.keyGeometry.datumWidthPx = all.datumWidthPx;
            gl.keyGeometry.deltaWidthPx = all.deltaWidthPx;
            gl.keyGeometry.measureWidthPx = all.measureWidthPx;
            for (RectPx& r : gl.keyGeometry.rowRects) {
                r.w = keyW;
                r.y += snap(2.0F * dpr);
            }
        }
        if (lay.overlapped) {
            // The key lists the groups one under the other.
            float rowsH = 0.0F;
            for (const RectPx& r : gl.keyGeometry.rowRects) {
                rowsH = std::max(rowsH, r.y + r.h);
            }
            const float keyH =
                std::max(0.0F, std::min(rowsH + snap(2.0F * dpr), traceBottom - overlappedKeyY));
            gl.key = RectPx{lay.keyColumn.x, overlappedKeyY, keyW, keyH};
            overlappedKeyY += keyH;
            if (gl.panel.w > handle) {
                const float hx = gl.panel.x + gl.panel.w - handle;
                gl.loHandle = RectPx{hx, y0, handle, std::min(handle, std::max(1.0F, y1 - y0))};
                gl.hiHandle =
                    RectPx{hx, std::max(y0, y1 - handle), handle, std::min(handle, std::max(1.0F, y1 - y0))};
            }
        } else {
            const float keyY = g == 0 ? plotTop : y0; // the first group's key starts under the caption
            gl.key = RectPx{lay.keyColumn.x, keyY, keyW, y1 - keyY};
        }
        if (body.showYScrollbar && axisW > 0.0F && static_cast<int>(g) == lay.activeGroup &&
            gl.panel.h > 0.0F) {
            gl.vScroll = RectPx{gl.axisStrip.x + snap(dpr), y0, snap(kVScrollLogical * dpr), y1 - y0};
        }
        lay.groups.push_back(std::move(gl));
    }
    return lay;
}

std::pair<double, double> TimeDistanceBuilder::zoomedScale(double vMin, double vMax,
                                                           const TdGroupZoom& zoom) noexcept {
    const double lo = std::clamp(std::min(zoom.lo, zoom.hi), 0.0, 1.0);
    const double hi = std::clamp(std::max(zoom.lo, zoom.hi), 0.0, 1.0);
    if (!(hi > lo) || (lo == 0.0 && hi == 1.0)) {
        return {vMin, vMax};
    }
    const double span = vMax - vMin;
    return {vMin + lo * span, vMin + hi * span};
}

std::string TimeDistanceBuilder::offsetText(Domain domain, const RangeGeom& range) {
    const bool time = domain == Domain::Time;
    std::string text = time ? detail::formatFixed(static_cast<double>(range.offsetUs) / 1e6, 3)
                            : detail::formatFixed(range.offsetM, 2);
    if (text.empty()) {
        text = "0";
    }
    if (text.front() != '-') {
        text.insert(text.begin(), '+');
    }
    return text + (time ? " s" : " m");
}

// ---- Static
// -------------------------------------------------------------------------------------------------

namespace {

struct Scale {
    double vMin = std::numeric_limits<double>::quiet_NaN();
    double vMax = std::numeric_limits<double>::quiet_NaN();
    bool valid() const noexcept { return std::isfinite(vMin) && std::isfinite(vMax); }
};

void unionScale(Scale& s, double lo, double hi) {
    if (!std::isfinite(lo) || !std::isfinite(hi)) {
        return;
    }
    if (!s.valid()) {
        s.vMin = lo;
        s.vMax = hi;
        return;
    }
    s.vMin = std::min(s.vMin, lo);
    s.vMax = std::max(s.vMax, hi);
}

// base -> axis unit for a manual scale persisted in `unit`.
std::optional<double> manualToAxis(double v, const std::string& unit, const ResolvedChannel& axis,
                                   const units::UnitRegistry* registry) {
    if (unit.empty()) {
        return axis.toDisplay(v); // base units
    }
    if (unit == axis.displayUnit) {
        return v;
    }
    if (registry != nullptr) {
        if (const auto id = registry->find(unit)) {
            return axis.toDisplay(registry->toBase(*id)(v));
        }
    }
    return v; // an unknown unit: taken in the axis unit
}

struct DrawnTrace {
    std::size_t job = 0;
    int group = 0;
    int flatIndex = 0;
    std::size_t slot = 0;
    bool active = false;
};

// The panel's y mapping of a group: its scale over the panel rectangle (identity scale while invalid).
YMap yMapOf(const RectPx& panel, const Scale& s) {
    YMap ymap;
    ymap.yTopPx = panel.y;
    ymap.yBottomPx = panel.y + panel.h;
    ymap.vMin = s.valid() ? s.vMin : 0.0;
    ymap.vMax = s.valid() ? s.vMax : 1.0;
    return ymap;
}

// Appends a list of vertex commands verbatim (vertex offsets rebased, coordinates copied bit for bit): the
// trace sub-lists emitted in parallel join the Static list exactly as if emitted in place.
void appendVertexCommands(DrawList& list, const DrawList& part) {
    const auto base = static_cast<std::uint32_t>(list.verts.size() / 2);
    list.verts.insert(list.verts.end(), part.verts.begin(), part.verts.end());
    for (DrawCmd cmd : part.cmds) {
        cmd.vertexOffset += base;
        list.cmds.push_back(cmd);
    }
}

std::vector<AxisTick> filterHorizontal(std::vector<AxisTick> ticks, const ITextMetrics& metrics, float dpr,
                                       float minPx, float maxPx) {
    std::vector<AxisTick> out;
    for (AxisTick& t : ticks) {
        const float half = 0.5F * metrics.advancePx(TextStyleId::Axis, t.label, dpr);
        if (t.posPx - half >= minPx && t.posPx + half <= maxPx) {
            out.push_back(std::move(t));
        }
    }
    return out;
}

std::vector<AxisTick> filterVertical(std::vector<AxisTick> ticks, float lineH, float minPx, float maxPx) {
    std::vector<AxisTick> out;
    for (AxisTick& t : ticks) {
        if (t.posPx - 0.5F * lineH >= minPx && t.posPx + 0.5F * lineH <= maxPx) {
            out.push_back(std::move(t));
        }
    }
    return out;
}

AxisSpec xAxisSpec(const ComponentBuildInputs& in, const TdLayout& lay) {
    AxisSpec spec;
    spec.kind = in.view.domain == Domain::Time ? AxisKind::Time : AxisKind::Distance;
    spec.lo = in.view.zoom.a;
    spec.hi = in.view.zoom.b;
    spec.lengthPx = static_cast<float>(lay.traceWidthPx);
    spec.vertical = false;
    spec.timeFormat = TimeFormat::MinutesSeconds;
    spec.unit = in.view.domain == Domain::Distance ? "m" : "";
    return spec;
}

// The group's scale from manual limits, override limits and the Main source's summaries (no samples read);
// `fallback` is set when a summary is incomplete (the caller then unites the drawn columns).
Scale scaleWithoutData(const ComponentBuildInputs& in, const TimeDistanceBody& body, const TdGroupLayout& gl,
                       const ResolvedChannel* axis, bool& fallback) {
    Scale s;
    fallback = false;
    const TdGroup& group = body.groups[static_cast<std::size_t>(gl.group)];
    if (axis == nullptr) {
        return s;
    }
    if (group.scaleMode == ScaleMode::Manual && group.scaleMin && group.scaleMax) {
        const auto lo = manualToAxis(*group.scaleMin, group.scaleUnit, *axis, in.units.get());
        const auto hi = manualToAxis(*group.scaleMax, group.scaleUnit, *axis, in.units.get());
        if (lo && hi) {
            unionScale(s, std::min(*lo, *hi), std::max(*lo, *hi));
        }
    }
    if (s.valid()) {
        return s;
    }
    const std::vector<std::string> names = body.channelNames();
    fallback = in.slots.empty();
    for (std::size_t r = 0; r < gl.traces.size() && !fallback; ++r) {
        const int flatIndex = gl.traces[r];
        const std::string& name = names[static_cast<std::size_t>(flatIndex)];
        const ResolvedChannel* rc = channelOf(in.slots.front(), flatIndex, name);
        if (rc == nullptr || !rc->descriptor || !rc->series) {
            continue; // a channel missing in the Main source does not scale the group
        }
        std::optional<double> limitLo;
        std::optional<double> limitHi;
        if (group.scaleMode == ScaleMode::Channel) {
            if (rc->limitMin) {
                limitLo = axis->toDisplay(*rc->limitMin);
            }
            if (rc->limitMax) {
                limitHi = axis->toDisplay(*rc->limitMax);
            }
        }
        if (limitLo && limitHi) {
            unionScale(s, std::min(*limitLo, *limitHi), std::max(*limitLo, *limitHi));
            continue;
        }
        const std::shared_ptr<const store::SummaryPyramid> summary =
            store::seriesValues(*rc->series).summarySnapshot();
        bool covered = false;
        const store::MinMax mm = summary && summary->size() > 0
                                     ? summary->minmax(0, summary->size() - 1, &covered)
                                     : store::MinMax::empty();
        if (!covered || summary == nullptr || summary->size() == 0) {
            fallback = true;
            break;
        }
        if (mm.isEmpty()) {
            continue;
        }
        double a = axis->toDisplay(static_cast<double>(mm.lo));
        double b = axis->toDisplay(static_cast<double>(mm.hi));
        if (a > b) {
            std::swap(a, b);
        }
        unionScale(s, limitLo.value_or(a), limitHi.value_or(b));
    }
    if (fallback) {
        s = Scale{};
    }
    return s;
}

// min / max / avg of the valid samples of a channel over the visible span (04 section 8.7), in base units,
// read on the worker (Column::require, one chunk at a time).
struct Measured {
    double min = std::numeric_limits<double>::infinity();
    double max = -std::numeric_limits<double>::infinity();
    double sum = 0.0;
    std::int64_t count = 0;
    Micros tMin = 0;
    Micros tMax = 0;
    void add(double v, Micros t) noexcept {
        if (!std::isfinite(v)) {
            return;
        }
        if (v < min) {
            min = v;
            tMin = t;
        }
        if (v > max) {
            max = v;
            tMax = t;
        }
        sum += v;
        ++count;
    }
};

Result<Measured> measureSpan(const store::Series& series, Micros t0, Micros t1, const CancelToken& cancel) {
    Measured m;
    if (t1 < t0) {
        std::swap(t0, t1);
    }
    if (const auto* u = std::get_if<store::UniformSeries>(&series)) {
        const SampleIdx n = u->values.size();
        if (n <= 0 || !u->rate.valid()) {
            return m;
        }
        SampleIdx i0 = t0 <= u->t0 ? 0 : u->rate.indexAt(t0, u->t0);
        if (u->rate.timeAt(i0, u->t0) < t0) {
            ++i0;
        }
        SampleIdx i1 = t1 < u->t0 ? -1 : u->rate.indexAt(t1, u->t0);
        if (i1 >= 0 && u->rate.timeAt(i1, u->t0) >= t1 && i1 > i0) {
            --i1; // half-open [t0, t1)
        }
        i0 = std::max<SampleIdx>(0, i0);
        i1 = std::min<SampleIdx>(n - 1, i1);
        for (SampleIdx c = store::chunkOf(i0); i0 <= i1 && c <= store::chunkOf(i1); ++c) {
            if (cancel.checkpoint()) {
                return fail(ErrorCode::Cancelled, "measurement cancelled");
            }
            const SampleIdx first = std::max(i0, c << store::kChunkShift);
            const SampleIdx last = std::min(i1, ((c + 1) << store::kChunkShift) - 1);
            Result<store::ChunkHandle> pin = u->values.require(first, last, Priority::Visible, cancel);
            if (!pin) {
                return Failure{pin.error()};
            }
            for (SampleIdx i = first; i <= last; ++i) {
                m.add(pin->at(i), u->rate.timeAt(i, u->t0));
            }
        }
        return m;
    }
    if (const auto* ts = std::get_if<store::TimestampedSeries>(&series)) {
        const SampleIdx n = ts->values.size();
        if (n <= 0 || ts->timesUs.size() != n) {
            return m;
        }
        Result<store::ChunkHandle> times = ts->timesUs.require(0, n - 1, Priority::Visible, cancel);
        if (!times) {
            return Failure{times.error()};
        }
        Result<store::ChunkHandle> values = ts->values.require(0, n - 1, Priority::Visible, cancel);
        if (!values) {
            return Failure{values.error()};
        }
        for (SampleIdx i = 0; i < n; ++i) {
            const std::optional<std::int64_t> t = times->int64At(i);
            if (t && *t >= t0 && *t < t1) {
                m.add(values->at(i), *t);
            }
        }
    }
    return m;
}

// The band contents of the visible span (04 section 8.11): the ranges of the visible slots and the other laps
// of the Main source's group, the sections of the body's band group, beacons, device events and annotations.
void fillBands(const ComponentBuildInputs& in, const TimeDistanceBody& body, const TdLayout& lay,
               const std::vector<FlatTrace>& flat, const Colours& colours, BandLayoutInput& input,
               TdStaticInfo& info, std::vector<float>& annotationXs) {
    if (in.slots.empty() || lay.traceWidthPx <= 0) {
        return;
    }
    const SlotData& main = in.slots.front();
    if (!slotDrawsX(in, main)) {
        return;
    }
    const DomainMapper mainMap = mapperOf(in, main, nullptr);
    const auto width = static_cast<double>(lay.traceWidthPx);
    const auto traceX = static_cast<double>(lay.traceArea.x);
    const auto pxOf = [&](double x) {
        return static_cast<float>(DomainMapper::pxOf(x, in.view.zoom, traceX, width) - traceX);
    };
    const auto visible = [&](double x0, double x1) {
        return std::max(x0, x1) > in.view.zoom.a && std::min(x0, x1) < in.view.zoom.b;
    };
    const model::DataLayer* layer = layerOf(main);
    if (input.showRanges) {
        // Other ranges of the Main range's group (the previous and next laps when zoomed out), then the slots
        // (overlays n..1, Overlay 0, Main on top).
        if (layer != nullptr) {
            if (const model::RangeGroup* g = layer->ranges.find(main.slot.range.groupPath)) {
                for (const model::Range& r : g->ranges) {
                    if (!r.enabled || r.id == main.slot.range.range) {
                        continue;
                    }
                    const double x0 = mainMap.xOf(r.start);
                    const double x1 = mainMap.xOf(r.end);
                    if (!visible(x0, x1)) {
                        continue;
                    }
                    BandCell cell;
                    cell.x0 = pxOf(x0);
                    cell.x1 = pxOf(x1);
                    cell.label = r.name;
                    cell.index = static_cast<int>(info.ranges.size());
                    input.ranges.push_back(std::move(cell));
                    info.ranges.push_back(TdBandEntry{x0, x1, r.name, -1});
                }
            }
        }
        std::set<std::string> sources;
        for (const SlotData& s : in.slots) {
            sources.insert(s.slot.range.src.hex());
        }
        struct SlotCell {
            std::size_t k = 0;
            double x0 = 0.0;
            double x1 = 0.0;
            std::string label;
        };
        std::vector<SlotCell> cells;
        for (std::size_t k = 0; k < in.slots.size(); ++k) {
            const SlotData& s = in.slots[k];
            if (!slotDrawsX(in, s)) {
                continue;
            }
            const DomainMapper map = mapperOf(in, s, nullptr);
            SlotCell c{k, map.xOf(s.range.start), map.xOf(s.range.end), s.slot.range.name};
            if (sources.size() > 1) {
                const std::string shortName = sourceShortName(s);
                if (!shortName.empty()) {
                    c.label += " (" + shortName + ")";
                }
            }
            cells.push_back(std::move(c));
        }
        // Cells that mostly lie under Main's (overlays aligned at the lap start) lend Main their names: one
        // label "Main | Overlay ..." instead of names written over each other.
        std::vector<std::string> drawnLabels;
        drawnLabels.reserve(cells.size());
        for (const SlotCell& c : cells) {
            drawnLabels.push_back(c.label);
        }
        if (!cells.empty() && cells.front().k == 0) {
            const SlotCell& mainCell = cells.front();
            for (std::size_t i = 1; i < cells.size(); ++i) {
                const double overlap =
                    std::min(mainCell.x1, cells[i].x1) - std::max(mainCell.x0, cells[i].x0);
                const double shorter = std::min(mainCell.x1 - mainCell.x0, cells[i].x1 - cells[i].x0);
                if (shorter > 0.0 && overlap >= 0.5 * shorter) {
                    drawnLabels.front() += " | " + cells[i].label;
                    drawnLabels[i].clear();
                }
            }
        }
        for (std::size_t i = cells.size(); i-- > 0;) {
            const SlotCell& c = cells[i];
            BandCell cell;
            cell.x0 = pxOf(c.x0);
            cell.x1 = pxOf(c.x1);
            cell.label = drawnLabels[i];
            cell.colour = c.k == 0 ? colours.rangeMark : geom::withAlpha(keyColourOf(in, flat, c.k), 0x40);
            cell.slot = static_cast<int>(c.k);
            cell.index = static_cast<int>(info.ranges.size());
            info.ranges.push_back(TdBandEntry{c.x0, c.x1, c.label, static_cast<int>(c.k)});
            input.ranges.push_back(std::move(cell));
        }
    }
    if (input.showSections && layer != nullptr) {
        if (const model::RangeGroup* g = layer->ranges.find(body.bandGroup)) {
            // A section's category is the child group that holds it (Corners, Straights, or a user category).
            std::map<model::RangeId, std::string> category;
            for (const model::RangeGroup& child : g->children) {
                const std::string c = child.name == "Corners"     ? std::string("corner")
                                      : child.name == "Straights" ? std::string("straight")
                                                                  : child.name;
                for (const model::Range& r : child.ranges) {
                    category[r.id] = c;
                }
            }
            for (const model::Range& r : g->ranges) {
                if (!r.enabled) {
                    continue;
                }
                const double x0 = mainMap.xOf(r.start);
                const double x1 = mainMap.xOf(r.end);
                if (!visible(x0, x1)) {
                    continue;
                }
                BandCell cell;
                cell.x0 = pxOf(x0);
                cell.x1 = pxOf(x1);
                cell.label = r.name;
                if (const auto it = category.find(r.id); it != category.end()) {
                    cell.category = it->second;
                }
                cell.index = static_cast<int>(info.sections.size());
                input.sections.push_back(std::move(cell));
                info.sections.push_back(TdBandEntry{x0, x1, r.name, 0});
            }
        }
    }
    if (input.showMarkers) {
        const float margin = 8.0F * lay.dpr;
        const auto inBand = [&](float px) {
            return px >= -margin && px <= static_cast<float>(width) + margin;
        };
        if (layer != nullptr) {
            for (const model::Marker& m : layer->markers) {
                if (!m.enabled) {
                    continue;
                }
                const bool beacon = m.cls == model::MarkerClass::BCN || m.cls == model::MarkerClass::SOL ||
                                    m.cls == model::MarkerClass::EOL;
                const bool event = m.cls == model::MarkerClass::DeviceEvent && body.showDeviceEvents;
                if (!beacon && !event) {
                    continue;
                }
                const double x = mainMap.xOf(m.t);
                const float px = pxOf(x);
                if (!inBand(px)) {
                    continue;
                }
                BandMarker bm;
                bm.x = px;
                bm.id = m.id;
                if (beacon) {
                    bm.kind = BandMarkerKind::Beacon;
                    bm.text = std::string(model::markerClassName(m.cls));
                    if (!m.name.empty()) {
                        bm.text += " " + m.name;
                    }
                } else {
                    bm.kind = BandMarkerKind::DeviceEvent;
                    bm.text = !m.comment.empty() ? m.comment : m.name;
                    bm.colour = colours.deviceEvent;
                }
                info.markers.push_back(TdMarkerEntry{bm.id, bm.kind, bm.text, x, m.t});
                input.markers.push_back(std::move(bm));
            }
        }
        // Annotations (04 section 8.11 item 4): the Main source's, at its absolute time through the Main
        // mapper.
        if (main.source != nullptr && main.source->sidecar.hasLayer(main.layer)) {
            for (const model::Annotation& a : main.source->sidecar.layer(main.layer).annotations) {
                const double x = mainMap.xOf(a.tUs);
                const float px = pxOf(x);
                if (!inBand(px)) {
                    continue;
                }
                BandMarker bm;
                bm.x = px;
                bm.kind = BandMarkerKind::Annotation;
                bm.id = model::MarkerId{a.id};
                bm.text = a.text;
                bm.colour = colours.annotation;
                info.markers.push_back(TdMarkerEntry{bm.id, bm.kind, bm.text, x, a.tUs});
                input.markers.push_back(std::move(bm));
                if (px >= 0.0F && px <= static_cast<float>(width)) {
                    annotationXs.push_back(std::round(lay.traceArea.x + px) + 0.5F);
                }
            }
        }
    }
}

// A filled triangle (two of a Quads command's four vertices coincide).
void triangle(DrawList& list, float x0, float y0, float x1, float y1, float x2, float y2, Rgba colour) {
    const std::array<float, 8> q{x0, y0, x1, y1, x2, y2, x2, y2};
    geom::quads(list, q.data(), 4, colour);
}

// A Polyline per run of finite values, one vertex per column centre.
void emitColumnRuns(DrawList& list, std::span<const double> values, float x0, const YMap& ymap, Rgba colour,
                    float lineW) {
    std::vector<float> run;
    const auto flush = [&] {
        if (run.size() >= 4) {
            list.polyline(run.data(), run.size() / 2, colour, lineW);
        } else if (run.size() == 2) {
            list.points(run.data(), 1, colour, lineW * 2.0F);
        }
        run.clear();
    };
    for (std::size_t c = 0; c < values.size(); ++c) {
        if (!std::isfinite(values[c])) {
            flush();
            continue;
        }
        run.push_back(x0 + static_cast<float>(c) + 0.5F);
        run.push_back(ymap.yOf(values[c]));
    }
    flush();
}

} // namespace

Result<ComponentBuild> TimeDistanceBuilder::buildStatic(const ComponentBuildInputs& inputs,
                                                        const CancelToken& cancel) {
    return buildStatic(inputs, TdLocalState{}, cancel, nullptr);
}

Result<ComponentBuild> TimeDistanceBuilder::buildStatic(const ComponentBuildInputs& inputs,
                                                        const TdLocalState& local, const CancelToken& cancel,
                                                        TdStaticInfo* infoOut) {
    ComponentBuild build;
    build.viewVersion = inputs.view.version;
    DrawList& list = build.list;
    list = newList(inputs);
    TdStaticInfo info;
    const float dpr = dprOf(inputs);
    const project::Scheme& scheme = schemeOf(inputs);
    const ITextMetrics& metrics = metricsOf(inputs);
    const Colours colours(scheme);
    const TimeDistanceBody body = bodyOf(inputs);
    TdLayout lay = layoutOf(inputs);
    const std::vector<FlatTrace> flat = flatTraces(body);
    list.clear(colours.background);
    if (lay.keyColumn.w > 0.0F) {
        list.filledRect(lay.keyColumn, colours.keyBackground);
    }

    // Bands (Static; 04 section 8.11).
    std::vector<float> annotationXs;
    if (lay.bands.heightPx > 0.0F) {
        BandLayoutInput bandInput = lay.bandFlags;
        fillBands(inputs, body, lay, flat, colours, bandInput, info, annotationXs);
        BandColours bc;
        bc.text = colours.foreground;
        bc.background = colours.keyBackground;
        bc.frame = colours.frame;
        bc.sectionCorner = colours.sectionCorner;
        bc.sectionStraight = colours.sectionStraight;
        bc.marker = colours.foreground2;
        bc.annotation = colours.annotation;
        bc.deviceEvent = colours.deviceEvent;
        BandLayout::emit(list, bandInput, lay.bands, metrics, bc, dpr, &build.hits);
    }

    // Decimation jobs: every visible trace in every visible slot with data (and distance in distance mode),
    // then the status channels in the Main slot.
    const TraceStyle style = body.traceStyle == "points" ? TraceStyle::Points : TraceStyle::Line;
    std::vector<TraceJob> jobs;
    std::vector<DrawnTrace> drawn;
    std::vector<const ResolvedChannel*> axisChannel(body.groups.size(), nullptr);
    const std::vector<std::string> names = body.channelNames();
    for (const TdGroupLayout& gl : lay.groups) {
        for (std::size_t r = 0; r < gl.traces.size(); ++r) {
            const int flatIndex = gl.traces[r];
            const std::string& channel = names[static_cast<std::size_t>(flatIndex)];
            for (std::size_t k = 0; k < inputs.slots.size(); ++k) {
                const SlotData& slot = inputs.slots[k];
                const ResolvedChannel* rc = channelOf(slot, flatIndex, channel);
                if (rc == nullptr || !rc->descriptor || !rc->series ||
                    rc->status == model::ChannelStatus::Pending) {
                    continue;
                }
                if (axisChannel[static_cast<std::size_t>(gl.group)] == nullptr) {
                    axisChannel[static_cast<std::size_t>(gl.group)] = rc;
                }
                if (inputs.view.domain == Domain::Distance && !slotHasDistance(slot)) {
                    continue;
                }
                TraceSpec spec;
                spec.column = store::seriesValues(*rc->series);
                if (const auto* u = std::get_if<store::UniformSeries>(&*rc->series)) {
                    spec.rate = u->rate;
                    spec.t0 = u->t0;
                } else if (const auto* ts = std::get_if<store::TimestampedSeries>(&*rc->series)) {
                    spec.timesUs = ts->timesUs;
                }
                spec.kind = rc->descriptor->kind;
                spec.interp = rc->descriptor->interp;
                spec.toDisplay = axisChannel[static_cast<std::size_t>(gl.group)]->toDisplay;
                spec.style = style;
                spec.colour = traceColour(scheme, inputs.selection, slot.slot, colourIndexOf(rc, flatIndex));
                spec.widthLogicalPx = style == TraceStyle::Points && body.pointSize != 2.0F
                                          ? body.pointSize
                                          : body.lineWidth * traceWidthScale(scheme, slot.slot);
                spec.dash = traceDash(scheme, slot.slot);
                jobs.push_back(TraceJob{spec, mapperOf(inputs, slot, &*rc->series)});
                drawn.push_back(DrawnTrace{jobs.size() - 1, gl.group, flatIndex, k,
                                           channel == inputs.spec.activeChannel});
            }
        }
    }
    std::vector<std::size_t> statusJobs(lay.statusChannels.size(), std::numeric_limits<std::size_t>::max());
    if (lay.statusStrip.h > 0.0F && !inputs.slots.empty() && slotDrawsX(inputs, inputs.slots.front())) {
        for (std::size_t i = 0; i < lay.statusChannels.size(); ++i) {
            const TdStatusChannel& sc = lay.statusChannels[i];
            if (!sc.series || !sc.descriptor) {
                continue;
            }
            TraceSpec spec;
            spec.column = store::seriesValues(*sc.series);
            if (const auto* u = std::get_if<store::UniformSeries>(&*sc.series)) {
                spec.rate = u->rate;
                spec.t0 = u->t0;
            } else if (const auto* ts = std::get_if<store::TimestampedSeries>(&*sc.series)) {
                spec.timesUs = ts->timesUs;
            }
            spec.kind = sc.descriptor->kind;
            spec.interp = store::Interp::Previous;
            jobs.push_back(TraceJob{spec, mapperOf(inputs, inputs.slots.front(), &*sc.series)});
            statusJobs[i] = jobs.size() - 1;
        }
    }
    TaskPool* pool = inputs.pool != nullptr ? inputs.pool : TaskPool::currentWorkerPool();
    std::vector<Result<DecimatedTrace>> traces =
        TraceBuilder::decimateMany(jobs, inputs.view.zoom, lay.traceWidthPx, cancel, pool);
    for (const Result<DecimatedTrace>& t : traces) {
        if (!t && t.error().code == ErrorCode::Cancelled) {
            return Failure{t.error()};
        }
    }
    if (cancel.checkpoint()) {
        return fail(ErrorCode::Cancelled, "time_distance build cancelled");
    }

    // Scales per group (04 section 8.3), then the widget's vertical zoom.
    std::vector<Scale> scales(body.groups.size());
    for (const TdGroupLayout& gl : lay.groups) {
        Scale& s = scales[static_cast<std::size_t>(gl.group)];
        bool fallback = false;
        s = scaleWithoutData(inputs, body, gl, axisChannel[static_cast<std::size_t>(gl.group)], fallback);
        if (fallback) {
            s = Scale{};
            for (const DrawnTrace& d : drawn) {
                if (d.group != gl.group || !traces[d.job]) {
                    continue;
                }
                const DecimatedTrace& t = *traces[d.job];
                for (const M4Column& col : t.columns) {
                    if ((col.flags & M4Flag::Empty) == 0) {
                        unionScale(s, t.origin + static_cast<double>(col.lo),
                                   t.origin + static_cast<double>(col.hi));
                    }
                }
            }
        }
        if (s.valid() && s.vMax == s.vMin) {
            s.vMin -= 1.0;
            s.vMax += 1.0;
        }
        if (s.valid()) {
            if (const auto it = local.vZoom.find(gl.group); it != local.vZoom.end()) {
                std::tie(s.vMin, s.vMax) = zoomedScale(s.vMin, s.vMax, it->second);
            }
        }
    }
    info.scales.resize(body.groups.size());
    for (std::size_t g = 0; g < scales.size(); ++g) {
        if (scales[g].valid()) {
            info.scales[g] = std::make_pair(scales[g].vMin, scales[g].vMax);
        }
    }

    // Trace emission (vertex generation, 04 section 4.7) per drawn trace into its own sub-list, in parallel
    // like the decimation (the per-trace emit is the largest serial part of a 20-trace build); the panels
    // below append the sub-lists in the draw order, so the list is identical to an in-place emission.
    std::vector<DrawList> parts(drawn.size());
    {
        std::vector<const TdGroupLayout*> layoutOfGroup(body.groups.size(), nullptr);
        for (const TdGroupLayout& gl : lay.groups) {
            layoutOfGroup[static_cast<std::size_t>(gl.group)] = &gl;
        }
        const auto emitOne = [&](std::size_t i) {
            const DrawnTrace& d = drawn[i];
            const TdGroupLayout* gl = layoutOfGroup[static_cast<std::size_t>(d.group)];
            if (gl == nullptr || !traces[d.job] || gl->panel.h <= 0.0F || gl->panel.w <= 0.0F) {
                return;
            }
            const DecimatedTrace& t = *traces[d.job];
            TraceBuilder::emit(parts[i], t.columns, t.origin, jobs[d.job].spec,
                               yMapOf(gl->panel, scales[static_cast<std::size_t>(d.group)]), gl->panel.x,
                               dpr);
        };
        if (pool == nullptr || drawn.size() < 2) {
            for (std::size_t i = 0; i < drawn.size(); ++i) {
                emitOne(i);
            }
        } else {
            std::vector<Future<int>> futures;
            futures.reserve(drawn.size() - 1);
            for (std::size_t i = 1; i < drawn.size(); ++i) {
                futures.push_back(
                    pool->submit(Priority::Visible, TaskKey{}, cancel, [&emitOne, i](const CancelToken&) {
                        emitOne(i);
                        return 0;
                    }));
            }
            emitOne(0);
            for (Future<int>& f : futures) {
                static_cast<void>(std::move(f).take()); // joined before `parts` is read (Rule W)
            }
        }
    }
    if (cancel.checkpoint()) {
        return fail(ErrorCode::Cancelled, "time_distance build cancelled");
    }
    std::size_t partVerts = 0;
    std::size_t partCmds = 0;
    for (const DrawList& p : parts) {
        partVerts += p.verts.size();
        partCmds += p.cmds.size();
    }
    list.verts.reserve(list.verts.size() + partVerts + 4096); // the traces plus grid, axes and frame
    list.cmds.reserve(list.cmds.size() + partCmds + 256);

    // x axis ticks (shared by every panel's vertical grid).
    const AxisSpec xSpec = xAxisSpec(inputs, lay);
    const std::vector<AxisTick> xTicks = AxisLayout::ticks(xSpec, metrics, dpr);
    const float lineW = std::max(1.0F, std::round(dpr));
    const float axisLineH = metrics.lineHeightPx(TextStyleId::Axis, dpr);

    // Overlapped mode: the whole area is a panel of the active group (cursor clicks between regions) and only
    // the active group draws its grid and y axis.
    if (lay.overlapped && lay.traceArea.w > 0.0F && lay.traceArea.h > 0.0F) {
        build.hits.addRect(lay.traceArea, hitOf(HitKind::Panel, lay.activeGroup));
    }
    for (std::size_t gi = 0; gi < lay.groups.size(); ++gi) {
        if (cancel.checkpoint()) {
            return fail(ErrorCode::Cancelled, "time_distance build cancelled");
        }
        const TdGroupLayout& gl = lay.groups[gi];
        const TdGroup& group = body.groups[static_cast<std::size_t>(gl.group)];
        const Scale& s = scales[static_cast<std::size_t>(gl.group)];
        const RectPx& panel = gl.panel;
        if (panel.h <= 0.0F || panel.w <= 0.0F) {
            continue;
        }
        const YMap ymap = yMapOf(panel, s);
        const bool decorated = !lay.overlapped || gl.group == lay.activeGroup;

        std::vector<AxisTick> yGrid;  // every tick: grid lines
        std::vector<AxisTick> yTicks; // the labelled ones
        if (s.valid()) {
            AxisSpec ySpec;
            ySpec.kind = AxisKind::Value;
            ySpec.lo = s.vMax;
            ySpec.hi = s.vMin;
            ySpec.lengthPx = panel.h;
            ySpec.vertical = true;
            // Labels stay inside their panel (the first may use the top margin) so neighbouring panels and
            // the x-axis labels never collide.
            const float bandsBottom = lay.caption.y + lay.caption.h + lay.bands.heightPx;
            const float topSlack =
                gi == 0 ? lay.traceArea.y - bandsBottom : (lay.overlapped ? panel.y - bandsBottom : 0.0F);
            yGrid = AxisLayout::ticks(ySpec, metrics, dpr);
            yTicks = filterVertical(yGrid, axisLineH, -topSlack, panel.h);
        }
        if (body.showGrid && decorated) {
            const RectPx& gridArea = lay.overlapped ? lay.traceArea : panel;
            std::vector<float> grid;
            for (const AxisTick& t : xTicks) {
                const float x = std::round(gridArea.x + t.posPx) + 0.5F;
                grid.insert(grid.end(), {x, gridArea.y, x, gridArea.y + gridArea.h});
            }
            for (const AxisTick& t : yGrid) {
                const float y = std::round(panel.y + t.posPx) + 0.5F;
                grid.insert(grid.end(), {panel.x, y, panel.x + panel.w, y});
            }
            if (!grid.empty()) {
                list.lines(grid.data(), grid.size() / 2, colours.grid, lineW);
            }
        }
        if (s.valid() && group.zeroLine && s.vMin < 0.0 && s.vMax > 0.0) {
            const float y = ymap.yOf(0.0);
            const std::array<float, 4> zero{panel.x, y, panel.x + panel.w, y};
            list.lines(zero.data(), 2, group.zeroColour.value_or(colours.foreground2), lineW);
        }
        // Scale lines min / max at the override limits of the group's first channel that has them.
        if (s.valid() && (group.minLine.enabled || group.maxLine.enabled) && !inputs.slots.empty()) {
            const ResolvedChannel* axis = axisChannel[static_cast<std::size_t>(gl.group)];
            for (const int flatIndex : gl.traces) {
                const ResolvedChannel* rc =
                    channelOf(inputs.slots.front(), flatIndex, names[static_cast<std::size_t>(flatIndex)]);
                if (axis == nullptr || rc == nullptr || (!rc->limitMin && !rc->limitMax)) {
                    continue;
                }
                const auto line = [&](const std::optional<double>& base, const TdScaleLine& sl) {
                    if (!sl.enabled || !base) {
                        return;
                    }
                    const double v = axis->toDisplay(*base);
                    if (!(v >= s.vMin && v <= s.vMax)) {
                        return;
                    }
                    const float y = std::round(ymap.yOf(v)) + 0.5F;
                    const std::array<float, 4> seg{panel.x, y, panel.x + panel.w, y};
                    list.lines(seg.data(), 2, sl.colour.value_or(colours.foreground2), lineW, DashId::Dash);
                };
                line(rc->limitMin, group.minLine);
                line(rc->limitMax, group.maxLine);
                break;
            }
        }
        // Traces: overlays n..1, Overlay 0, Main last; within a slot the active channel last.
        list.scissor(panel);
        for (std::size_t k = inputs.slots.size(); k-- > 0;) {
            for (int pass = 0; pass < 2; ++pass) {
                for (std::size_t i = 0; i < drawn.size(); ++i) {
                    const DrawnTrace& d = drawn[i];
                    if (d.group != gl.group || d.slot != k || d.active != (pass == 1) || !traces[d.job]) {
                        continue;
                    }
                    appendVertexCommands(list, parts[i]);
                }
            }
        }
        list.scissor(RectPx{});
        if (!lay.overlapped && gi + 1 < lay.groups.size()) {
            const float y = panel.y + panel.h - 0.5F * lineW;
            const std::array<float, 4> sep{lay.traceArea.x - (body.showYAxis ? gl.axisStrip.w : 0.0F), y,
                                           lay.traceArea.x + lay.traceArea.w, y};
            list.lines(sep.data(), 2, colours.frame, lineW);
        }
        if (body.showYAxis && !yTicks.empty() && decorated) {
            AxisSpec ySpec;
            ySpec.kind = AxisKind::Value;
            ySpec.vertical = true;
            AxisLayout::emit(list, ySpec, yTicks, gl.axisStrip, colours.foreground, dpr);
        }
        build.hits.addRect(panel, hitOf(HitKind::Panel, gl.group));
        if (body.showYAxis) {
            build.hits.addRect(gl.axisStrip, hitOf(HitKind::Axis, gl.group));
        }
    }
    // Annotation lines (dotted, the whole trace area; 04 section 8.11 item 4).
    if (!annotationXs.empty() && lay.traceArea.h > 0.0F) {
        std::vector<float> segs;
        for (const float x : annotationXs) {
            segs.insert(segs.end(), {x, lay.traceArea.y, x, lay.traceArea.y + lay.traceArea.h});
        }
        list.lines(segs.data(), segs.size() / 2, colours.annotation, lineW, DashId::Dot);
    }
    if (lay.traceArea.w > 0.0F && lay.traceArea.h > 0.0F) {
        list.rectOutline(lay.traceArea, colours.frame, lineW);
    }

    // Status strip (04 section 8.10 item 1): one row per status channel, filled where the value is non-zero.
    if (lay.statusStrip.h > 0.0F) {
        const RectPx& strip = lay.statusStrip;
        list.filledRect(strip, colours.background);
        for (std::size_t i = 0; i < lay.statusChannels.size(); ++i) {
            const TdStatusChannel& sc = lay.statusChannels[i];
            const float y = strip.y + lay.statusRowHeight * static_cast<float>(i);
            const float rowH = std::min(lay.statusRowHeight, strip.y + strip.h - y);
            if (rowH <= 0.0F) {
                break;
            }
            const RectPx row{strip.x, y, strip.w, rowH};
            info.statusNames.push_back(sc.name);
            const std::size_t job = statusJobs[i];
            if (job < traces.size() && traces[job]) {
                const DecimatedTrace& t = *traces[job];
                const float inset = std::max(1.0F, std::round(dpr));
                std::size_t c = 0;
                while (c < t.columns.size()) {
                    const auto on = [&](std::size_t k) {
                        const M4Column& col = t.columns[k];
                        return (col.flags & M4Flag::Empty) == 0 &&
                               (t.origin + static_cast<double>(col.lo) != 0.0 ||
                                t.origin + static_cast<double>(col.hi) != 0.0);
                    };
                    if (!on(c)) {
                        ++c;
                        continue;
                    }
                    std::size_t e = c;
                    while (e < t.columns.size() && on(e)) {
                        ++e;
                    }
                    list.filledRect(RectPx{strip.x + static_cast<float>(c), y + inset,
                                           static_cast<float>(e - c), std::max(1.0F, rowH - 2.0F * inset)},
                                    sc.colour);
                    c = e;
                }
            }
            // The channel name left of the strip.
            const float nameRight = strip.x - 2.0F * dpr;
            if (nameRight > 4.0F * dpr) {
                list.textRun(sc.name, nameRight, y + 0.5F * rowH, TextStyleId::Band, TextAnchor::MiddleRight,
                             sc.descriptor ? colours.foreground2 : colours.frame, nameRight - 2.0F * dpr);
            }
            HitResult h = hitOf(HitKind::StatusRow, -1, static_cast<int>(i));
            build.hits.addRect(row, h);
        }
        const std::array<float, 4> top{strip.x, strip.y + 0.5F * lineW, strip.x + strip.w,
                                       strip.y + 0.5F * lineW};
        list.lines(top.data(), 2, colours.frame, lineW);
    }

    // Variance panel (04 section 8.8, D-023): one VarianceTable per visible overlay, none without one.
    if (lay.variance.h > 0.0F && !inputs.slots.empty() && lay.traceWidthPx > 0) {
        const RectPx& panel = lay.variance;
        list.filledRect(panel, colours.background);
        const SlotData& main = inputs.slots.front();
        const std::shared_ptr<const analysis::DistanceIndex> mainIdx = lapIndexOf(main);
        const bool distance = inputs.view.domain == Domain::Distance;
        const double toX = distance ? 1.0 : 1e-6; // domain units -> metres or seconds
        struct Drawn {
            std::size_t slot = 0;
            analysis::VarianceTable table;
        };
        std::vector<Drawn> tables;
        for (std::size_t k = 1; k < inputs.slots.size(); ++k) {
            const SlotData& s = inputs.slots[k];
            if (spansLaps(main) || spansLaps(s)) {
                info.varianceSpansLaps = true;
                continue;
            }
            const std::shared_ptr<const analysis::DistanceIndex> idx = lapIndexOf(s);
            if (mainIdx == nullptr || idx == nullptr) {
                info.varianceNoData = true;
                continue;
            }
            analysis::VarianceRequest request;
            request.mode =
                distance ? analysis::VarianceMode::TimeAtDistance : analysis::VarianceMode::DistanceAtTime;
            request.x0 = inputs.view.zoom.a * toX;
            request.x1 = inputs.view.zoom.b * toX;
            request.columns = lay.traceWidthPx;
            request.mainOffset =
                distance ? main.range.offsetM : static_cast<double>(main.range.offsetUs) / 1e6;
            request.overlayOffset = distance ? s.range.offsetM : static_cast<double>(s.range.offsetUs) / 1e6;
            ++info.varianceTables;
            Result<analysis::VarianceTable> table = analysis::VarianceTable::build(*mainIdx, *idx, request);
            if (!table) {
                info.varianceNoData = true;
                continue;
            }
            tables.push_back(Drawn{k, std::move(*table)});
        }
        Scale vs;
        if (body.variance.autoScale) {
            for (const Drawn& d : tables) {
                if (const auto r = d.table.valueRange()) {
                    unionScale(vs, r->first, r->second);
                }
            }
            if (vs.valid()) {
                unionScale(vs, 0.0, 0.0); // keep the zero line in view
                const double headroom =
                    0.1 * (vs.vMax - vs.vMin); // the ends' labels fit, the trace clears them
                vs.vMin -= headroom;
                vs.vMax += headroom;
            }
        } else {
            unionScale(vs, std::min(body.variance.min.v, body.variance.max.v),
                       std::max(body.variance.min.v, body.variance.max.v));
        }
        if (!vs.valid()) {
            vs.vMin = -1.0;
            vs.vMax = 1.0;
        }
        if (vs.vMax == vs.vMin) {
            vs.vMin -= 1.0;
            vs.vMax += 1.0;
        }
        const float pad = std::round(2.0F * dpr);
        const RectPx plot{panel.x, panel.y + pad, panel.w, std::max(1.0F, panel.h - 2.0F * pad)};
        const YMap ymap = yMapOf(plot, vs);
        if (vs.vMin < 0.0 && vs.vMax > 0.0) {
            const float y = std::round(ymap.yOf(0.0)) + 0.5F;
            const std::array<float, 4> zero{panel.x, y, panel.x + panel.w, y};
            list.lines(zero.data(), 2, colours.varianceZero, lineW);
        }
        list.scissor(panel);
        for (std::size_t i = tables.size(); i-- > 0;) {
            emitColumnRuns(list, tables[i].table.values(), panel.x, ymap,
                           keyColourOf(inputs, flat, tables[i].slot), lineW * body.lineWidth);
        }
        list.scissor(RectPx{});
        const std::array<float, 4> top{panel.x, panel.y + 0.5F * lineW, panel.x + panel.w,
                                       panel.y + 0.5F * lineW};
        list.lines(top.data(), 2, colours.frame, lineW);
        if (lay.varianceAxis.w > 0.0F) {
            AxisSpec ySpec;
            ySpec.kind = AxisKind::Value;
            ySpec.lo = vs.vMax;
            ySpec.hi = vs.vMin;
            ySpec.lengthPx = plot.h;
            ySpec.vertical = true;
            const std::vector<AxisTick> ticks =
                filterVertical(AxisLayout::ticks(ySpec, metrics, dpr), axisLineH, -pad, plot.h + pad);
            AxisSpec emitSpec;
            emitSpec.kind = AxisKind::Value;
            emitSpec.vertical = true;
            AxisLayout::emit(list, emitSpec, ticks,
                             RectPx{lay.varianceAxis.x, plot.y, lay.varianceAxis.w, plot.h},
                             colours.foreground, dpr);
        }
        list.textRun(distance ? "\xCE\x94t [s]"
                              : "\xCE\x94"
                                "d [m]",
                     panel.x + 3.0F * dpr, panel.y + pad, TextStyleId::Band, TextAnchor::TopLeft,
                     colours.foreground2);
        build.hits.addRect(panel, hitOf(HitKind::Variance, -1));
    }

    if (lay.xAxis.h > 0.0F && !xTicks.empty()) {
        const float leftSlack =
            body.showYAxis ? lay.traceArea.x - (body.keyPos == "right" ? 0.0F : lay.keyColumn.w) : 0.0F;
        AxisSpec spec = xSpec;
        const float rightLimit = lay.bounds.w - lay.traceArea.x;
        std::vector<AxisTick> kept = filterHorizontal(xTicks, metrics, dpr, -leftSlack, rightLimit);
        if (!kept.empty() && !spec.unit.empty()) {
            const AxisTick& last = kept.back();
            const float lastRight = last.posPx + 0.5F * metrics.advancePx(TextStyleId::Axis, last.label, dpr);
            const float unitW = metrics.advancePx(TextStyleId::Axis, spec.unit, dpr);
            if (lastRight + unitW + 2.0F * dpr > lay.xAxis.w) {
                spec.unit.clear();
            }
        }
        AxisLayout::emit(list, spec, kept, lay.xAxis, colours.foreground, dpr);
        build.hits.addRect(lay.xAxis, hitOf(HitKind::Axis, -1));
    }

    // Offset axis (04 sections 3.3 and 8.9): the Main and the reference overlay sub-axes, each with its ticks
    // in its own lap coordinate and the offset written at its right end.
    if (lay.offsetMain.h > 0.0F && !inputs.slots.empty()) {
        const auto subAxis = [&](const RectPx& row, std::size_t k, HandleKind kind) {
            const SlotData& s = inputs.slots[k];
            const Rgba colour = keyColourOf(inputs, flat, k);
            const double off =
                inputs.view.domain == Domain::Time ? static_cast<double>(s.range.offsetUs) : s.range.offsetM;
            list.filledRect(row, colours.keyBackground);
            AxisSpec spec = xSpec;
            spec.lo = inputs.view.zoom.a - off;
            spec.hi = inputs.view.zoom.b - off;
            std::vector<float> marks;
            const float tick = std::round(4.0F * dpr);
            for (const AxisTick& t : AxisLayout::ticks(spec, metrics, dpr)) {
                const float x = std::round(row.x + t.posPx) + 0.5F;
                marks.insert(marks.end(), {x, row.y, x, row.y + (t.major ? tick : 0.5F * tick)});
            }
            const std::array<float, 4> base{row.x, row.y + 0.5F * lineW, row.x + row.w, row.y + 0.5F * lineW};
            list.lines(base.data(), 2, colour, lineW);
            if (!marks.empty()) {
                list.lines(marks.data(), marks.size() / 2, colour, lineW);
            }
            list.textRun(offsetText(inputs.view.domain, s.range), row.x + row.w - 3.0F * dpr,
                         row.y + 0.5F * row.h, TextStyleId::Band, TextAnchor::MiddleRight, colour, 0.0F, 0,
                         colours.keyBackground);
            build.hits.addRect(row, handleOf(HitKind::OffsetAxis, kind, -1, static_cast<int>(k)));
        };
        subAxis(lay.offsetMain, 0, HandleKind::OffsetMain);
        if (lay.referenceSlot > 0) {
            subAxis(lay.offsetReference, static_cast<std::size_t>(lay.referenceSlot),
                    HandleKind::OffsetReference);
        } else {
            list.filledRect(lay.offsetReference, colours.keyBackground);
        }
    }

    // Horizontal scrollbar (04 section 8.6): the thumb is the zoom over the Main outing extent.
    if (lay.scrollbar.h > 0.0F && !inputs.slots.empty()) {
        const SlotData& main = inputs.slots.front();
        const DomainMapper mainMap = mapperOf(inputs, main, nullptr);
        XSpan extent = slotDrawsX(inputs, main) ? mainMap.outingExtent() : XSpan{};
        info.scrollExtent = extent;
        const RectPx& bar = lay.scrollbar;
        list.filledRect(bar, colours.keyBackground);
        build.hits.addRect(bar, handleOf(HitKind::ScrollBar, HandleKind::None));
        if (extent.length() > 0.0) {
            const double a = std::clamp(inputs.view.zoom.a, extent.a, extent.b);
            const double b = std::clamp(inputs.view.zoom.b, extent.a, extent.b);
            const auto at = [&](double x) {
                return bar.x + static_cast<float>((x - extent.a) / extent.length()) * bar.w;
            };
            const float minThumb = 8.0F * dpr;
            float x0 = at(a);
            float x1 = std::max(at(b), x0 + minThumb);
            if (x1 > bar.x + bar.w) {
                x0 = std::max(bar.x, bar.x + bar.w - (x1 - x0));
                x1 = bar.x + bar.w;
            }
            const float inset = std::round(2.0F * dpr);
            const RectPx thumb{x0, bar.y + inset, x1 - x0, std::max(1.0F, bar.h - 2.0F * inset)};
            list.filledRect(thumb, colours.frame);
            build.hits.addRect(thumb, handleOf(HitKind::ScrollBar, HandleKind::ScrollThumb));
        }
    }

    // The active group's vertical scrollbar, the Lo/Hi handles and the panel splitters.
    for (const TdGroupLayout& gl : lay.groups) {
        if (gl.vScroll.w > 0.0F && gl.vScroll.h > 0.0F) {
            list.filledRect(gl.vScroll, colours.keyBackground);
            TdGroupZoom z;
            if (const auto it = local.vZoom.find(gl.group); it != local.vZoom.end()) {
                z = it->second;
            }
            const double lo = std::clamp(std::min(z.lo, z.hi), 0.0, 1.0);
            const double hi = std::clamp(std::max(z.lo, z.hi), 0.0, 1.0);
            const float top = gl.vScroll.y + static_cast<float>(1.0 - hi) * gl.vScroll.h;
            const float bottom = gl.vScroll.y + static_cast<float>(1.0 - lo) * gl.vScroll.h;
            const RectPx thumb{gl.vScroll.x, top, gl.vScroll.w, std::max(2.0F * dpr, bottom - top)};
            list.filledRect(thumb, colours.frame);
            build.hits.addRect(gl.vScroll, handleOf(HitKind::ScrollBar, HandleKind::None, gl.group));
            build.hits.addRect(thumb, handleOf(HitKind::ScrollBar, HandleKind::ScrollThumb, gl.group));
        }
        if (gl.loHandle.w > 0.0F) {
            const Rgba colour = gl.rows.empty() ? colours.frame : gl.rows.front().colour;
            const RectPx& lo = gl.loHandle;
            const RectPx& hi = gl.hiHandle;
            triangle(list, lo.x, lo.y, lo.x + lo.w, lo.y, lo.x + 0.5F * lo.w, lo.y + lo.h, colour);
            triangle(list, hi.x + 0.5F * hi.w, hi.y, hi.x + hi.w, hi.y + hi.h, hi.x, hi.y + hi.h, colour);
            build.hits.addRect(lo, handleOf(HitKind::Handle, HandleKind::GroupLo, gl.group));
            build.hits.addRect(hi, handleOf(HitKind::Handle, HandleKind::GroupHi, gl.group));
        }
    }
    // The panel splitters: grabbed left of the trace area (the axis strip, and the key column when it is on
    // the left), so a click in the trace area always places the cursor.
    const float splitterX = 0.0F;
    if (!lay.overlapped && lay.groups.size() > 1 && lay.traceArea.x > splitterX) {
        const float half = snap(kSplitterHalfLogical * dpr);
        for (std::size_t gi = 0; gi + 1 < lay.groups.size(); ++gi) {
            const TdGroupLayout& upper = lay.groups[gi];
            const float y = upper.panel.y + upper.panel.h;
            if (upper.panel.h <= 0.0F) {
                continue;
            }
            HitResult hr = handleOf(HitKind::Handle, HandleKind::PanelSplitter, upper.group);
            hr.handle = lay.groups[gi + 1].group;
            build.hits.addRect(RectPx{splitterX, y - half, lay.traceArea.x - splitterX, 2.0F * half}, hr);
        }
    }

    // Measurements (04 section 8.7): min / max / avg per trace and slot over the visible span.
    if (lay.keyColumns.measurements) {
        for (TdGroupLayout& gl : lay.groups) {
            for (std::size_t r = 0; r < gl.rows.size(); ++r) {
                KeyRow& row = gl.rows[r];
                const int flatIndex = gl.traces[r];
                const TdTrace* trace = flat[static_cast<std::size_t>(flatIndex)].trace;
                for (std::size_t k = 0; k < row.measurements.size() && k < inputs.slots.size(); ++k) {
                    KeyMeasurement& m = row.measurements[k];
                    m.min.text.clear();
                    m.max.text.clear();
                    m.avg.text.clear();
                    TdMeasurementEntry entry;
                    entry.group = gl.group;
                    entry.trace = flatIndex;
                    entry.slot = static_cast<int>(k);
                    const SlotData& slot = inputs.slots[k];
                    const ResolvedChannel* rc = channelOf(slot, flatIndex, trace->channel);
                    if (rc != nullptr && rc->descriptor && rc->series &&
                        rc->status != model::ChannelStatus::Pending && slotDrawsX(inputs, slot)) {
                        const DomainMapper map = mapperOf(inputs, slot, &*rc->series);
                        Result<Measured> meas = measureSpan(*rc->series, map.timeAt(inputs.view.zoom.a),
                                                            map.timeAt(inputs.view.zoom.b), cancel);
                        if (!meas) {
                            if (meas.error().code == ErrorCode::Cancelled) {
                                return Failure{meas.error()};
                            }
                        } else if (meas->count > 0) {
                            const int dps = dpsOf(*trace, rc);
                            double lo = rc->toDisplay(meas->min);
                            double hi = rc->toDisplay(meas->max);
                            Micros tLo = meas->tMin;
                            Micros tHi = meas->tMax;
                            if (lo > hi) { // a negative display scale swaps the extremes
                                std::swap(lo, hi);
                                std::swap(tLo, tHi);
                            }
                            m.min.text = detail::formatFixed(lo, dps);
                            m.max.text = detail::formatFixed(hi, dps);
                            m.avg.text = detail::formatFixed(
                                rc->toDisplay(meas->sum / static_cast<double>(meas->count)), dps);
                            entry.valid = true;
                            entry.minT = tLo;
                            entry.maxT = tHi;
                            entry.minX = map.xOf(tLo);
                            entry.maxX = map.xOf(tHi);
                            m.minXPx = static_cast<float>(DomainMapper::pxOf(
                                entry.minX, inputs.view.zoom, static_cast<double>(lay.traceArea.x),
                                static_cast<double>(lay.traceWidthPx)));
                            m.maxXPx = static_cast<float>(DomainMapper::pxOf(
                                entry.maxX, inputs.view.zoom, static_cast<double>(lay.traceArea.x),
                                static_cast<double>(lay.traceWidthPx)));
                            m.positionsKnown = true;
                        }
                    }
                    info.measurements.push_back(entry);
                }
            }
        }
    }

    for (const TdGroupLayout& gl : lay.groups) {
        if (gl.key.w <= 0.0F) {
            continue;
        }
        // The group heading (04 section 8.2 item 4; M4, D-211 (e)): the group's part of the key column; its
        // labels, values and measurement cells, added after it, win where they lie.
        if (gl.key.h > 0.0F) {
            build.hits.addRect(gl.key, hitOf(HitKind::GroupHeading, gl.group));
        }
        KeyLayout::emitLabels(list, gl.rows, gl.keyGeometry, gl.key.x, gl.key.y, colours.foreground2);
        for (std::size_t r = 0; r < gl.rows.size() && r < gl.keyGeometry.rowRects.size(); ++r) {
            RectPx rect = gl.keyGeometry.rowRects[r];
            rect.x += gl.key.x;
            rect.y += gl.key.y;
            build.hits.addRect(rect, hitOf(HitKind::KeyLabel, gl.group, gl.traces[r]));
            for (std::size_t line = 0; line < gl.rows[r].slotValues.size(); ++line) {
                RectPx v = gl.keyGeometry.valueRect(r, line);
                v.x += gl.key.x;
                v.y += gl.key.y;
                build.hits.addRect(v,
                                   hitOf(HitKind::KeyValue, gl.group, gl.traces[r], static_cast<int>(line)));
            }
        }
        if (lay.keyColumns.measurements) {
            KeyLayout::emitMeasurements(list, gl.rows, gl.keyGeometry, gl.key.x, gl.key.y,
                                        colours.foreground2, &build.hits);
        }
    }
    if (infoOut != nullptr) {
        *infoOut = std::move(info);
    }
    return build;
}

// ---- Dynamic
// ------------------------------------------------------------------------------------------------

namespace {

std::string enumLabel(const SlotData& slot, const ResolvedChannel& rc, double value) {
    if (!rc.descriptor || !rc.descriptor->enums || !slot.source) {
        return {};
    }
    const model::DataLayer* layer = slot.source->layer(slot.layer);
    if (layer == nullptr) {
        return {};
    }
    const model::EnumTable* table = layer->enumTable(*rc.descriptor->enums);
    if (table == nullptr) {
        return {};
    }
    if (const model::EnumEntry* e = table->find(static_cast<std::int32_t>(std::llround(value)))) {
        return e->label;
    }
    return {};
}

const char* const kPending = "\xE2\x80\xA6"; // U+2026, "..." while pending (D-022)

// The value of a channel in a slot at the Main-domain x: its text ("..." pending, "" invalid or missing) and
// the display value (NaN unless valid). The callers pass only channels with a descriptor; testing it here
// keeps the access below checked (bugprone-unchecked-optional-access).
std::pair<std::string, double> valueAtX(const ComponentBuildInputs& inputs, const SlotData& slot,
                                        const ResolvedChannel& rc, const TdTrace* trace, double x) {
    if (rc.status == model::ChannelStatus::Pending || !rc.series) {
        return {kPending, std::numeric_limits<double>::quiet_NaN()};
    }
    if (!rc.descriptor || !slotDrawsX(inputs, slot)) {
        return {{}, std::numeric_limits<double>::quiet_NaN()};
    }
    const DomainMapper map = mapperOf(inputs, slot, &*rc.series);
    const Micros t = map.timeAt(x);
    const store::ValueSample v = store::Resampler::sampleAt(*rc.series, t, rc.descriptor->interp);
    if (v.state == store::SampleState::Absent) {
        return {kPending, std::numeric_limits<double>::quiet_NaN()};
    }
    if (v.state != store::SampleState::Valid) {
        return {{}, std::numeric_limits<double>::quiet_NaN()};
    }
    const double display = rc.toDisplay(v.value);
    std::string text = enumLabel(slot, rc, v.value);
    if (text.empty()) {
        text = detail::formatFixed(display, trace != nullptr ? dpsOf(*trace, &rc) : rc.dps);
    }
    return {std::move(text), display};
}

} // namespace

DrawList TimeDistanceBuilder::buildDynamic(const ComponentBuildInputs& inputs) {
    return buildDynamic(inputs, TdLocalState{});
}

DrawList TimeDistanceBuilder::buildDynamic(const ComponentBuildInputs& inputs, const TdLocalState& local) {
    DrawList list = newList(inputs);
    if (inputs.slots.empty()) {
        return list;
    }
    const float dpr = dprOf(inputs);
    const project::Scheme& scheme = schemeOf(inputs);
    const Colours colours(scheme);
    const TimeDistanceBody body = bodyOf(inputs);
    TdLayout lay = layoutOf(inputs);
    const SlotData& mainSlot = inputs.slots.front();
    const DomainMapper mainMap = mapperOf(inputs, mainSlot, nullptr);
    const bool haveX = inputs.view.domain == Domain::Time || mainMap.hasDistance();
    const auto width = static_cast<double>(lay.traceWidthPx);
    const auto traceX = static_cast<double>(lay.traceArea.x);
    const float lineW = std::max(1.0F, std::round(dpr));
    const std::vector<FlatTrace> flat = flatTraces(body);

    const auto cursorPx = [&](Micros rel) -> std::optional<float> {
        const double x = mainMap.xOf(mainSlot.range.start + rel);
        const double px = DomainMapper::pxOf(x, inputs.view.zoom, traceX, width);
        if (!(px >= traceX) || !(px <= traceX + width)) {
            return std::nullopt;
        }
        return std::round(static_cast<float>(px)) + 0.5F;
    };
    const auto cursorLine = [&](Micros rel, Rgba colour, DashId dash) {
        const std::optional<float> sx = cursorPx(rel);
        if (!sx) {
            return;
        }
        std::vector<float> segs;
        if (lay.overlapped) {
            segs.insert(segs.end(), {*sx, lay.traceArea.y, *sx, lay.traceArea.y + lay.traceArea.h});
        } else {
            for (const TdGroupLayout& gl : lay.groups) {
                segs.insert(segs.end(), {*sx, gl.panel.y, *sx, gl.panel.y + gl.panel.h});
            }
        }
        for (const RectPx& r : {lay.statusStrip, lay.variance}) {
            if (r.h > 0.0F) {
                segs.insert(segs.end(), {*sx, r.y, *sx, r.y + r.h});
            }
        }
        if (!segs.empty()) {
            list.lines(segs.data(), segs.size() / 2, colour, lineW, dash);
        }
    };
    if (haveX) {
        if (inputs.view.datumT) {
            cursorLine(*inputs.view.datumT, colours.datum, DashId::Dash);
        }
        if (body.cursorStyle != "none") {
            cursorLine(inputs.view.cursorT, colours.cursor, DashId::Solid);
        }
    }
    const double xc = haveX ? mainMap.xOf(mainSlot.range.start + inputs.view.cursorT) : 0.0;

    // Crosshair (Y): a horizontal line at the active channel's Main value in its panel.
    if (haveX && body.cursorStyle == "crosshair" && lay.activeGroup >= 0) {
        const int active = activeIndexOf(inputs, flat);
        const TdGroupLayout* gl = nullptr;
        for (const TdGroupLayout& g : lay.groups) {
            if (g.group == lay.activeGroup) {
                gl = &g;
            }
        }
        if (active >= 0 && gl != nullptr && gl->panel.h > 0.0F && cursorPx(inputs.view.cursorT)) {
            const FlatTrace& ft = flat[static_cast<std::size_t>(active)];
            const ResolvedChannel* rc = channelOf(mainSlot, ft.index, ft.trace->channel);
            std::optional<std::pair<double, double>> scale;
            const auto g = static_cast<std::size_t>(gl->group);
            if (g < local.builtScales.size()) {
                scale = local.builtScales[g];
            } else if (rc != nullptr) {
                bool fallback = false;
                const Scale s = scaleWithoutData(inputs, body, *gl, rc, fallback);
                if (s.valid()) {
                    Scale z = s;
                    if (z.vMax == z.vMin) {
                        z.vMin -= 1.0;
                        z.vMax += 1.0;
                    }
                    if (const auto it = local.vZoom.find(gl->group); it != local.vZoom.end()) {
                        std::tie(z.vMin, z.vMax) = zoomedScale(z.vMin, z.vMax, it->second);
                    }
                    scale = std::make_pair(z.vMin, z.vMax);
                }
            }
            if (rc != nullptr && rc->descriptor && scale) {
                const auto [text, v] = valueAtX(inputs, mainSlot, *rc, ft.trace, xc);
                if (std::isfinite(v) && v >= scale->first && v <= scale->second) {
                    Scale s;
                    s.vMin = scale->first;
                    s.vMax = scale->second;
                    const float y = std::round(yMapOf(gl->panel, s).yOf(v)) + 0.5F;
                    const std::array<float, 4> seg{gl->panel.x, y, gl->panel.x + gl->panel.w, y};
                    list.lines(seg.data(), 2, colours.cursor, lineW, DashId::Dot);
                }
            }
        }
    }

    if (!body.keyValues || lay.keyColumn.w <= 0.0F) {
        return list;
    }
    const bool datum = lay.keyColumns.datum && inputs.view.datumT && haveX;
    const double xd = datum ? mainMap.xOf(mainSlot.range.start + *inputs.view.datumT) : 0.0;
    for (TdGroupLayout& gl : lay.groups) {
        for (std::size_t r = 0; r < gl.rows.size(); ++r) {
            const int flatIndex = gl.traces[r];
            const TdTrace* trace = flat[static_cast<std::size_t>(flatIndex)].trace;
            const std::string& channel = trace->channel;
            KeyRow& row = gl.rows[r];
            for (std::size_t k = 0; k < row.slotValues.size(); ++k) {
                std::string text;
                std::string datumText;
                std::string deltaText;
                if (k < inputs.slots.size() && haveX) {
                    const SlotData& slot = inputs.slots[k];
                    const ResolvedChannel* rc = channelOf(slot, flatIndex, channel);
                    if (rc != nullptr && rc->descriptor) {
                        double vMain = std::numeric_limits<double>::quiet_NaN();
                        std::tie(text, vMain) = valueAtX(inputs, slot, *rc, trace, xc);
                        if (datum) {
                            double vDatum = std::numeric_limits<double>::quiet_NaN();
                            std::tie(datumText, vDatum) = valueAtX(inputs, slot, *rc, trace, xd);
                            if (std::isfinite(vMain) && std::isfinite(vDatum)) {
                                deltaText = detail::formatFixed(vMain - vDatum, dpsOf(*trace, rc));
                                if (vMain - vDatum > 0.0 &&
                                    deltaText.find_first_not_of("0.") != std::string::npos) {
                                    deltaText.insert(deltaText.begin(), '+');
                                }
                            } else if (datumText == kPending || text == kPending) {
                                deltaText = kPending;
                            }
                        }
                    } else if (k > 0) {
                        text =
                            "(not in " + slot.slot.range.name + ")"; // 04 section 8.4; Main shows "(missing)"
                    }
                }
                row.slotValues[k].text = std::move(text);
                if (k < row.datumValues.size()) {
                    row.datumValues[k].text = std::move(datumText);
                }
                if (k < row.deltaValues.size()) {
                    row.deltaValues[k].text = std::move(deltaText);
                }
            }
        }
        KeyLayout::emitValues(list, gl.rows, gl.keyGeometry, gl.key.x, gl.key.y, colours.foreground,
                              colours.keyBackground);
        if (datum) {
            KeyLayout::emitDatumValues(list, gl.rows, gl.keyGeometry, gl.key.x, gl.key.y, colours.foreground,
                                       colours.keyBackground);
        }
    }
    return list;
}

// ---- Chrome
// -------------------------------------------------------------------------------------------------

DrawList TimeDistanceBuilder::buildChrome(const ComponentBuildInputs& inputs, bool pending) {
    DrawList list = newList(inputs);
    const float dpr = dprOf(inputs);
    const project::Scheme& scheme = schemeOf(inputs);
    const ITextMetrics& metrics = metricsOf(inputs);
    const Colours colours(scheme);
    const TimeDistanceBody body = bodyOf(inputs);
    const TdLayout lay = layoutOf(inputs);

    if (lay.caption.h > 0.0F) {
        list.filledRect(lay.caption, colours.captionBackground);
        std::string text = inputs.spec.caption.text;
        if (inputs.spec.caption.autoText) {
            text.clear();
            for (const TdGroup& g : body.groups) {
                for (const TdTrace& t : g.traces) {
                    if (!t.visible) {
                        continue;
                    }
                    if (!text.empty()) {
                        text += ", ";
                    }
                    text += t.channel;
                }
            }
        }
        if (!text.empty()) {
            list.textRun(text, lay.caption.x + 4.0F * dpr, lay.caption.y + 0.5F * lay.caption.h,
                         TextStyleId::Caption, TextAnchor::MiddleLeft, colours.captionForeground,
                         std::max(1.0F, lay.caption.w - 8.0F * dpr));
        }
    }
    // Active-channel square.
    bool activeDone = false;
    for (const TdGroupLayout& gl : lay.groups) {
        for (std::size_t r = 0; r < gl.rows.size() && !activeDone && gl.key.w > 0.0F; ++r) {
            if (!gl.rows[r].active) {
                continue;
            }
            RectPx sq = gl.keyGeometry.squareRect(r);
            sq.x += gl.key.x;
            sq.y += gl.key.y;
            list.filledRect(sq, gl.rows[r].colour);
            activeDone = true;
        }
    }
    // Pending stripe: a build in flight or a Pending channel (D-022).
    bool anyPending = pending;
    for (const SlotData& slot : inputs.slots) {
        for (const ResolvedChannel& rc : slot.channels) {
            anyPending = anyPending || (rc.descriptor && rc.status == model::ChannelStatus::Pending);
        }
    }
    if (anyPending && lay.traceArea.w > 0.0F) {
        list.filledRect(RectPx{lay.traceArea.x, lay.traceArea.y, lay.traceArea.w, std::round(3.0F * dpr)},
                        colours.pending);
    }
    // "no distance data" banner, one line per source without distance (AC-M1-09).
    if (inputs.view.domain == Domain::Distance && lay.traceArea.w > 0.0F) {
        std::set<std::string> seen;
        float y = lay.traceArea.y + 4.0F * dpr;
        const float lineH = metrics.lineHeightPx(TextStyleId::Banner, dpr);
        for (const SlotData& slot : inputs.slots) {
            if (slotHasDistance(slot)) {
                continue;
            }
            const std::string name = sourceName(slot);
            if (!seen.insert(name).second) {
                continue;
            }
            const std::string text =
                name.empty() ? std::string("No distance data") : "No distance data for " + name;
            list.textRun(text, lay.traceArea.x + 0.5F * lay.traceArea.w, y, TextStyleId::Banner,
                         TextAnchor::TopCentre, colours.foreground,
                         std::max(1.0F, lay.traceArea.w - 8.0F * dpr), 0, colours.keyBackground);
            y += lineH;
        }
    }
    // The variance panel's banner (04 section 8.8 item 3): a lap of Main or of an overlay has no
    // DistanceIndex, or a range spans laps (D-201).
    if (lay.variance.h > 0.0F && !inputs.slots.empty()) {
        bool missing = lapIndexOf(inputs.slots.front()) == nullptr;
        bool spans = spansLaps(inputs.slots.front());
        for (std::size_t k = 1; k < inputs.slots.size(); ++k) {
            missing = missing || lapIndexOf(inputs.slots[k]) == nullptr;
            spans = spans || spansLaps(inputs.slots[k]);
        }
        if (spans || missing) {
            list.textRun(spans ? "variance compares single laps" : "variance needs distance data",
                         lay.variance.x + 0.5F * lay.variance.w, lay.variance.y + 0.5F * lay.variance.h,
                         TextStyleId::Banner, TextAnchor::Centre, colours.foreground,
                         std::max(1.0F, lay.variance.w - 8.0F * dpr), 0, colours.keyBackground);
        }
    }
    if (inputs.focused && lay.bounds.w > 0.0F) {
        list.rectOutline(lay.bounds, colours.focus, std::round(2.0F * dpr));
    }
    return list;
}

} // namespace esses::plot
