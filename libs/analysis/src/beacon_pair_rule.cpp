// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "rule_json.hpp"
#include "series_access.hpp"

#include <esses/analysis/beacon_pair_rule.hpp>
#include <esses/base/casefold.hpp>
#include <esses/base/json_nlohmann.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/marker.hpp>

#include <fast_float/fast_float.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>

namespace esses::analysis {

namespace {

using detail::speedUnitToMps;
using detail::stringArray;
using jsonio::asBool;
using jsonio::asInt;
using jsonio::asInt32;
using jsonio::asNumber;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;
using model::LapClass;
using model::Marker;
using model::MarkerClass;

constexpr std::string_view kAnyClass = "*";

std::optional<LapClass> lapClassForKey(std::string_view key) noexcept {
    if (key == "flying") {
        return LapClass::Flying;
    }
    if (key == "out") {
        return LapClass::Out;
    }
    if (key == "in") {
        return LapClass::In;
    }
    if (key == "partial") {
        return LapClass::Partial;
    }
    if (key == "pit") {
        return LapClass::Pit;
    }
    return std::nullopt;
}

std::string expandTemplate(std::string_view tpl, int number) {
    std::string out;
    out.reserve(tpl.size() + 4);
    const std::string n = std::to_string(number);
    for (std::size_t i = 0; i < tpl.size();) {
        if (tpl.compare(i, 3, "{n}") == 0) {
            out += n;
            i += 3;
        } else {
            out += tpl[i];
            ++i;
        }
    }
    return out;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

// The integral value of a numeric detail ("5", "5.0", or a Quantity 5); nullopt otherwise.
std::optional<int> integralDetail(const model::DetailValue& value) {
    double v = 0.0;
    if (const auto* q = std::get_if<model::Quantityd>(&value)) {
        v = q->v;
    } else if (const auto* text = std::get_if<std::string>(&value)) {
        const std::string_view t = trim(*text);
        if (t.empty()) {
            return std::nullopt;
        }
        // fast_float, not std::from_chars: Apple Clang 15's libc++ (and MinGW's libstdc++) has no
        // floating-point from_chars.
        const auto [ptr, ec] = fast_float::from_chars(t.data(), t.data() + t.size(), v);
        if (ec != std::errc{} || ptr != t.data() + t.size()) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (!std::isfinite(v) || v != std::floor(v) || v < -1'000'000.0 || v > 1'000'000.0) {
        return std::nullopt;
    }
    return static_cast<int>(v);
}

BeaconPairClass readClass(const nlohmann::json& j, const std::string& pointer) {
    ObjectReader r(j, pointer);
    BeaconPairClass c;
    c.name = asString(r.required("name"), r.child("name"));
    c.abbrev = asString(r.required("abbrev"), r.child("abbrev"));
    if (const nlohmann::json* p = r.optional("trusted")) {
        c.trusted = asBool(*p, r.child("trusted"));
    }
    if (const nlohmann::json* p = r.optional("numbered")) {
        c.numbered = asBool(*p, r.child("numbered"));
    }
    if (const nlohmann::json* p = r.optional("number")) {
        if (p->is_string()) {
            if (p->get<std::string>() != "n+1") {
                violate(r.child("number"), "number is an integer or \"n+1\"");
            }
            c.numberIsLastPlusOne = true;
        } else {
            c.fixedNumber = asInt32(*p, r.child("number"));
        }
        if (c.numbered) {
            violate(r.child("number"),
                    "a numbered class takes the running number and cannot also set number");
        }
    }
    r.closed();
    return c;
}

void readAnnotations(const nlohmann::json& j, const std::string& pointer, BeaconPairAnnotations& a) {
    ObjectReader r(j, pointer);
    if (const nlohmann::json* p = r.optional("enabled")) {
        a.enabled = asBool(*p, r.child("enabled"));
    }
    if (const nlohmann::json* p = r.optional("phase")) {
        if (asInt32(*p, r.child("phase")) != 2) {
            violate(r.child("phase"), "beacon-pair annotations run in phase 2");
        }
    }
    if (const nlohmann::json* p = r.optional("stint")) {
        ObjectReader s(*p, r.child("stint"));
        if (const nlohmann::json* q = s.optional("counterChannel")) {
            a.counterChannel = asString(*q, s.child("counterChannel"));
        }
        if (const nlohmann::json* q = s.optional("dataGapUs")) {
            a.dataGapUs = asInt(*q, s.child("dataGapUs"));
            if (a.dataGapUs <= 0) {
                violate(s.child("dataGapUs"), "dataGapUs is positive");
            }
        }
        s.closed();
    }
    if (const nlohmann::json* p = r.optional("pitStop")) {
        ObjectReader s(*p, r.child("pitStop"));
        if (const nlohmann::json* q = s.optional("stationarySec")) {
            a.stationarySec = asNumber(*q, s.child("stationarySec"));
            if (a.stationarySec <= 0.0) {
                violate(s.child("stationarySec"), "stationarySec is positive");
            }
        }
        if (const nlohmann::json* q = s.optional("speedChannel")) {
            a.speedChannel = asString(*q, s.child("speedChannel"));
        }
        if (const nlohmann::json* q = s.optional("stationaryBelow")) {
            ObjectReader v(*q, s.child("stationaryBelow"));
            const double value = asNumber(v.required("v"), v.child("v"));
            const std::string& unit = asString(v.required("unit"), v.child("unit"));
            const std::optional<double> factor = speedUnitToMps(unit);
            if (!factor) {
                violate(v.child("unit"), "'" + unit + "' is not a speed unit (m/s, km/h, mph, kn, ft/s)");
            }
            if (value < 0.0) {
                violate(v.child("v"), "stationaryBelow is not negative");
            }
            a.stationaryBelowMps = value * *factor;
            v.closed();
        }
        s.closed();
    }
    r.closed();
}

struct MarkerFilter {
    const BeaconPairSettings& s;
    std::set<MarkerClass> named;   // classes some transition names explicitly
    std::set<MarkerClass> ignored; // ignoreClasses

    explicit MarkerFilter(const BeaconPairSettings& settings) : s(settings) {
        for (const BeaconPairTransition& t : s.transitions) {
            for (const std::string* name : {&t.from, &t.to}) {
                if (*name != kAnyClass) {
                    if (const auto c = model::parseMarkerClass(*name)) {
                        named.insert(*c);
                    }
                }
            }
        }
        for (const std::string& name : s.ignoreClasses) {
            if (const auto c = model::parseMarkerClass(name)) {
                ignored.insert(*c);
            }
        }
    }

    bool inGroups(const Marker& m) const {
        return std::find(s.markerGroups.begin(), s.markerGroups.end(), m.group) != s.markerGroups.end();
    }
    // The markers the transition table runs over.
    bool relevant(const Marker& m) const {
        return m.enabled && inGroups(m) && ignored.count(m.cls) == 0 && named.count(m.cls) != 0;
    }
};

bool classMatches(std::string_view pattern, MarkerClass cls) {
    return pattern == kAnyClass || pattern == model::markerClassName(cls);
}

} // namespace

// ---- settings ---------------------------------------------------------------------------------------------

Result<BeaconPairSettings> BeaconPairSettings::fromDocument(const RuleDocument& document) {
    if (document.kind != "beacon-pair") {
        Error e{ErrorCode::Invalid,
                "rule '" + document.id + "' is of kind '" + document.kind + "', not beacon-pair",
                {}};
        e.context.detail = "/kind";
        return Failure{std::move(e)};
    }
    auto parsed = jsonio::parseJson(document.json.compact());
    if (!parsed) {
        return Failure{parsed.error()};
    }
    return jsonio::guardJson([&]() -> Result<BeaconPairSettings> {
        const nlohmann::json& j = *parsed;
        if (!j.is_object()) {
            violate("", "a rule-1 document is a JSON object");
        }
        ObjectReader r(j, "");
        detail::skipEnvelope(r);
        r.skip("id");
        r.skip("kind");
        r.skip("phase");
        r.skip("layers");
        BeaconPairSettings s;
        s.layers = document.layers.empty() ? std::vector<std::string>{"*"} : document.layers;
        s.transitions.clear();
        s.classes.clear();
        if (const nlohmann::json* p = r.optional("produces")) {
            s.produces = asString(*p, r.child("produces"));
        }
        if (s.produces.empty()) {
            violate(r.child("produces"), "produces is a group path");
        }
        s.parent = std::string(model::parentGroupPath(s.produces));
        if (const nlohmann::json* p = r.optional("parent")) {
            if (asString(*p, r.child("parent")) != s.parent) {
                violate(r.child("parent"), "parent must be the parent path of '" + s.produces + "'");
            }
        }
        if (const nlohmann::json* p = r.optional("markerGroups")) {
            s.markerGroups = stringArray(*p, r.child("markerGroups"));
        }
        if (const nlohmann::json* p = r.optional("ignoreClasses")) {
            s.ignoreClasses = stringArray(*p, r.child("ignoreClasses"));
            for (std::size_t i = 0; i < s.ignoreClasses.size(); ++i) {
                if (!model::parseMarkerClass(s.ignoreClasses[i])) {
                    violate(jsonio::pointerIndex(r.child("ignoreClasses"), i),
                            "'" + s.ignoreClasses[i] + "' is not a marker class");
                }
            }
        }
        {
            const std::string ptr = r.child("classes");
            const nlohmann::json& classesJson = r.required("classes");
            if (!classesJson.is_object() || classesJson.empty()) {
                violate(ptr, "classes is a non-empty object keyed by lap class");
            }
            for (const auto& [key, value] : classesJson.items()) {
                const std::string child = jsonio::pointerChild(ptr, key);
                if (!lapClassForKey(key)) {
                    violate(child, "'" + key + "' is not a lap class (flying, out, in, partial, pit)");
                }
                s.classes.emplace(key, readClass(value, child));
            }
        }
        {
            const std::string ptr = r.child("transitions");
            const nlohmann::json& transitionsJson = r.required("transitions");
            if (!transitionsJson.is_array() || transitionsJson.empty()) {
                violate(ptr, "transitions is a non-empty array");
            }
            for (std::size_t i = 0; i < transitionsJson.size(); ++i) {
                const std::string child = jsonio::pointerIndex(ptr, i);
                ObjectReader t(transitionsJson[i], child);
                BeaconPairTransition bt;
                bt.from = asString(t.required("from"), t.child("from"));
                bt.to = asString(t.required("to"), t.child("to"));
                bt.lapClass = asString(t.required("class"), t.child("class"));
                t.closed();
                if (bt.from != kAnyClass && !model::parseMarkerClass(bt.from)) {
                    violate(t.child("from"), "'" + bt.from + "' is not a marker class or \"*\"");
                }
                if (bt.to != kAnyClass && !model::parseMarkerClass(bt.to)) {
                    violate(t.child("to"), "'" + bt.to + "' is not a marker class or \"*\"");
                }
                if (s.classes.find(bt.lapClass) == s.classes.end()) {
                    violate(t.child("class"), "class '" + bt.lapClass + "' is not defined in classes");
                }
                s.transitions.push_back(std::move(bt));
            }
        }
        if (const nlohmann::json* p = r.optional("numbering")) {
            ObjectReader n(*p, r.child("numbering"));
            if (const nlohmann::json* q = n.optional("startDetail")) {
                s.startDetail = asString(*q, n.child("startDetail"));
            }
            if (const nlohmann::json* q = n.optional("default")) {
                s.defaultStart = asInt32(*q, n.child("default"));
            }
            n.closed();
        }
        if (const nlohmann::json* p = r.optional("annotations")) {
            readAnnotations(*p, r.child("annotations"), s.annotations);
        }
        if (const nlohmann::json* p = r.optional("groupFlags")) {
            ObjectReader g(*p, r.child("groupFlags"));
            if (const nlohmann::json* q = g.optional("supportsTrackGeneration")) {
                s.supportsTrackGeneration = asBool(*q, g.child("supportsTrackGeneration"));
            }
            if (const nlohmann::json* q = g.optional("supportsReporting")) {
                s.supportsReporting = asBool(*q, g.child("supportsReporting"));
            }
            g.closed();
        }
        r.closed();
        return s;
    });
}

BeaconPairSettings BeaconPairSettings::circuitDefault() {
    BeaconPairSettings s;
    s.transitions = {{"SOL", "BCN", "out"}, {"BCN", "BCN", "flying"},  {"BCN", "EOL", "in"},
                     {"SOL", "EOL", "out"}, {"*", "RESET", "partial"}, {"RESET", "BCN", "out"}};
    BeaconPairClass flying;
    flying.name = "Lap {n}";
    flying.abbrev = "L{n}";
    flying.trusted = true;
    flying.numbered = true;
    BeaconPairClass out;
    out.name = "Out Lap";
    out.abbrev = "Out";
    out.trusted = false;
    out.fixedNumber = 0;
    BeaconPairClass in;
    in.name = "In Lap";
    in.abbrev = "In";
    in.trusted = false;
    in.numberIsLastPlusOne = true;
    BeaconPairClass partial;
    partial.name = "Lap {n} (partial)";
    partial.abbrev = "P{n}";
    partial.trusted = false;
    partial.numbered = false;
    s.classes.emplace("flying", std::move(flying));
    s.classes.emplace("out", std::move(out));
    s.classes.emplace("in", std::move(in));
    s.classes.emplace("partial", std::move(partial));
    return s;
}

// ---- the rule ---------------------------------------------------------------------------------------------

BeaconPairRule::BeaconPairRule(BeaconPairSettings settings, std::string id)
    : settings_(std::move(settings)), id_(std::move(id)) {}

Result<std::unique_ptr<BeaconPairRule>> BeaconPairRule::fromDocument(const RuleDocument& document) {
    auto settings = BeaconPairSettings::fromDocument(document);
    if (!settings) {
        return Failure{settings.error()};
    }
    return std::make_unique<BeaconPairRule>(std::move(settings).value(), document.id);
}

bool BeaconPairRule::appliesTo(std::string_view layerName) const {
    return layerListMatches(settings_.layers, layerName);
}

Result<model::RangeGroupTree> BeaconPairRule::run(const model::DataLayer& layer,
                                                  const RuleContext& context) const {
    model::RangeGroupTree tree;
    const MarkerFilter filter(settings_);

    std::vector<const Marker*> seq;
    std::deque<Marker> implied; // EOLs implied by a SOL of an outing still open (stable addresses)
    bool anyBeacon = false;
    bool anySol = false;
    std::optional<Micros> lastRelevant;
    for (const Marker& m : layer.markers) {
        if (!filter.relevant(m)) {
            continue; // disabled markers, device events and other groups never bound a lap
        }
        lastRelevant = lastRelevant ? std::max(*lastRelevant, m.t) : m.t;
        if (m.cls == MarkerClass::SOL && !seq.empty() && seq.back()->cls != MarkerClass::EOL) {
            // A new outing starts while the previous one is open: that outing ends here, so its last lap (the
            // BCN -> EOL in lap) is kept instead of being dropped by the BCN -> SOL pair.
            Marker eol;
            eol.cls = MarkerClass::EOL;
            eol.t = m.t;
            eol.id = model::MarkerId{}; // nil: the bound has no marker
            implied.push_back(eol);
            seq.push_back(&implied.back());
        }
        seq.push_back(&m);
        anyBeacon = anyBeacon || m.cls == MarkerClass::BCN;
        anySol = anySol || m.cls == MarkerClass::SOL;
    }
    if (!anyBeacon) {
        return tree; // D-104: no beacons, no laps group
    }

    Marker virtualSol;
    if (!anySol) {
        virtualSol.cls = MarkerClass::SOL;
        virtualSol.t = 0;
        virtualSol.id = model::markerIdLogged(MarkerClass::SOL, 0, model::kGroupIntrinsic, 0);
        seq.insert(seq.begin(), &virtualSol);
    }
    Marker virtualEol;
    if (seq.back()->cls != MarkerClass::EOL) {
        // The end of the logged data; only without a duration the last relevant marker.
        virtualEol.cls = MarkerClass::EOL;
        virtualEol.t = layer.duration > 0 ? layer.duration : lastRelevant.value_or(0);
        virtualEol.id = model::MarkerId{}; // nil: the bound has no marker
        seq.push_back(&virtualEol);
    }

    int start = settings_.defaultStart;
    if (!settings_.startDetail.empty()) {
        const std::string wanted = caseFold(settings_.startDetail);
        for (const model::Detail& d : context.details) {
            if (caseFold(d.id) == wanted) {
                if (const std::optional<int> n = integralDetail(d.value)) {
                    start = *n;
                }
                break;
            }
        }
    }

    model::RangeGroup group = model::RangeGroup::make(settings_.produces);
    group.supportsTrackGeneration = settings_.supportsTrackGeneration;
    group.supportsReporting = settings_.supportsReporting;

    int next = start; // the number the next flying lap takes
    for (std::size_t k = 1; k < seq.size(); ++k) {
        const Marker& a = *seq[k - 1];
        const Marker& b = *seq[k];
        if (a.cls == MarkerClass::SOL) {
            next = start; // numbering is per outing
        }
        const BeaconPairTransition* match = nullptr;
        for (const BeaconPairTransition& t : settings_.transitions) {
            if (classMatches(t.from, a.cls) && classMatches(t.to, b.cls)) {
                match = &t;
                break;
            }
        }
        if (match == nullptr || b.t <= a.t) {
            continue;
        }
        const auto classIt = settings_.classes.find(match->lapClass);
        const std::optional<LapClass> lapClass = lapClassForKey(match->lapClass);
        if (classIt == settings_.classes.end() || !lapClass) {
            continue; // fromDocument rejects this; a hand-built settings object may not
        }
        const BeaconPairClass& cls = classIt->second;
        int number = next;
        if (cls.fixedNumber) {
            number = *cls.fixedNumber;
        } else if (cls.numbered) {
            ++next;
        }
        model::Range r;
        r.id = model::rangeId(group.path, a.id);
        r.kind = model::RangeKind::Lap;
        r.lapClass = *lapClass;
        r.startMarker = a.id;
        r.endMarker = b.id;
        r.start = a.t;
        r.end = b.t;
        r.name = expandTemplate(cls.name, number);
        r.abbrev = expandTemplate(cls.abbrev, number);
        r.number = number;
        r.trusted = cls.trusted;
        group.ranges.push_back(std::move(r));
    }
    tree.roots.push_back(std::move(group));
    return tree;
}

// ---- phase 2 ----------------------------------------------------------------------------------------------

Result<void> BeaconPairRule::annotate(const model::DataLayer& layer,
                                      const std::optional<store::Series>& speed, model::RangeGroup& laps,
                                      const CancelToken& cancel) const {
    const BeaconPairAnnotations& an = settings_.annotations;
    std::vector<model::Range>& ranges = laps.ranges;
    if (!an.enabled) {
        for (model::Range& r : ranges) {
            r.stint = 0;
            r.pitStop = false;
        }
        return ok();
    }
    if (ranges.empty()) {
        return ok();
    }
    constexpr Priority kPrio = Priority::Interactive;

    // Stationary periods and movement, per lap.
    const store::Series* speedSeries = speed ? &*speed : nullptr;
    if (speedSeries == nullptr && !an.speedChannel.empty()) {
        if (const model::Channel* c = layer.find(an.speedChannel)) {
            speedSeries = &c->series;
        }
    }
    struct LapMotion {
        bool pitStop = false;
        bool movedBeforeStop = false; // a moving sample before the first qualifying stationary run
        bool moved = false;           // a moving sample anywhere in the lap
    };
    std::vector<LapMotion> motion(ranges.size());
    if (speedSeries != nullptr) {
        const double threshold = an.stationaryBelowMps;
        const auto minRunUs = static_cast<Micros>(std::llround(an.stationarySec * 1e6));
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            const model::Range& lap = ranges[i];
            LapMotion& mo = motion[i];
            bool inRun = false;
            Micros runStart = 0;
            bool movedAtRunStart = false;
            const auto closeRun = [&](Micros endT) {
                if (inRun && !mo.pitStop && endT - runStart >= minRunUs) {
                    mo.pitStop = true;
                    mo.movedBeforeStop = movedAtRunStart;
                }
                inRun = false;
            };
            const auto visit = [&](Micros t, double v, bool valid) {
                if (valid && v < threshold) {
                    if (!inRun) {
                        inRun = true;
                        runStart = t;
                        movedAtRunStart = mo.moved;
                    }
                } else {
                    closeRun(t);
                }
                if (valid && v >= threshold) {
                    mo.moved = true;
                }
            };
            auto visited = detail::forEachSample(*speedSeries, lap.start, lap.end, kPrio, cancel, visit);
            if (!visited) {
                return Failure{visited.error()};
            }
            closeRun(lap.end);
        }
    }

    // Stint boundaries: a new stint starts at the first lap whose start is >= b.
    const MarkerFilter filter(settings_);
    std::vector<Micros> bounds;
    std::optional<Micros> lastEol;
    for (const Marker& m : layer.markers) {
        if (!m.enabled || !filter.inGroups(m) || filter.ignored.count(m.cls) != 0) {
            continue;
        }
        if (m.cls == MarkerClass::RESET) {
            bounds.push_back(m.t);
        } else if (m.cls == MarkerClass::EOL) {
            lastEol = m.t;
        } else if (m.cls == MarkerClass::SOL) {
            if (lastEol && m.t - *lastEol > an.dataGapUs) {
                bounds.push_back(m.t);
            }
            lastEol.reset();
        }
    }
    if (!an.counterChannel.empty()) {
        if (const model::Channel* c = layer.find(an.counterChannel)) {
            bool have = false;
            double prev = 0.0;
            Micros prevT = 0;
            const auto visit = [&](Micros t, double v, bool valid) {
                if (!valid) {
                    return;
                }
                if (have && v < prev) {
                    bounds.push_back(prevT + 1);
                }
                have = true;
                prev = v;
                prevT = t;
            };
            auto visited = detail::forEachSample(c->series, kPrio, cancel, visit);
            if (!visited) {
                return Failure{visited.error()};
            }
        }
    }
    std::vector<const store::Column*> timeColumns;
    for (const model::Channel& c : layer.channels) {
        const auto* ts = std::get_if<store::TimestampedSeries>(&c.series);
        if (ts == nullptr || ts->size() < 2) {
            continue;
        }
        const bool seen = std::any_of(timeColumns.begin(), timeColumns.end(),
                                      [&](const store::Column* col) { return *col == ts->timesUs; });
        if (seen) {
            continue;
        }
        timeColumns.push_back(&ts->timesUs);
        bool have = false;
        Micros prevT = 0;
        const auto visit = [&](Micros t, double, bool) {
            if (have && t - prevT > an.dataGapUs) {
                bounds.push_back(t);
            }
            have = true;
            prevT = t;
        };
        auto visited = detail::forEachSample(c.series, kPrio, cancel, visit);
        if (!visited) {
            return Failure{visited.error()};
        }
    }
    std::sort(bounds.begin(), bounds.end());

    std::size_t k = 0;
    std::uint16_t stint = 1;
    bool movedInStint = false;
    std::optional<Micros> pitBoundary;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        model::Range& r = ranges[i];
        bool crossed = false;
        while (k < bounds.size() && bounds[k] <= r.start) {
            crossed = true;
            ++k;
        }
        if (pitBoundary && *pitBoundary <= r.start) {
            crossed = true;
            pitBoundary.reset();
        }
        if (crossed && i > 0) {
            if (stint < std::numeric_limits<std::uint16_t>::max()) {
                ++stint;
            }
            movedInStint = false;
        }
        r.stint = stint;
        r.pitStop = motion[i].pitStop;
        if (motion[i].pitStop && (movedInStint || motion[i].movedBeforeStop)) {
            pitBoundary = r.end;
        }
        movedInStint = movedInStint || motion[i].moved;
    }
    return ok();
}

} // namespace esses::analysis
