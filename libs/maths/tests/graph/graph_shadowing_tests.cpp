// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M2-03 (07 section 5.2; 03 sections 3.3, 4.3, 7.1): shadowing chains of MathGraph. Local (sidecar)
// maths beats global; modules apply in project order; the first definition in a module wins (W-DUP-IGNORED);
// a filter item whose output name equals its input shadows the channel for consumers and reads the
// unshadowed input; cycles are reported on every node of the cycle with the path; the Scope order of 03
// section 3.3 (mappings, aliases with W-ALIAS-FALLBACK, chain head, logged channel, detail, constant with
// W-CONST-AMBIGUOUS and W-CONST-SHADOWED) and module conditions.

#include "graph_test_support.hpp"

#include <esses/maths/math_graph.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::maths;
using test::expr;
using test::nodeInfo;

namespace {

double f32(double v) {
    return static_cast<double>(static_cast<float>(v));
}

std::vector<double> loggedRpm() {
    std::vector<double> out;
    for (const float v : test::rpmValues()) {
        out.push_back(static_cast<double>(v));
    }
    return out;
}

} // namespace

TEST_CASE("math graph shadowing: logged, then local, then global modules in project order",
          "[unit][graph][shadowing]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    const MathModule local = test::module("local", {expr("rpm", "'rpm' + 1")}, MathModule::Origin::Local);
    std::vector<MathModule> globals = {
        test::module("g1", {expr("rpm", "'rpm' * 2"), expr("rpm10", "'rpm' * 10")}),
        test::module("g2", {expr("rpm", "'rpm' - 3")})};
    const std::vector<ChannelKey> changed = g.setModules(globals, &local, nullptr);
    CHECK(changed.size() == 2); // rpm and rpm10
    CHECK(g.outputs() == std::vector<ChannelKey>{test::keyOf(g, "rpm"), test::keyOf(g, "rpm10")});

    const std::vector<MathNodeInfo> nodes = g.nodes();
    REQUIRE(nodes.size() == 4);
    const MathNodeInfo* l = nodeInfo(nodes, "local", "rpm");
    const MathNodeInfo* g1 = nodeInfo(nodes, "g1", "rpm");
    const MathNodeInfo* g2 = nodeInfo(nodes, "g2", "rpm");
    const MathNodeInfo* r10 = nodeInfo(nodes, "g1", "rpm10");
    REQUIRE((l != nullptr && g1 != nullptr && g2 != nullptr && r10 != nullptr));
    CHECK(l->id.chainIndex == 0);
    CHECK(g1->id.chainIndex == 1);
    CHECK(g2->id.chainIndex == 2);
    CHECK_FALSE(l->head);
    CHECK_FALSE(g1->head);
    CHECK(g2->head);
    CHECK(l->selfMaths);
    CHECK(g1->selfMaths);
    CHECK_FALSE(r10->selfMaths);
    CHECK(l->origin == MathModule::Origin::Local);
    // Each self-maths element reads the element before it; a consumer reads the head.
    REQUIRE(l->inputs.size() == 1);
    CHECK(l->inputs[0].kind == MathInput::Kind::Logged);
    REQUIRE(g1->inputs.size() == 1);
    CHECK(g1->inputs[0].kind == MathInput::Kind::Maths);
    CHECK(g1->inputs[0].node == l->id);
    CHECK(g2->inputs[0].node == g1->id);
    REQUIRE(r10->inputs.size() == 1);
    CHECK(r10->inputs[0].node == g2->id);
    CHECK(g.consumersOf(test::keyOf(g, "rpm")) == std::vector<ChannelKey>{test::keyOf(g, "rpm10")});
    CHECK(g.status(l->id).state == MathState::Pending);

    // Values: local `rpm + 1`, then g1 `* 2`, then g2 `- 3` (each stored as F32), and rpm10 from the head.
    const std::vector<double> x = loggedRpm();
    const std::vector<double> head = test::values(g, "rpm");
    const std::vector<double> ten = test::values(g, "rpm10");
    REQUIRE(head.size() == x.size());
    REQUIRE(ten.size() == x.size());
    for (std::size_t i = 0; i < x.size(); i += 37) {
        const double expected = f32(f32(f32(x[i] + 1.0) * 2.0) - 3.0);
        CHECK(head[i] == expected);
        CHECK(ten[i] == f32(expected * 10.0));
    }
    CHECK(g.status(test::keyOf(g, "rpm")).state == MathState::Ok);
    CHECK(g.evaluationCount() == 4);

    // The trace of the head: its input, the element before it.
    const model::ResolutionTrace t = g.trace(test::keyOf(g, "rpm"));
    CHECK(t.requested == "rpm");
    CHECK(t.result == model::channelIdMathsModule(src->id, "Normal", "rpm", "g1"));
    CHECK(g.descriptor(test::keyOf(g, "rpm"))->id ==
          model::channelIdMathsModule(src->id, "Normal", "rpm", "g2"));

    SECTION("swapping the project order swaps the chain") {
        std::swap(globals[0], globals[1]);
        const std::vector<ChannelKey> again = g.setModules(globals, &local, nullptr);
        CHECK(std::find(again.begin(), again.end(), test::keyOf(g, "rpm")) != again.end());
        CHECK(std::find(again.begin(), again.end(), test::keyOf(g, "rpm10")) != again.end());
        const std::vector<double> swapped = test::values(g, "rpm");
        for (std::size_t i = 0; i < x.size(); i += 41) {
            CHECK(swapped[i] == f32(f32(f32(x[i] + 1.0) - 3.0) * 2.0));
        }
        // The local element keeps its version and its column (nothing it reads changed).
        CHECK(g.status(l->id).state == MathState::Ok);
    }
    SECTION("without local maths the first global element reads the logged channel") {
        const std::vector<ChannelKey> again = g.setModules(globals, nullptr, nullptr);
        CHECK(again.size() == 2);
        const std::vector<double> head2 = test::values(g, "rpm");
        for (std::size_t i = 0; i < x.size(); i += 41) {
            CHECK(head2[i] == f32(f32(x[i] * 2.0) - 3.0));
        }
    }
    SECTION("a transient filter applies last") {
        const MathModule transient = test::module(std::string(MathModule::kTransientId),
                                                  {expr("rpm", "'rpm' / 2")}, MathModule::Origin::Transient);
        g.setModules(globals, &local, &transient);
        const std::vector<double> head3 = test::values(g, "rpm");
        for (std::size_t i = 0; i < x.size(); i += 41) {
            CHECK(head3[i] == f32(f32(f32(f32(x[i] + 1.0) * 2.0) - 3.0) / 2.0));
        }
    }
    SECTION("a disabled element is skipped: consumers and the next element read the one before it") {
        Diagnostics d;
        MathItem off = expr("rpm", "'rpm' * 2");
        off.enabled = false;
        const NodeId id = g.define(globals[0], off, d);
        CHECK(id.chainIndex == 1);
        CHECK(g.status(id).state == MathState::Disabled);
        const std::vector<double> head4 = test::values(g, "rpm");
        for (std::size_t i = 0; i < x.size(); i += 41) {
            CHECK(head4[i] == f32(f32(x[i] + 1.0) - 3.0));
        }
        g.setEnabled(id, true);
        CHECK(g.status(id).state == MathState::Pending);
        CHECK(test::values(g, "rpm") == head);
    }
}

TEST_CASE("math graph shadowing: the first definition in a module wins", "[unit][graph][shadowing]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    MathModule m =
        test::module("user", {expr("Y", "'rpm' * 0 + 5"), expr("y", "'rpm' * 0 + 7"), expr("Z", "'Y' + 1")});
    g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr);
    const std::vector<MathNodeInfo> nodes = g.nodes();
    REQUIRE(nodes.size() == 3);
    const MathNodeInfo* second = nodeInfo(nodes, "user", "y");
    REQUIRE(second != nullptr);
    CHECK(second->id.chainIndex == NodeId::kIgnored);
    CHECK(second->status.state == MathState::Warning);
    REQUIRE_FALSE(second->status.diags.empty());
    CHECK(second->status.diags[0].code == "W-DUP-IGNORED");
    CHECK(g.status(second->id).state == MathState::Warning);
    CHECK(test::values(g, "Y")[10] == 5.0);
    CHECK(test::values(g, "Z")[10] == 6.0);

    // define() reports the duplicate as ignored.
    Diagnostics d;
    const NodeId again = g.define(m, expr("y", "'rpm' * 0 + 9"), d);
    CHECK(again.chainIndex == 0); // define replaces the first item of that name
    CHECK(test::values(g, "Y")[10] == 9.0);

    // Disabling the first definition lets the later one take over.
    MathModule twice = test::module("user", {expr("Y", "'rpm' * 0 + 5"), expr("Y", "'rpm' * 0 + 7")});
    g.setModules(std::span<const MathModule>(&twice, 1), nullptr, nullptr);
    CHECK(test::values(g, "Y")[10] == 5.0);
    const std::vector<MathNodeInfo> both = g.nodes();
    const MathNodeInfo* first = nodeInfo(both, "user", "Y");
    REQUIRE(first != nullptr);
    g.setEnabled(first->id, false);
    CHECK(test::values(g, "Y")[10] == 7.0);
    CHECK(g.nodes()[1].id.chainIndex == 1); // no longer ignored
}

TEST_CASE(
    "math graph shadowing: a filter item shadows its input for consumers and reads the unshadowed input",
    "[unit][graph][shadowing]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    MathItem filter;
    filter.kind = ItemKind::Filter;
    filter.name = "rpm";
    filter.window.points = 5;
    MathModule m = test::module("user", {filter, expr("rpm2", "'rpm' * 2")});
    g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr);
    const std::vector<MathNodeInfo> nodes = g.nodes();
    const MathNodeInfo* f = nodeInfo(nodes, "user", "rpm");
    const MathNodeInfo* consumer = nodeInfo(nodes, "user", "rpm2");
    REQUIRE((f != nullptr && consumer != nullptr));
    CHECK(f->kind == ItemKind::Filter);
    CHECK(f->head);
    CHECK(f->selfMaths);
    REQUIRE(f->inputs.size() == 1);
    CHECK(f->inputs[0].kind == MathInput::Kind::Logged); // the unshadowed input
    REQUIRE(consumer->inputs.size() == 1);
    CHECK(consumer->inputs[0].kind == MathInput::Kind::Maths);
    CHECK(consumer->inputs[0].node == f->id);
    CHECK(g.status(f->id).resolvedInput == test::keyOf(g, "rpm"));

    // The same numbers as smooth() of the logged channel, evaluated on its own.
    MathGraph ref = test::makeGraph(rt, src);
    MathModule r =
        test::module("ref", {expr("smoothed", "smooth('rpm', 5)"), expr("ref2", "smooth('rpm', 5) * 2")});
    ref.setModules(std::span<const MathModule>(&r, 1), nullptr, nullptr);
    const std::vector<double> filtered = test::values(g, "rpm");
    const std::vector<double> doubled = test::values(g, "rpm2");
    const std::vector<double> smoothed = test::values(ref, "smoothed");
    const std::vector<double> ref2 = test::values(ref, "ref2");
    REQUIRE(filtered.size() == smoothed.size());
    REQUIRE(doubled.size() == ref2.size());
    CHECK(filtered == smoothed);
    for (std::size_t i = 0; i < doubled.size(); ++i) {
        CHECK(std::abs(doubled[i] - ref2[i]) <= 1e-6 * std::abs(ref2[i]) + 1e-3);
    }
    // Self-maths keeps the shadowed channel's display settings (03 section 8).
    const std::optional<model::ChannelDescriptor> d = g.descriptor(test::keyOf(g, "rpm"));
    REQUIRE(d);
    CHECK(d->source == model::ChannelSource::Maths);
    CHECK(d->dps == src->layers[0].find("rpm")->desc.dps);
    CHECK(d->id == model::channelIdMathsModule(src->id, "Normal", "rpm", "user"));

    SECTION("a filter after a local self-maths item reads that item, not the logged channel") {
        const MathModule local = test::module("local", {expr("rpm", "'rpm' + 1")}, MathModule::Origin::Local);
        g.setModules(std::span<const MathModule>(&m, 1), &local, nullptr);
        const std::vector<MathNodeInfo> again = g.nodes();
        const MathNodeInfo* lf = nodeInfo(again, "user", "rpm");
        const MathNodeInfo* l = nodeInfo(again, "local", "rpm");
        REQUIRE((lf != nullptr && l != nullptr));
        CHECK(lf->inputs[0].kind == MathInput::Kind::Maths);
        CHECK(lf->inputs[0].node == l->id);
        const std::vector<double> plusOne = test::values(g, "rpm");
        CHECK(std::abs(plusOne[100] - (filtered[100] + 1.0)) < 1e-3);
    }
}

TEST_CASE("math graph shadowing: cycles are reported on every node with the path",
          "[unit][graph][shadowing]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    MathModule m =
        test::module("user", {expr("A", "'B' + 1"), expr("B", "'C' + 1"), expr("C", "'A' + 1"),
                              expr("D", "'A' * 2"), expr("Z", "'Z' + 1"), expr("Ok", "'rpm' * 1")});
    g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr);
    const std::string arrow = "\xE2\x86\x92";
    const auto cycleOf = [&](const char* name) {
        const MathStatus s = g.status(test::keyOf(g, name));
        CHECK(s.state == MathState::Error);
        CHECK(test::hasCode(s.diags, "E-CYCLE"));
        return s.summary;
    };
    CHECK_THAT(cycleOf("A"), Catch::Matchers::ContainsSubstring("'A' " + arrow + " 'B' " + arrow + " 'C' " +
                                                                arrow + " 'A'"));
    CHECK_THAT(cycleOf("B"), Catch::Matchers::ContainsSubstring("'B' " + arrow + " 'C' " + arrow + " 'A' " +
                                                                arrow + " 'B'"));
    CHECK_THAT(cycleOf("C"), Catch::Matchers::ContainsSubstring("'C' " + arrow + " 'A' " + arrow + " 'B' " +
                                                                arrow + " 'C'"));
    Future<ColumnHandle> a = g.require(test::keyOf(g, "A"), SampleRange::all(), Priority::Visible, {});
    REQUIRE(a.ready());
    CHECK(a.get().error().code == ErrorCode::Cycle);

    // A consumer of the cycle is defined; its evaluation fails with the cycle's code.
    CHECK(g.status(test::keyOf(g, "D")).state == MathState::Pending);
    Result<ColumnHandle> d = test::evaluate(g, "D");
    REQUIRE_FALSE(d);
    const MathStatus ds = g.status(test::keyOf(g, "D"));
    CHECK(ds.state == MathState::Error);
    REQUIRE_FALSE(ds.diags.empty());
    CHECK(ds.diags[0].code == "E-CYCLE");
    CHECK_THAT(ds.summary, Catch::Matchers::StartsWith("input 'A': "));

    // Reading its own name with nothing before it is no cycle: the name does not resolve.
    const MathStatus z = g.status(test::keyOf(g, "Z"));
    CHECK(z.state == MathState::Error);
    CHECK(test::hasCode(z.diags, "E-UNKNOWN-CHANNEL"));
    CHECK(test::values(g, "Ok").size() == test::kRpmSamples);

    // Breaking the cycle clears the errors.
    Diagnostics diags;
    g.define(m, expr("C", "'rpm' * 0 + 1"), diags);
    CHECK_FALSE(hasError(diags));
    CHECK(test::values(g, "A")[0] == 3.0);
    CHECK(test::values(g, "D")[0] == 6.0);
}

TEST_CASE("math graph scope: conditions, constants and details", "[unit][graph][shadowing]") {
    test::Runtime rt;
    auto base = test::graphSource(rt.cache);
    auto src = std::make_shared<model::DataSource>(*base);
    src->details.push_back(model::Detail{"Vehicle Id", std::string("GT3-07"), model::Provenance::Logged, {}});
    src->details.push_back(
        model::Detail{"Wheelbase",
                      model::makeQuantity(270.0, test::unitOf("cm"), *test::shippedUnits()),
                      model::Provenance::Logged,
                      {}});
    MathGraph g = test::makeGraph(rt, src);

    MathModule chassis =
        test::module("chassis", {expr("a", "'rpm' * 'Gain'"), expr("wb", "'rpm' * 0 + 'Wheelbase'")});
    chassis.condition = "'Vehicle Id' == \"GT3-07\"";
    chassis.constants.push_back(MathConstant{"Gain", UnitValue{2.0, units::kNoUnit}, true, ""});
    MathModule other = test::module("other", {expr("o", "'rpm' * 3")});
    other.condition = "'Vehicle Id' == \"LMP-01\"";
    MathModule broken = test::module("broken", {expr("b", "'rpm' * 4")});
    broken.condition = "'rpm' > 3";
    MathModule k1 = test::module("k1", {});
    k1.constants.push_back(MathConstant{"K", UnitValue{1.0, units::kNoUnit}, true, ""});
    MathModule k2 = test::module("k2", {});
    k2.constants.push_back(MathConstant{"K", UnitValue{2.0, units::kNoUnit}, true, ""});
    MathModule use = test::module("use", {expr("k", "'rpm' * 0 + 'K'"), expr("s", "'rpm' + 0")});
    use.constants.push_back(MathConstant{"rpm", UnitValue{5.0, units::kNoUnit}, true, ""});
    const std::vector<MathModule> globals = {chassis, other, broken, k1, k2, use};
    g.setModules(globals, nullptr, nullptr);

    const std::vector<double> x = loggedRpm();
    CHECK(test::values(g, "a")[3] == f32(x[3] * 2.0));
    CHECK(std::abs(test::values(g, "wb")[3] - 2.7) < 1e-6); // the numeric detail, in its base unit (m)

    const MathStatus o = g.status(test::keyOf(g, "o"));
    CHECK(o.state == MathState::Disabled);
    CHECK(o.summary == "Disabled (condition)");
    CHECK(g.require(test::keyOf(g, "o"), SampleRange::all(), Priority::Visible, {}).get().error().code ==
          ErrorCode::Invalid);
    const MathStatus b = g.status(test::keyOf(g, "b"));
    CHECK(b.state == MathState::Disabled);
    CHECK(test::hasCode(b.diags, "E-CONDITION"));

    // Constants of other modules: the first in project order, W-CONST-AMBIGUOUS.
    CHECK(test::values(g, "k")[0] == 1.0);
    CHECK(g.status(test::keyOf(g, "k")).state == MathState::Warning);
    CHECK(test::hasCode(g.status(test::keyOf(g, "k")).diags, "W-CONST-AMBIGUOUS"));
    // A channel and a constant of one name: the channel, W-CONST-SHADOWED.
    CHECK(test::values(g, "s")[7] == f32(x[7]));
    CHECK(test::hasCode(g.status(test::keyOf(g, "s")).diags, "W-CONST-SHADOWED"));

    // A detail edit re-versions only the items that read it (03 section 7.2).
    const Version av = g.version(test::keyOf(g, "a"));
    const Version wv = g.version(test::keyOf(g, "wb"));
    auto edited = std::make_shared<model::DataSource>(*src);
    edited->version += 1;
    model::findDetail(edited->details, "Wheelbase")->value =
        model::makeQuantity(280.0, test::unitOf("cm"), *test::shippedUnits());
    const std::vector<ChannelKey> changed = g.setSource(edited);
    CHECK(changed == std::vector<ChannelKey>{test::keyOf(g, "wb")});
    CHECK(g.version(test::keyOf(g, "a")) == av);
    CHECK(g.version(test::keyOf(g, "wb")) != wv);
    CHECK(std::abs(test::values(g, "wb")[3] - 2.8) < 1e-6);

    // The condition follows the detail: another vehicle disables the chassis module.
    auto lmp = std::make_shared<model::DataSource>(*edited);
    std::get<std::string>(model::findDetail(lmp->details, "Vehicle Id")->value) = "LMP-01";
    g.setSource(lmp);
    CHECK(g.status(test::keyOf(g, "a")).state == MathState::Disabled);
    CHECK(g.status(test::keyOf(g, "o")).state == MathState::Pending);
}

TEST_CASE("math graph scope: mappings and aliases, with W-ALIAS-FALLBACK", "[unit][graph][shadowing]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    model::MappingTable mappings;
    mappings.rows.push_back(model::MappingRow{"RPM_RAW", "rpm", std::nullopt, false});
    model::AliasTable aliases;
    aliases.rows.push_back(
        model::AliasRow{"Engine Speed", {{"Missing Ch", true}, {"Err Item", true}, {"rpm", true}}, -1, {}});
    aliases.rows.push_back(model::AliasRow{"Fast", {{"rpm10", true}}, -1, {}});
    aliases.rows.push_back(model::AliasRow{"A1", {{"A2", true}}, -1, {}});
    aliases.rows.push_back(model::AliasRow{"A2", {{"A1", true}}, -1, {}});
    g.setResolverTables(mappings, aliases);
    MathModule m = test::module("user", {expr("Err Item", "'rpm' +"), expr("es", "'Engine Speed' * 1"),
                                         expr("raw", "'RPM_RAW' * 1"), expr("rpm10", "'rpm' * 10"),
                                         expr("fast", "'Fast' + 1"), expr("loop", "'A1' * 1")});
    g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr);
    const std::vector<double> x = loggedRpm();

    // The alias skips a missing candidate and a maths candidate in Error, then reads the logged channel.
    CHECK(g.status(test::keyOf(g, "Err Item")).state == MathState::Error);
    CHECK(test::values(g, "es")[5] == f32(x[5]));
    const MathStatus es = g.status(test::keyOf(g, "es"));
    CHECK(es.state == MathState::Warning);
    CHECK(test::hasCode(es.diags, "W-ALIAS-FALLBACK"));
    const std::vector<MathNodeInfo> nodes = g.nodes();
    const MathNodeInfo* esInfo = nodeInfo(nodes, "user", "es");
    REQUIRE(esInfo != nullptr);
    REQUIRE(esInfo->inputs.size() == 1);
    const model::ResolutionTrace& t = esInfo->inputs[0].trace;
    CHECK(esInfo->inputs[0].kind == MathInput::Kind::Logged);
    CHECK(t.resolved());
    const auto aliasSteps = std::count_if(t.steps.begin(), t.steps.end(), [](const model::ResolutionStep& s) {
        return s.kind == model::ResolutionStep::Kind::Alias;
    });
    CHECK(aliasSteps == 3);
    CHECK_THAT(t.summary(), Catch::Matchers::ContainsSubstring("alias"));

    // A mapping renames before the chain and the logged lookup.
    CHECK(test::values(g, "raw")[5] == f32(x[5]));
    const MathNodeInfo* rawInfo = nodeInfo(nodes, "user", "raw");
    REQUIRE(rawInfo != nullptr);
    CHECK(rawInfo->inputs[0].trace.steps.front().kind == model::ResolutionStep::Kind::Mapping);

    // An alias may target a maths channel.
    const MathNodeInfo* fastInfo = nodeInfo(nodes, "user", "fast");
    REQUIRE(fastInfo != nullptr);
    CHECK(fastInfo->inputs[0].kind == MathInput::Kind::Maths);
    CHECK(test::values(g, "fast")[5] == f32(f32(x[5] * 10.0) + 1.0));

    // An alias cycle stops the resolution with its path.
    const MathStatus loop = g.status(test::keyOf(g, "loop"));
    CHECK(loop.state == MathState::Error);
    CHECK(test::hasCode(loop.diags, "E-UNKNOWN-CHANNEL"));
    CHECK_THAT(loop.summary, Catch::Matchers::ContainsSubstring("alias cycle"));

    // Fixing the item in Error makes the alias read it (its version changes with the target).
    const Version before = g.version(test::keyOf(g, "es"));
    Diagnostics d;
    g.define(m, expr("Err Item", "'rpm' + 1"), d);
    CHECK(g.version(test::keyOf(g, "es")) != before);
    CHECK(test::values(g, "es")[5] == f32(f32(x[5] + 1.0)));
}
