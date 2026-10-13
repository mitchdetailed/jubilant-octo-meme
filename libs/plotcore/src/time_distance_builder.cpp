// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "text_format.hpp"

#include <esses/base/taskpool.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/axis_layout.hpp>
#include <esses/plotcore/selection_snapshot.hpp>
#include <esses/plotcore/time_distance_builder.hpp>
#include <esses/plotcore/trace_builder.hpp>
#include <esses/store/resampler.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace esses::plot {

namespace {

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

json valueWithUnit(double v, const std::string& unit) {
    json o = json::object();
    o["v"] = v;
    if (!unit.empty()) {
        o["unit"] = unit;
    }
    return o;
}

// ---- scheme roles
// -------------------------------------------------------------------------------------------

struct Colours {
    Rgba background, keyBackground, foreground, foreground2, frame, grid, cursor, datum;
    Rgba captionBackground, captionForeground, focus, pending;

    explicit Colours(const project::Scheme& s)
        : background(s.role("cptBackground", 0xFFFFFFFFU)),
          keyBackground(s.role("keyBackground", 0xF4F4F4FFU)),
          foreground(s.role("cptForeground", 0x202020FFU)),
          foreground2(s.role("cptForeground2", 0x707070FFU)), frame(s.role("cptFrame", 0xA0A0A0FFU)),
          grid(s.role("dataGrid", 0xE6E6E6FFU)), cursor(s.role("cursorMain", 0xFF0000FFU)),
          datum(s.role("cursorDatum", 0xC09000FFU)),
          captionBackground(s.role("cptCaptionBackground", 0xE8E8E8FFU)),
          captionForeground(s.role("cptCaptionForeground", 0x202020FFU)),
          focus(s.role("cptFocusBorder", 0x2060FFFFU)), pending(s.role("pendingHatch", 0xC8C8C8FFU)) {}
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

// ---- layout
// -------------------------------------------------------------------------------------------------

constexpr float kCaptionLogical = 18.0F;
constexpr float kXAxisLogical = 18.0F;
constexpr float kAxisStripLogical = 36.0F;
constexpr float kTopMarginLogical = 6.0F;
constexpr float kRightMarginLogical = 4.0F;

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
        if (!readAs(*d, "traceStyle", out.traceStyle, "/display", err) ||
            !readNumber(*d, "lineWidth", lineWidth, "/display", err) ||
            !readAs(*d, "showGrid", out.showGrid, "/display", err)) {
            return invalid();
        }
        out.lineWidth = static_cast<float>(lineWidth);
        if (const auto k = d->find("key"); k != d->end() && k->is_object()) {
            if (!readAs(*k, "mode", out.keyMode, "/display/key", err) ||
                !readAs(*k, "pos", out.keyPos, "/display/key", err) ||
                !readAs(*k, "values", out.keyValues, "/display/key", err)) {
                return invalid();
            }
        }
        if (const auto a = d->find("axis"); a != d->end() && a->is_object()) {
            if (!readAs(*a, "showX", out.showXAxis, "/display/axis", err) ||
                !readAs(*a, "showY", out.showYAxis, "/display/axis", err)) {
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
            if (!readAs(gj, "name", g.name, ptr, err) || !readAs(gj, "scaleMode", mode, ptr, err)) {
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
                if (const auto zero = lines->find("zero"); zero != lines->end() && zero->is_object()) {
                    if (!readAs(*zero, "enabled", g.zeroLine, ptr + "/lines/zero", err)) {
                        return invalid();
                    }
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
        put(display, "showGrid", showGrid, defaults.showGrid);
        json key = display.contains("key") && display["key"].is_object() ? display["key"] : json::object();
        put(key, "mode", keyMode, defaults.keyMode);
        put(key, "pos", keyPos, defaults.keyPos);
        put(key, "values", keyValues, defaults.keyValues);
        if (!key.empty()) {
            display["key"] = key;
        }
        json axis =
            display.contains("axis") && display["axis"].is_object() ? display["axis"] : json::object();
        put(axis, "showX", showXAxis, defaults.showXAxis);
        put(axis, "showY", showYAxis, defaults.showYAxis);
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
            const bool hasZero = gj.contains("lines") && gj["lines"].is_object() &&
                                 gj["lines"].contains("zero") && gj["lines"]["zero"].is_object();
            if (hasZero || g.zeroLine != gd.zeroLine) {
                gj["lines"]["zero"]["enabled"] = g.zeroLine;
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

    // Key rows: one per visible trace, values per visible slot (templates here; Dynamic fills them).
    const bool showKey = body.keyMode != "none";
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
        if (body.keyValues) {
            const std::string tmpl = valueTemplate(dpsOf(*ft.trace, mainRc));
            for (std::size_t k = 0; k < std::max<std::size_t>(1, inputs.slots.size()); ++k) {
                const ResolvedChannel* rc = k < inputs.slots.size()
                                                ? channelOf(inputs.slots[k], ft.index, ft.trace->channel)
                                                : nullptr;
                row.slotValues.push_back(
                    KeyValue{tmpl, traceColour(scheme, inputs.selection, slotOf(inputs, k),
                                               colourIndexOf(rc, ft.index))});
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
        all = KeyLayout::layout(allRows, metrics, w, rowH, dpr);
        keyW = snap(all.widthPx);
    }
    const bool keyRight = body.keyPos == "right";
    const float plotTop = top;
    const float plotBottom = std::max(plotTop, h - xAxisH);
    lay.keyColumn =
        keyW > 0.0F ? RectPx{keyRight ? w - keyW : 0.0F, plotTop, keyW, plotBottom - plotTop} : RectPx{};
    const float traceX = (keyRight ? 0.0F : keyW) + axisW;
    const float traceRight = keyRight ? w - keyW - rightMargin : w - rightMargin;
    const float traceTop = std::min(plotBottom, plotTop + topMargin);
    lay.traceArea = RectPx{traceX, traceTop, std::max(0.0F, traceRight - traceX), plotBottom - traceTop};
    lay.traceWidthPx = static_cast<int>(lay.traceArea.w);
    lay.xAxis = xAxisH > 0.0F ? RectPx{traceX, plotBottom, lay.traceArea.w, xAxisH} : RectPx{};

    // Panels by edges; equal shares when every group keeps the default [0, 1].
    const std::size_t n = body.groups.size();
    const bool allDefault = std::all_of(body.groups.begin(), body.groups.end(), [](const TdGroup& g) {
        return g.edgeTop == 0.0 && g.edgeBottom == 1.0;
    });
    for (std::size_t g = 0; g < n; ++g) {
        double e0 = std::clamp(body.groups[g].edgeTop, 0.0, 1.0);
        double e1 = std::clamp(body.groups[g].edgeBottom, 0.0, 1.0);
        if (allDefault && n > 1) {
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
        const float keyY = g == 0 ? plotTop : y0; // the first group's key starts under the caption
        gl.key = RectPx{lay.keyColumn.x, keyY, keyW, y1 - keyY};
        gl.rows = std::move(groupRows[g]);
        gl.traces = std::move(groupTraces[g]);
        if (showKey) {
            gl.keyGeometry = KeyLayout::layout(gl.rows, metrics, w, rowH, dpr);
            gl.keyGeometry.widthPx = keyW;
            gl.keyGeometry.labelWidthPx = all.labelWidthPx + (keyW - all.widthPx);
            gl.keyGeometry.valueWidthPx = all.valueWidthPx;
            for (RectPx& r : gl.keyGeometry.rowRects) {
                r.w = keyW;
                r.y += snap(2.0F * dpr);
            }
        }
        lay.groups.push_back(std::move(gl));
    }
    return lay;
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

} // namespace

Result<ComponentBuild> TimeDistanceBuilder::buildStatic(const ComponentBuildInputs& inputs,
                                                        const CancelToken& cancel) {
    ComponentBuild build;
    build.viewVersion = inputs.view.version;
    DrawList& list = build.list;
    list = newList(inputs);
    const float dpr = dprOf(inputs);
    const project::Scheme& scheme = schemeOf(inputs);
    const ITextMetrics& metrics = metricsOf(inputs);
    const Colours colours(scheme);
    const TimeDistanceBody body = bodyOf(inputs);
    const TdLayout lay = layoutOf(inputs);
    list.clear(colours.background);
    if (lay.keyColumn.w > 0.0F) {
        list.filledRect(lay.keyColumn, colours.keyBackground);
    }

    // Decimation jobs: every visible trace in every visible slot with data (and distance in distance mode).
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
                spec.widthLogicalPx = body.lineWidth * traceWidthScale(scheme, slot.slot);
                spec.dash = traceDash(scheme, slot.slot);
                jobs.push_back(TraceJob{spec, mapperOf(inputs, slot, &*rc->series)});
                drawn.push_back(DrawnTrace{jobs.size() - 1, gl.group, flatIndex, k,
                                           channel == inputs.spec.activeChannel});
            }
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

    // Scales per group (04 section 8.3).
    std::vector<Scale> scales(body.groups.size());
    for (const TdGroupLayout& gl : lay.groups) {
        const TdGroup& group = body.groups[static_cast<std::size_t>(gl.group)];
        Scale& s = scales[static_cast<std::size_t>(gl.group)];
        const ResolvedChannel* axis = axisChannel[static_cast<std::size_t>(gl.group)];
        if (group.scaleMode == ScaleMode::Manual && group.scaleMin && group.scaleMax && axis != nullptr) {
            const auto lo = manualToAxis(*group.scaleMin, group.scaleUnit, *axis, inputs.units.get());
            const auto hi = manualToAxis(*group.scaleMax, group.scaleUnit, *axis, inputs.units.get());
            if (lo && hi) {
                unionScale(s, std::min(*lo, *hi), std::max(*lo, *hi));
            }
        }
        if (s.valid() || axis == nullptr) {
            continue;
        }
        bool fallback = inputs.slots.empty();
        for (std::size_t r = 0; r < gl.traces.size() && !fallback; ++r) {
            const int flatIndex = gl.traces[r];
            const ResolvedChannel* rc =
                channelOf(inputs.slots.front(), flatIndex, names[static_cast<std::size_t>(flatIndex)]);
            if (rc == nullptr || !rc->descriptor || !rc->series) {
                continue; // a channel missing in the Main source does not scale the group
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
            const double a = axis->toDisplay(static_cast<double>(mm.lo));
            const double b = axis->toDisplay(static_cast<double>(mm.hi));
            unionScale(s, std::min(a, b), std::max(a, b));
        }
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
            const float topSlack = gi == 0 ? lay.traceArea.y - lay.caption.h - lay.caption.y : 0.0F;
            yGrid = AxisLayout::ticks(ySpec, metrics, dpr);
            yTicks = filterVertical(yGrid, axisLineH, -topSlack, panel.h);
        }
        if (body.showGrid) {
            std::vector<float> grid;
            for (const AxisTick& t : xTicks) {
                const float x = std::round(panel.x + t.posPx) + 0.5F;
                grid.insert(grid.end(), {x, panel.y, x, panel.y + panel.h});
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
            list.lines(zero.data(), 2, colours.foreground2, lineW);
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
        if (gi + 1 < lay.groups.size()) {
            const float y = panel.y + panel.h - 0.5F * lineW;
            const std::array<float, 4> sep{lay.traceArea.x - (body.showYAxis ? gl.axisStrip.w : 0.0F), y,
                                           lay.traceArea.x + lay.traceArea.w, y};
            list.lines(sep.data(), 2, colours.frame, lineW);
        }
        if (body.showYAxis && !yTicks.empty()) {
            AxisSpec ySpec;
            ySpec.kind = AxisKind::Value;
            ySpec.vertical = true;
            AxisLayout::emit(list, ySpec, yTicks, gl.axisStrip, colours.foreground, dpr);
        }
        build.hits.addRect(panel, HitResult{HitKind::Panel, gl.group, -1, -1, -1, {}, -1});
        if (body.showYAxis) {
            build.hits.addRect(gl.axisStrip, HitResult{HitKind::Axis, gl.group, -1, -1, -1, {}, -1});
        }
    }
    if (lay.traceArea.w > 0.0F && lay.traceArea.h > 0.0F) {
        list.rectOutline(lay.traceArea, colours.frame, lineW);
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
        build.hits.addRect(lay.xAxis, HitResult{HitKind::Axis, -1, -1, -1, -1, {}, -1});
    }
    for (const TdGroupLayout& gl : lay.groups) {
        if (gl.key.w <= 0.0F) {
            continue;
        }
        KeyLayout::emitLabels(list, gl.rows, gl.keyGeometry, gl.key.x, gl.key.y, colours.foreground2);
        for (std::size_t r = 0; r < gl.rows.size() && r < gl.keyGeometry.rowRects.size(); ++r) {
            RectPx rect = gl.keyGeometry.rowRects[r];
            rect.x += gl.key.x;
            rect.y += gl.key.y;
            build.hits.addRect(rect, HitResult{HitKind::KeyLabel, gl.group, gl.traces[r], -1, -1, {}, -1});
            for (std::size_t line = 0; line < gl.rows[r].slotValues.size(); ++line) {
                RectPx v = gl.keyGeometry.valueRect(r, line);
                v.x += gl.key.x;
                v.y += gl.key.y;
                build.hits.addRect(
                    v,
                    HitResult{HitKind::KeyValue, gl.group, gl.traces[r], static_cast<int>(line), -1, {}, -1});
            }
        }
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

} // namespace

DrawList TimeDistanceBuilder::buildDynamic(const ComponentBuildInputs& inputs) {
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

    const auto cursorLine = [&](Micros rel, Rgba colour, DashId dash) {
        const double x = mainMap.xOf(mainSlot.range.start + rel);
        const double px = DomainMapper::pxOf(x, inputs.view.zoom, traceX, width);
        if (!(px >= traceX) || !(px <= traceX + width)) {
            return;
        }
        const float sx = std::round(static_cast<float>(px)) + 0.5F;
        std::vector<float> segs;
        for (const TdGroupLayout& gl : lay.groups) {
            segs.insert(segs.end(), {sx, gl.panel.y, sx, gl.panel.y + gl.panel.h});
        }
        if (!segs.empty()) {
            list.lines(segs.data(), segs.size() / 2, colour, lineW, dash);
        }
    };
    if (haveX) {
        if (inputs.view.datumT) {
            cursorLine(*inputs.view.datumT, colours.datum, DashId::Dash);
        }
        cursorLine(inputs.view.cursorT, colours.cursor, DashId::Solid);
    }

    if (!body.keyValues || lay.keyColumn.w <= 0.0F) {
        return list;
    }
    const double xc = haveX ? mainMap.xOf(mainSlot.range.start + inputs.view.cursorT) : 0.0;
    const std::vector<FlatTrace> flat = flatTraces(body);
    for (TdGroupLayout& gl : lay.groups) {
        for (std::size_t r = 0; r < gl.rows.size(); ++r) {
            const int flatIndex = gl.traces[r];
            const TdTrace* trace = flat[static_cast<std::size_t>(flatIndex)].trace;
            const std::string& channel = trace->channel;
            KeyRow& row = gl.rows[r];
            for (std::size_t k = 0; k < row.slotValues.size(); ++k) {
                std::string text;
                if (k < inputs.slots.size() && haveX) {
                    const SlotData& slot = inputs.slots[k];
                    const ResolvedChannel* rc = channelOf(slot, flatIndex, channel);
                    if (rc != nullptr && rc->descriptor) {
                        if (rc->status == model::ChannelStatus::Pending || !rc->series) {
                            text = kPending;
                        } else if (inputs.view.domain == Domain::Time || slotHasDistance(slot)) {
                            const DomainMapper map = mapperOf(inputs, slot, &*rc->series);
                            const Micros t = map.timeAt(xc);
                            const store::ValueSample v =
                                store::Resampler::sampleAt(*rc->series, t, rc->descriptor->interp);
                            if (v.state == store::SampleState::Absent) {
                                text = kPending;
                            } else if (v.state == store::SampleState::Valid) {
                                text = enumLabel(slot, *rc, v.value);
                                if (text.empty()) {
                                    text =
                                        detail::formatFixed(rc->toDisplay(v.value),
                                                            trace != nullptr ? dpsOf(*trace, rc) : rc->dps);
                                }
                            }
                        }
                    } else if (k > 0) {
                        text =
                            "(not in " + slot.slot.range.name + ")"; // 04 section 8.4; Main shows "(missing)"
                    }
                }
                row.slotValues[k].text = std::move(text);
            }
        }
        KeyLayout::emitValues(list, gl.rows, gl.keyGeometry, gl.key.x, gl.key.y, colours.foreground,
                              colours.keyBackground);
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
    if (inputs.focused && lay.bounds.w > 0.0F) {
        list.rectOutline(lay.bounds, colours.focus, std::round(2.0F * dpr));
    }
    return list;
}

} // namespace esses::plot
