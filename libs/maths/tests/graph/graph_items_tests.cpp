// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathGraph item kinds (03 sections 4.1, 4.4, 6.4, 8; 07 section 5.1): expression (result unit to base,
// descriptor, explicit rate), scale_offset (computed in the offset's unit, D-186; the shadowed channel's
// descriptor), table2d (the axis expression and the table stage), the kinds evaluated from v1.0 or never
// (table3d, regression, plugin), built-in outputs read by items, and a `builtin` item that overrides a
// registered module's settings for this source.

#include "graph_test_support.hpp"

#include <esses/maths/math_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::maths;
using test::expr;

TEST_CASE("math graph items: an expression with a declared unit is stored in base units",
          "[unit][graph][items]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    MathItem celsius = expr("Temp C", "'Temp'[deg C] + 0");
    celsius.quantity = *test::shippedUnits()->quantityByName("Temperature");
    celsius.resultUnit = test::unitOf("deg C");
    celsius.display.dps = 3;
    celsius.display.colourIndex = 4;
    MathItem slow = expr("Slow", "'rpm' * 1");
    slow.rate.automatic = false;
    slow.rate.rate = Rate::fromHz(20);
    MathModule m = test::module("user", {celsius, slow});
    g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr);

    const std::optional<model::ChannelDescriptor> d = g.descriptor(test::keyOf(g, "Temp C"));
    REQUIRE(d);
    CHECK(d->unit == test::unitOf("K")); // the values are stored in the base unit (03 section 3.2 rule 11)
    CHECK(d->quantity == celsius.quantity);
    CHECK(d->dps == 3);
    CHECK(d->colourIndex == 4);
    CHECK(d->source == model::ChannelSource::Maths);
    CHECK(d->rate == Rate::fromHz(10));
    CHECK(d->statusText == "Pending");
    const std::vector<double> v = test::values(g, "Temp C");
    REQUIRE(v.size() == 200);
    CHECK(std::abs(v[7] - 307.0) < 1e-4); // 307 K -> 33.85 deg C -> stored as 307 K again

    const std::vector<double> s = test::values(g, "Slow");
    CHECK(s.size() == 400); // 20 s at 20 Hz
    CHECK(g.descriptor(test::keyOf(g, "Slow"))->rate == Rate::fromHz(20));
}

TEST_CASE("math graph items: scale_offset computes in the offset's unit and stores the base unit",
          "[unit][graph][items]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    MathItem so;
    so.kind = ItemKind::ScaleOffset;
    so.name = "Temp";
    so.scale = 2.0;
    so.offset = UnitValue{9.0, test::unitOf("deg F")}; // y = x * 2 + 9 in deg F (D-186)
    const MathModule local = test::module("local", {so}, MathModule::Origin::Local);
    g.setModules({}, &local, nullptr);
    const std::vector<double> v = test::values(g, "Temp");
    REQUIRE(v.size() == 200);
    for (std::size_t i = 0; i < v.size(); i += 13) {
        const double fahrenheit = (300.0 + static_cast<double>(i) - 273.15) * 1.8 + 32.0;
        CHECK(std::abs(v[i] - ((fahrenheit * 2.0 + 9.0 - 32.0) / 1.8 + 273.15)) < 1e-3);
    }
    // An offset in the stored unit (or without one) applies in that unit: 03 section 4.4's base form.
    MathItem kelvin = so;
    kelvin.offset = UnitValue{5.0, test::unitOf("K")};
    const MathModule local2 = test::module("local", {kelvin}, MathModule::Origin::Local);
    g.setModules({}, &local2, nullptr);
    const std::vector<double> k = test::values(g, "Temp");
    REQUIRE(k.size() == 200);
    for (std::size_t i = 0; i < k.size(); i += 13) {
        CHECK(std::abs(k[i] - ((300.0 + static_cast<double>(i)) * 2.0 + 5.0)) < 1e-3);
    }
    g.setModules({}, &local, nullptr);
    const std::optional<model::ChannelDescriptor> d = g.descriptor(test::keyOf(g, "Temp"));
    REQUIRE(d);
    CHECK(d->unit == test::unitOf("K"));
    CHECK(d->dps == 1); // kept from the shadowed channel (03 section 8)
    CHECK(d->id == model::channelIdMathsLocal(src->id, "Normal", "Temp"));
    const std::vector<MathNodeInfo> nodes = g.nodes();
    REQUIRE(nodes.size() == 1);
    CHECK(nodes[0].kind == ItemKind::ScaleOffset);
    CHECK(nodes[0].selfMaths);
    CHECK(nodes[0].inputs[0].kind == MathInput::Kind::Logged);
}

TEST_CASE("math graph items: table2d looks the axis expression up in the table", "[unit][graph][items]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    MathItem t;
    t.kind = ItemKind::Table2D;
    t.name = "Force";
    t.table.x.expr = "'rpm'";
    t.table.x.breakpoints = {1000.0, 2000.0, 6000.0};
    t.table.valuesUnit = test::unitOf("kN");
    t.table.values = {0.0, 1.0, 5.0};
    t.quantity = *test::shippedUnits()->quantityByName("Force");
    t.resultUnit = test::unitOf("N");
    MathItem bad = t;
    bad.name = "Bad";
    bad.table.x.breakpoints = {2.0, 1.0, 3.0};
    MathModule m = test::module("user", {t, bad});
    g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr);
    const std::vector<double> v = test::values(g, "Force");
    const std::vector<float> x = test::rpmValues();
    REQUIRE(v.size() == x.size());
    for (std::size_t i = 0; i < x.size(); i += 17) {
        const double xi = static_cast<double>(x[i]);
        const double expected = xi <= 1000.0   ? 0.0 // clamp below the first breakpoint
                                : xi <= 2000.0 ? (xi - 1000.0) / 1000.0 * 1000.0
                                               : 1000.0 + (xi - 2000.0) / 4000.0 * 4000.0;
        CHECK(std::abs(v[i] - expected) < 1e-2); // N (the base unit of kN)
    }
    CHECK(g.descriptor(test::keyOf(g, "Force"))->unit == test::unitOf("N"));
    const MathStatus b = g.status(test::keyOf(g, "Bad"));
    CHECK(b.state == MathState::Error);
    CHECK(test::hasCode(b.diags, "E-TABLE-AXIS"));
}

TEST_CASE("math graph items: kinds without an evaluation in 0.1 keep their item and show why",
          "[unit][graph][items]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    MathItem t3;
    t3.kind = ItemKind::Table3D;
    t3.name = "Aero";
    MathItem reg;
    reg.kind = ItemKind::Regression;
    reg.name = "Fit";
    MathItem plugin;
    plugin.kind = ItemKind::Plugin;
    plugin.name = "Plug";
    MathItem broken = expr("Broken", "'rpm' * (2");
    MathModule m = test::module("user", {t3, reg, plugin, broken});
    g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr);
    CHECK(test::hasCode(g.status(test::keyOf(g, "Aero")).diags, "E-KIND-UNSUPPORTED"));
    CHECK(test::hasCode(g.status(test::keyOf(g, "Fit")).diags, "E-KIND-UNSUPPORTED"));
    CHECK(test::hasCode(g.status(test::keyOf(g, "Plug")).diags, "E-PLUGIN-UNAVAILABLE"));
    const MathStatus s = g.status(test::keyOf(g, "Broken"));
    CHECK(s.state == MathState::Error);
    CHECK(test::hasCode(s.diags, "E-PAREN-UNBALANCED"));
    CHECK(g.descriptor(test::keyOf(g, "Broken"))->statusText.starts_with("Error: "));
    CHECK(g.require(test::keyOf(g, "Broken"), SampleRange::all(), Priority::Visible, {}).get().error().code ==
          ErrorCode::Parse);
    // define() reports the item's diagnostics; the node exists.
    Diagnostics d;
    const NodeId id = g.define(m, expr("Broken", "'rpm' +* 2"), d);
    CHECK(hasError(d));
    CHECK(id.key == test::keyOf(g, "Broken"));
    CHECK(g.status(id).state == MathState::Error);
    Diagnostics none;
    CHECK(g.define(m, MathItem{}, none) == NodeId{});
    CHECK(test::hasCode(none, "E-NAME-EMPTY"));
}

TEST_CASE("math graph items: items read built-in outputs, and a builtin item overrides the module settings",
          "[unit][graph][items]") {
    test::Runtime rt;
    std::vector<model::Channel> channels;
    std::vector<float> speed(3000);
    for (std::size_t i = 0; i < speed.size(); ++i) {
        speed[i] = static_cast<float>(100.0 + static_cast<double>(i % 100));
    }
    channels.push_back(
        test::uniformF32(rt.cache, "Speed In", test::unitOf("km/h"), Rate::fromHz(100), speed));
    channels.push_back(
        test::uniformF32(rt.cache, "Other In", units::kNoUnit, Rate::fromHz(10), {1.F, 2.F, 3.F}));
    auto src = test::makeSource(std::move(channels), 30'000'000);
    MathGraph g = test::makeGraph(rt, src);
    test::FakeModule module;
    Diagnostics diags;
    REQUIRE(g.defineBuiltin(module, {}, diags));
    MathModule user = test::module("user", {expr("Speed x2", "'Fake Speed'[km/h] * 2")});
    g.setModules(std::span<const MathModule>(&user, 1), nullptr, nullptr);
    const std::vector<double> v = test::values(g, "Speed x2");
    REQUIRE(v.size() == 3000);
    // Fake Speed = gain 2 x Speed In in m/s, stored F32; read back in km/h and doubled.
    CHECK(std::abs(v[42] - 2.0 * 2.0 * static_cast<double>(speed[42])) < 1e-2);
    CHECK(module.speedEvals->load() == 1);

    // A builtin item in the local maths overrides the registration's settings for this source.
    MathItem override;
    override.kind = ItemKind::Builtin;
    override.name = "fake";
    override.moduleId = "test.fake";
    override.settings = test::FakeModule::json(R"({"gain": 3})");
    const MathModule local = test::module("local", {override}, MathModule::Origin::Local);
    const std::vector<ChannelKey> changed =
        g.setModules(std::span<const MathModule>(&user, 1), &local, nullptr);
    CHECK(changed.size() == 4); // the module's three outputs (new settings) and Speed x2
    const std::vector<double> v3 = test::values(g, "Speed x2");
    CHECK(std::abs(v3[42] - 2.0 * 3.0 * static_cast<double>(speed[42])) < 1e-2);

    // Disabled, it disables the module's outputs for this source.
    override.enabled = false;
    const MathModule localOff = test::module("local", {override}, MathModule::Origin::Local);
    g.setModules(std::span<const MathModule>(&user, 1), &localOff, nullptr);
    CHECK(g.status(test::keyOf(g, "Fake Speed")).state == MathState::Disabled);
    CHECK_FALSE(test::evaluate(g, "Speed x2"));
    CHECK(g.status(test::keyOf(g, "Speed x2")).state == MathState::Error);
}

TEST_CASE("math graph items: integrate_dist reads the chain head of Corr Dist", "[unit][graph][items]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    // A stand-in Corr Dist: 10 m/s over the layer, so 0.1 m per 100 Hz sample.
    MathItem dist = expr("Corr Dist", "integrate(10)");
    dist.quantity = *test::shippedUnits()->quantityByName("Distance");
    dist.resultUnit = test::unitOf("m");
    MathModule m = test::module("user", {dist, expr("Dist Count", "integrate_dist(1)")});
    g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr);
    const std::vector<MathNodeInfo> nodes = g.nodes();
    const MathNodeInfo* counter = test::nodeInfo(nodes, "user", "Dist Count");
    REQUIRE(counter != nullptr);
    REQUIRE(counter->inputs.size() == 1);
    CHECK(counter->inputs[0].name == "Corr Dist");
    CHECK(counter->inputs[0].kind == MathInput::Kind::Maths);
    const std::vector<double> d = test::values(g, "Corr Dist");
    const std::vector<double> c = test::values(g, "Dist Count");
    REQUIRE(c.size() == d.size());
    CHECK(std::abs(c.back() - (d.back() - d.front())) < 1e-2 * std::abs(d.back()) + 1e-3);

    // Editing Corr Dist re-versions its integrate_dist reader.
    const Version before = g.version(test::keyOf(g, "Dist Count"));
    Diagnostics diags;
    MathItem faster = dist;
    faster.expr = "integrate(20)";
    g.define(m, faster, diags);
    CHECK(g.version(test::keyOf(g, "Dist Count")) != before);

    // Without Corr Dist the item cannot compile.
    MathModule alone = test::module("user", {expr("Dist Count", "integrate_dist(1)")});
    g.setModules(std::span<const MathModule>(&alone, 1), nullptr, nullptr);
    CHECK(test::hasCode(g.status(test::keyOf(g, "Dist Count")).diags, "E-NO-DISTANCE"));
}
