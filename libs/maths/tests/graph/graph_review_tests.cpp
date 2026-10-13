// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Regressions of the M2 review (D-185, D-186) through MathGraph: the rate of a plan that reads a channel only
// through a statistic (03 section 6.4 rule 2), the dtype of integer and Bool results (03 section 6.5) and of
// those converted from a declared unit, non-finite results invalid (03 sections 5.1, 6.6), the synthesised
// self reference of filter and scale_offset items, scale_offset in the offset's unit for affine units, filter
// windows of 0 and 1 points, LapScalar and window results stepping, the recursive previous_sample form as
// E-CYCLE, a moved math window re-keying ungated statistics, variance against another source's lap, the
// table axis checked against its unit and the nominal rate of a timestamped series. Expected values come
// from the definitions in 03, not from the code under test (D-041).

#include "graph/graph_internal.hpp"
#include "graph_test_support.hpp"

#include <esses/base/uuid.hpp>
#include <esses/maths/math_graph.hpp>
#include <esses/maths/whole_signal.hpp>
#include <esses/store/memory_provider.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::maths;
using test::expr;

namespace {

MathGraph graphWith(test::Runtime& rt, const std::shared_ptr<const model::DataSource>& src,
                    std::vector<MathItem> items) {
    MathGraph g = test::makeGraph(rt, src);
    const MathModule m = test::module("user", std::move(items)); // setModules copies the modules
    g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr);
    return g;
}

analysis::UniformSignal signalOf(MathGraph& g, std::string_view name) {
    Result<ColumnHandle> r = test::evaluate(g, name);
    if (!r) {
        FAIL(std::string(name) + ": " + r.error().toString());
    }
    Result<analysis::UniformSignal> s = readWholeSignal(r->series, {});
    REQUIRE(s);
    return std::move(s).value();
}

double meanOf(double first, std::size_t count) {
    return first + (static_cast<double>(count) - 1.0) / 2.0;
}

model::Range lap(model::DataSource& src, int number, Micros start, Micros end) {
    model::Range r;
    r.kind = model::RangeKind::Lap;
    r.number = number;
    r.start = start;
    r.end = end;
    r.name = "Lap " + std::to_string(number);
    r.id = model::RangeId{uuidFromHash(hash128("review-lap-" + std::to_string(number) + src.id.hex()))};
    return r;
}

std::shared_ptr<model::DataSource> withLaps(const std::shared_ptr<model::DataSource>& src, Micros split) {
    auto copy = std::make_shared<model::DataSource>(*src);
    model::DataLayer& layer = copy->layers.front();
    layer.ranges.roots.clear();
    model::RangeGroup outings = model::RangeGroup::make("Outings");
    model::RangeGroup laps = model::RangeGroup::make("Outings:Laps");
    laps.ranges = {lap(*copy, 1, 0, split), lap(*copy, 2, split, layer.duration)};
    laps.version = static_cast<std::uint64_t>(split);
    REQUIRE(layer.ranges.setGroup(std::move(outings)));
    REQUIRE(layer.ranges.setGroup(std::move(laps)));
    return copy;
}

} // namespace

TEST_CASE("math graph review: a channel read only by a statistic sets the result rate (03 6.4 rule 2)",
          "[unit][graph][review]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache); // rpm 100 Hz, Temp 10 Hz (300 + i K, 200 samples)
    MathGraph g = graphWith(rt, src,
                            {expr("Mean", "stat_mean('Temp'[K])"), expr("Sd", "stat_std_dev('Temp'[K])"),
                             expr("Centred", "'Temp'[K] - stat_mean('Temp'[K])")});
    for (const char* name : {"Mean", "Sd", "Centred"}) {
        INFO(name);
        const analysis::UniformSignal s = signalOf(g, name);
        CHECK(s.rate == Rate::fromHz(10)); // not the layer's 100 Hz
        CHECK(s.values.size() == 200);
    }
    // Over the samples as logged, not linearly upsampled: the mean and the population deviation of 0..199.
    CHECK(std::abs(signalOf(g, "Mean").values[17] - meanOf(300.0, 200)) < 1e-3);
    CHECK(std::abs(signalOf(g, "Sd").values[3] - std::sqrt((200.0 * 200.0 - 1.0) / 12.0)) < 1e-3);
}

TEST_CASE("math graph review: integer and Bool results with a declared unit are stored as real values",
          "[unit][graph][review]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathItem half = expr("Half", "choose('rpm' > 0, 50, 0)");
    half.quantity = *test::shippedUnits()->quantityByName("Ratio");
    half.resultUnit = test::unitOf("%");
    MathItem whole = expr("Whole", "int('Temp'[deg C])");
    whole.quantity = *test::shippedUnits()->quantityByName("Temperature");
    whole.resultUnit = test::unitOf("deg C");
    MathItem above = expr("Above", "'rpm' > 1000.5");
    above.quantity = *test::shippedUnits()->quantityByName("Ratio");
    above.resultUnit = test::unitOf("%");
    MathGraph g = graphWith(rt, src, {half, whole, above});

    const analysis::UniformSignal h = signalOf(g, "Half");
    CHECK(std::abs(h.values[5] - 0.5) < 1e-6); // 50 % stored as the ratio 0.5, not truncated to 0
    CHECK(g.descriptor(test::keyOf(g, "Half"))->dtype != store::Dtype::I32);
    CHECK(g.descriptor(test::keyOf(g, "Half"))->kind == model::SampleKind::Real);
    // 300 K is 26.85 deg C, int() of it 26 deg C = 299.15 K (not 299).
    CHECK(std::abs(signalOf(g, "Whole").values[0] - 299.15) < 1e-3);
    CHECK(std::abs(signalOf(g, "Above").values[600] - 0.01) < 1e-6); // true = 1 % = 0.01
}

TEST_CASE("math graph review: integer results that may leave the I32 range are F64 (03 6.5)",
          "[unit][graph][review]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = graphWith(rt, src,
                            {expr("Big", "int('rpm' * 10000000)"), expr("Flag", "'rpm' > 2000"),
                             expr("Pick", "choose('rpm' > 2000, 1, 0)"), expr("Small", "int(5) + 2")});
    CHECK(g.descriptor(test::keyOf(g, "Big"))->dtype == store::Dtype::F64);
    CHECK(g.descriptor(test::keyOf(g, "Big"))->kind == model::SampleKind::Integer);
    CHECK(g.descriptor(test::keyOf(g, "Flag"))->dtype == store::Dtype::I32);
    CHECK(g.descriptor(test::keyOf(g, "Pick"))->dtype == store::Dtype::I32);
    CHECK(g.descriptor(test::keyOf(g, "Small"))->dtype == store::Dtype::I32);
    const std::vector<float> rpm = test::rpmValues();
    const std::vector<double> big = test::values(g, "Big");
    REQUIRE(big.size() == rpm.size());
    for (std::size_t i = 0; i < rpm.size(); i += 97) {
        CHECK(big[i] == std::trunc(static_cast<double>(rpm[i]) * 1e7)); // not saturated at 2^31 - 1
    }
}

TEST_CASE("math graph review: non-finite results are invalid (03 5.1, 6.6)", "[unit][graph][review]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g =
        graphWith(rt, src,
                  {expr("Pow", "0 ^ (0 - 'rpm')"), expr("Exp", "exp(1000 + 'rpm')"),
                   expr("F32", "'rpm' * 1e38"), expr("Div", "'rpm' / 1e-320"), expr("Ok", "'rpm' * 2")});
    for (const char* name : {"Pow", "Exp", "F32", "Div"}) {
        INFO(name);
        const analysis::UniformSignal s = signalOf(g, name);
        REQUIRE(s.values.size() == test::kRpmSamples);
        for (std::size_t i = 0; i < s.values.size(); i += 101) {
            CHECK_FALSE(s.validAt(i));
            CHECK_FALSE(std::isinf(s.values[i]));
        }
    }
    CHECK(signalOf(g, "Ok").validAt(3));
}

TEST_CASE("math graph review: filter and scale_offset read their input without W-UNIT-MISSING",
          "[unit][graph][review]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    MathItem filter;
    filter.kind = ItemKind::Filter;
    filter.name = "Temp";
    filter.window.points = 3;
    MathItem scale;
    scale.kind = ItemKind::ScaleOffset;
    scale.name = "Temp";
    scale.scale = 1.0;
    scale.offset = UnitValue{1.0, test::unitOf("K")};
    const MathModule local = test::module("local", {filter}, MathModule::Origin::Local);
    const MathModule global = test::module("user", {scale});
    g.setModules(std::span<const MathModule>(&global, 1), &local, nullptr);
    static_cast<void>(test::values(g, "Temp"));
    for (const MathNodeInfo& n : g.nodes()) {
        INFO(n.moduleId);
        CHECK_FALSE(test::hasCode(n.status.diags, "W-UNIT-MISSING"));
    }
    CHECK(g.status(test::keyOf(g, "Temp")).state == MathState::Ok);
}

TEST_CASE("math graph review: scale_offset scales a temperature in the offset's unit (D-186)",
          "[unit][graph][review]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache); // Temp 300 + i K
    MathGraph g = test::makeGraph(rt, src);
    MathItem so;
    so.kind = ItemKind::ScaleOffset;
    so.name = "Temp";
    so.scale = 1.02;
    so.offset = UnitValue{0.0, test::unitOf("deg C")};
    const MathModule local = test::module("local", {so}, MathModule::Origin::Local);
    g.setModules({}, &local, nullptr);
    const std::vector<double> v = test::values(g, "Temp");
    REQUIRE(v.size() == 200);
    for (std::size_t i = 0; i < v.size(); i += 11) {
        const double celsius = 300.0 + static_cast<double>(i) - 273.15;
        CHECK(std::abs(v[i] - (celsius * 1.02 + 273.15)) < 1e-3); // 80 deg C * 1.02 = 81.6 deg C
    }
    CHECK(g.descriptor(test::keyOf(g, "Temp"))->unit == test::unitOf("K"));
}

TEST_CASE("math graph review: a filter item of 0 or 1 points is a window of 2 (03 5.2)",
          "[unit][graph][review]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    for (const int points : {0, 1}) {
        INFO(points);
        MathGraph g = test::makeGraph(rt, src);
        MathItem f;
        f.kind = ItemKind::Filter;
        f.name = "Temp";
        f.window.points = points;
        const MathModule local = test::module("local", {f}, MathModule::Origin::Local);
        g.setModules({}, &local, nullptr);
        const std::vector<double> v = test::values(g, "Temp");
        REQUIRE(v.size() == 200);
        CHECK(std::abs(v[10] - 309.5) < 1e-4); // (309 + 310) / 2: the window [i - 1, i]
    }
    MathGraph g = test::makeGraph(rt, src);
    MathItem bad;
    bad.kind = ItemKind::Filter;
    bad.name = "Temp";
    bad.window.points = -1;
    MathItem secs = bad;
    secs.name = "rpm";
    secs.window.points.reset();
    secs.window.seconds = 0.0;
    const MathModule local = test::module("local", {bad, secs}, MathModule::Origin::Local);
    g.setModules({}, &local, nullptr);
    CHECK(test::hasCode(g.status(test::keyOf(g, "Temp")).diags, "E-ARG-VALUE"));
    CHECK(g.status(test::keyOf(g, "rpm")).state == MathState::Pending); // 0 s: a window of 2
}

TEST_CASE("math graph review: per-lap and window constants step (Previous interpolation)",
          "[unit][graph][review]") {
    test::Runtime rt;
    auto src = withLaps(test::graphSource(rt.cache), 10'000'000);
    MathItem explicitInterp = expr("Lap Mean Linear", "stat_mean('rpm', 1, range_change(\"Outings:Laps\"))");
    explicitInterp.interp = store::Interp::Closest;
    MathGraph g = graphWith(rt, src,
                            {expr("Lap Mean", "stat_mean('rpm', 1, range_change(\"Outings:Laps\"))"),
                             expr("Mean", "stat_mean('rpm')"), explicitInterp, expr("Plain", "'rpm' * 2")});
    CHECK(g.descriptor(test::keyOf(g, "Lap Mean"))->interp == store::Interp::Previous);
    CHECK(g.descriptor(test::keyOf(g, "Mean"))->interp == store::Interp::Previous);
    CHECK(g.descriptor(test::keyOf(g, "Lap Mean Linear"))->interp == store::Interp::Closest);
    CHECK(g.descriptor(test::keyOf(g, "Plain"))->interp == store::Interp::Linear);
}

TEST_CASE("math graph review: previous_sample of its own name with nothing before it is E-CYCLE (03 5.4)",
          "[unit][graph][review]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = graphWith(rt, src,
                            {expr("Prev", "previous_sample('Prev') + 1"), expr("Z", "'Z' + 1"),
                             expr("rpm", "previous_sample('rpm') + 1")});
    const MathStatus prev = g.status(test::keyOf(g, "Prev"));
    CHECK(prev.state == MathState::Error);
    CHECK(test::hasCode(prev.diags, "E-CYCLE"));
    CHECK_FALSE(test::hasCode(prev.diags, "E-UNKNOWN-CHANNEL"));
    CHECK(test::hasCode(g.status(test::keyOf(g, "Z")).diags, "E-UNKNOWN-CHANNEL")); // no previous_sample
    // With a logged channel before it, previous_sample reads that channel.
    CHECK(g.status(test::keyOf(g, "rpm")).state == MathState::Pending);
    const std::vector<double> rpm = test::values(g, "rpm");
    const std::vector<float> x = test::rpmValues();
    CHECK(std::abs(rpm[5] - (static_cast<double>(x[4]) + 1.0)) < 1e-3);
}

TEST_CASE("math graph review: a moved math window re-keys an ungated statistic", "[unit][graph][review]") {
    test::Runtime rt;
    auto base = test::graphSource(rt.cache); // rpm: a 5 s saw tooth, 20 s
    auto src = withLaps(base, 10'000'000);
    MathGraph g = graphWith(rt, src, {expr("Mean", "stat_mean('rpm')")});
    MathSelection sel; // Main in another source; this source's window is its selected overlay, lap 1
    sel.main.ref.src = hash128("another source");
    sel.main.ref.layer = "Normal";
    sel.main.start = 0;
    sel.main.end = 1'000'000;
    model::RangeRef overlay;
    overlay.src = src->id;
    overlay.layer = "Normal";
    overlay.groupPath = "Outings:Laps";
    overlay.range = src->layers.front().ranges.find("Outings:Laps")->ranges.front().id;
    sel.overlays.push_back(overlay);
    sel.key = hash128("review: overlay lap 1");
    static_cast<void>(g.setSelection(sel));
    const double tenSeconds = test::values(g, "Mean")[0];

    // A marker edit ends lap 1 at 2.5 s: the same range (its start marker), the same selection key.
    auto moved = withLaps(base, 2'500'000);
    const std::vector<ChannelKey> changed = g.setSource(moved);
    CHECK(std::find(changed.begin(), changed.end(), test::keyOf(g, "Mean")) != changed.end());
    CHECK(g.status(test::keyOf(g, "Mean")).state == MathState::Pending);
    const double quarter = test::values(g, "Mean")[0];
    CHECK(quarter != tenSeconds);
    const std::vector<float> x = test::rpmValues();
    double sum = 0.0;
    for (std::size_t i = 0; i < 250; ++i) {
        sum += static_cast<double>(x[i]);
    }
    CHECK(std::abs(quarter - sum / 250.0) < 1e-2);
}

TEST_CASE("math graph review: variance against another source's lap is E-REF-SOURCE, not all-invalid OK",
          "[unit][graph][review]") {
    test::Runtime rt;
    auto src = withLaps(test::graphSource(rt.cache), 10'000'000);
    MathGraph g = graphWith(rt, src, {expr("Var", "variance_time('rpm')")});
    const model::Range& lap1 = src->layers.front().ranges.find("Outings:Laps")->ranges.front();
    MathSelection sel;
    sel.main.ref.src = src->id;
    sel.main.ref.layer = "Normal";
    sel.main.ref.groupPath = "Outings:Laps";
    sel.main.ref.range = lap1.id;
    sel.main.start = lap1.start;
    sel.main.end = lap1.end;
    MathRangeGeom ref;
    ref.ref.src = hash128("another source");
    ref.ref.layer = "Normal";
    ref.ref.groupPath = "Outings:Laps";
    ref.start = 0;
    ref.end = 10'000'000;
    sel.reference = ref;
    sel.key = hash128("review: reference elsewhere");
    static_cast<void>(g.setSelection(sel));
    Result<ColumnHandle> r = test::evaluate(g, "Var");
    REQUIRE_FALSE(r);
    const MathStatus s = g.status(test::keyOf(g, "Var"));
    CHECK(s.state == MathState::Error);
    CHECK(test::hasCode(s.diags, "E-REF-SOURCE"));
    // The reference lap in this source evaluates.
    sel.reference->ref.src = src->id;
    sel.key = hash128("review: reference here");
    static_cast<void>(g.setSelection(sel));
    CHECK(test::evaluate(g, "Var").ok());
}

TEST_CASE("math graph review: a table axis is checked against its unit", "[unit][graph][review]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathItem scaled;
    scaled.kind = ItemKind::Table2D;
    scaled.name = "Scaled";
    scaled.table.x.expr = "'Temp'[K]";
    scaled.table.x.unit = test::unitOf("deg F");
    scaled.table.x.breakpoints = {0.0, 1000.0};
    scaled.table.values = {0.0, 1.0};
    MathItem dim = scaled;
    dim.name = "Dim";
    dim.table.x.expr = "'Temp'[K]";
    dim.table.x.unit = test::unitOf("m/s");
    MathItem right = scaled;
    right.name = "Right";
    right.table.x.expr = "'Temp'[deg F]";
    MathGraph g = graphWith(rt, src, {scaled, dim, right});
    const MathStatus s = g.status(test::keyOf(g, "Scaled"));
    REQUIRE(test::hasCode(s.diags, "I-RESULT-SCALE"));
    for (const Diagnostic& d : s.diags) {
        if (d.code == "I-RESULT-SCALE") {
            CHECK_THAT(d.message, Catch::Matchers::ContainsSubstring("the axis unit"));
        }
    }
    CHECK(test::hasCode(g.status(test::keyOf(g, "Dim")).diags, "W-RESULT-DIM"));
    CHECK_FALSE(test::hasCode(g.status(test::keyOf(g, "Right")).diags, "I-RESULT-SCALE"));
}

TEST_CASE("math graph review: a timestamped series has a whole-Hz nominal rate (03 6.4 rule 2)",
          "[unit][graph][review]") {
    store::ChunkCache cache(std::size_t{64} << 20);
    const auto series = [&](std::size_t n, Micros tMin, Micros tMax) {
        store::TimestampedSeries ts;
        store::ColumnOptions o;
        o.cache = &cache;
        ts.values = store::Column(store::InMemoryProvider::fromF32(std::vector<float>(n, 1.0F)), o);
        ts.timesUs = store::Column(store::InMemoryProvider::fromI64(std::vector<std::int64_t>(n, 0)), o);
        ts.tMin = tMin;
        ts.tMax = tMax;
        return ts;
    };
    // Three samples over 1.0001 s: about 2 Hz, not 1 / 500,050 us.
    CHECK(graph::timestampedRate(series(3, 0, 1'000'100)) == Rate::fromHz(2));
    CHECK(graph::timestampedRate(series(21, 5'000, 2'005'000)) == Rate::fromHz(10));
    CHECK(graph::timestampedRate(series(2, 0, 4'000'000)) == Rate::fromPeriodUs(4'000'000)); // below 1 Hz
    CHECK_FALSE(graph::timestampedRate(series(1, 0, 0)).valid());

    // A layer of timestamped channels only has a highest rate (the result rate of a constant).
    model::DataLayer layer;
    model::Channel ch;
    ch.desc.name = "sparse";
    ch.series = series(21, 5'000, 2'005'000);
    layer.addChannel(std::move(ch));
    CHECK(graph::highestRate(layer) == Rate::fromHz(10));
}
