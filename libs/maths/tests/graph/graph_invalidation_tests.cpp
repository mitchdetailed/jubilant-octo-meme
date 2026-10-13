// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M2-04 (07 section 5.2; 03 section 7.2, D-022, D-140): editing one item recomputes only its cone. A
// 50-item module with one edit re-evaluates <= 3 nodes, asserted by the node versions and by the graph's
// evaluation counter. Also: what invalidates nothing (a sidecar commit, an annotation, an unrelated detail),
// SelectionDependent keys (ungated stat_* re-key on a selection change; a column per selection is kept, so
// switching back is free), Recalculate (invalidate), compat switches and module enable.

#include "graph_test_support.hpp"

#include <esses/maths/math_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::maths;
using test::expr;

namespace {

// I0..I49: most read the logged channel; I10 -> I11 -> I12 is a three-node cone; I20 reads the constant C.
MathModule fiftyItems() {
    std::vector<MathItem> items;
    for (int k = 0; k < 50; ++k) {
        const std::string name = "I" + std::to_string(k);
        if (k == 10) {
            items.push_back(expr(name, "'rpm' + 10"));
        } else if (k == 11) {
            items.push_back(expr(name, "'I10' * 2"));
        } else if (k == 12) {
            items.push_back(expr(name, "'I11' + 1"));
        } else if (k == 20) {
            items.push_back(expr(name, "'rpm' * 'C'"));
        } else {
            items.push_back(expr(name, "'rpm' * " + std::to_string(k + 1)));
        }
    }
    MathModule m = test::module("user", std::move(items));
    m.constants.push_back(MathConstant{"C", UnitValue{3.0, units::kNoUnit}, true, ""});
    return m;
}

std::vector<Version> versionsOf(const MathGraph& g) {
    std::vector<Version> out;
    for (const ChannelKey& k : g.outputs()) {
        out.push_back(g.version(k));
    }
    return out;
}

void requireAll(MathGraph& g) {
    std::vector<Future<ColumnHandle>> futures;
    for (const ChannelKey& k : g.outputs()) {
        futures.push_back(g.require(k, SampleRange::all(), Priority::Visible, {}));
    }
    for (const Future<ColumnHandle>& f : futures) {
        REQUIRE(f.get());
    }
    static_cast<void>(g.applyReadyCompletions());
}

std::size_t changedCount(const std::vector<Version>& a, const std::vector<Version>& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        n += a[i] != b[i] ? 1u : 0u;
    }
    return n;
}

} // namespace

TEST_CASE("maths invalidation: one edit in a 50-item module re-evaluates only its cone (AC-M2-04)",
          "[unit][graph][invalidation]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    MathModule m = fiftyItems();
    REQUIRE(g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr).size() == 50);
    REQUIRE(g.outputs().size() == 50);
    requireAll(g);
    CHECK(g.evaluationCount() == 50);
    for (const ChannelKey& k : g.outputs()) {
        CHECK(g.status(k).state == MathState::Ok);
    }
    const std::vector<Version> before = versionsOf(g);

    SECTION("an expression edit") {
        Diagnostics d;
        const NodeId id = g.define(m, expr("I10", "'rpm' + 11"), d);
        CHECK_FALSE(hasError(d));
        CHECK(id.key == test::keyOf(g, "I10"));
        const std::vector<Version> after = versionsOf(g);
        CHECK(changedCount(before, after) == 3); // I10, I11, I12
        CHECK(after[10] != before[10]);
        CHECK(after[11] != before[11]);
        CHECK(after[12] != before[12]);
        CHECK(g.status(test::keyOf(g, "I13")).state == MathState::Ok);
        requireAll(g);
        CHECK(g.evaluationCount() - 50 <= 3);
        CHECK(g.evaluationCount() == 53);
        CHECK(test::values(g, "I12")[0] == test::values(g, "I11")[0] + 1.0);
    }
    SECTION("a constant edit re-versions only its readers") {
        m.constants[0].value.v = 4.0;
        const std::vector<ChannelKey> changed =
            g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr);
        CHECK(changed == std::vector<ChannelKey>{test::keyOf(g, "I20")});
        requireAll(g);
        CHECK(g.evaluationCount() == 51);
    }
    SECTION("a cosmetic edit (comment, display, spacing) re-evaluates nothing") {
        MathItem same = expr("I30", "'rpm'   *   31   // the same numbers");
        same.comment = "edited";
        same.display.dps = 4;
        Diagnostics d;
        g.define(m, same, d);
        CHECK(changedCount(before, versionsOf(g)) == 0);
        requireAll(g);
        CHECK(g.evaluationCount() == 50);
        CHECK(g.descriptor(test::keyOf(g, "I30"))->dps == 4);
    }
    SECTION("a sidecar commit, an annotation or an unrelated detail invalidates nothing (D-140)") {
        auto next = std::make_shared<model::DataSource>(*src);
        next->version += 3;
        next->details.push_back(model::Detail{"Driver", std::string("Someone"), model::Provenance::User, {}});
        CHECK(g.setSource(next).empty());
        requireAll(g);
        CHECK(g.evaluationCount() == 50);
    }
    SECTION("Recalculate evaluates the module again without changing a version") {
        const std::vector<ChannelKey> keys = g.invalidate("user");
        CHECK(keys.size() == 50);
        CHECK(changedCount(before, versionsOf(g)) == 0);
        CHECK(g.status(test::keyOf(g, "I0")).state == MathState::Pending);
        requireAll(g);
        CHECK(g.evaluationCount() == 100);
        CHECK(g.invalidate("nobody").empty());
    }
    SECTION("the power switch re-versions only what it parses differently; a function alias every item") {
        // 03 section 2.3: unaryMinusAbovePower changes only an expression with a sign and '^' (none here).
        MathsCompat compat;
        compat.unaryMinusAbovePower = true;
        CHECK(g.setCompat(compat).empty());
        CHECK(versionsOf(g) == before);
        compat.functionAliases["running_lo"] = "running_min";
        CHECK(g.setCompat(compat).size() == 50);
        CHECK(g.setCompat(compat).empty());
    }
    SECTION("disabling the module disables every item; enabling restores the versions") {
        CHECK(g.setModuleEnabled("user", false).size() == 50);
        CHECK(g.status(test::keyOf(g, "I0")).state == MathState::Disabled);
        CHECK(g.setModuleEnabled("user", true).size() == 50);
        CHECK(versionsOf(g) == before);
    }
}

TEST_CASE("maths invalidation: SelectionDependent nodes re-key on a selection change",
          "[unit][graph][invalidation]") {
    test::Runtime rt;
    auto src = test::graphSource(rt.cache);
    MathGraph g = test::makeGraph(rt, src);
    MathModule m = test::module(
        "user", {expr("mean", "stat_mean('rpm')"), expr("mean2", "'mean' * 2"), expr("plain", "'rpm' * 2")});
    g.setModules(std::span<const MathModule>(&m, 1), nullptr, nullptr);
    const std::vector<MathNodeInfo> nodes = g.nodes();
    REQUIRE(nodes.size() == 3);
    CHECK(nodes[0].selectionDependent);
    CHECK(nodes[1].selectionDependent); // inherited from its upstream
    CHECK_FALSE(nodes[2].selectionDependent);

    // The whole layer, then the first 7.5 s (Main in this source), then back.
    const std::vector<double> whole = test::values(g, "mean2");
    REQUIRE_FALSE(whole.empty());
    static_cast<void>(test::values(g, "plain"));
    CHECK(g.evaluationCount() == 3);
    const TaskKey wholeKey = g.taskKey(test::keyOf(g, "mean"));

    MathSelection half;
    half.main.ref.src = src->id;
    half.main.ref.layer = "Normal";
    half.main.start = 0;
    half.main.end = 7'500'000; // one and a half periods of the saw tooth
    half.key = hash128("selection: first 7.5 s");
    const std::vector<ChannelKey> rekeyed = g.setSelection(half);
    CHECK(rekeyed == std::vector<ChannelKey>{test::keyOf(g, "mean"), test::keyOf(g, "mean2")});
    CHECK(g.status(test::keyOf(g, "mean")).state == MathState::Pending);
    CHECK(g.status(test::keyOf(g, "plain")).state == MathState::Ok);
    CHECK(g.taskKey(test::keyOf(g, "mean")).text != wholeKey.text);
    const std::vector<double> firstHalf = test::values(g, "mean2");
    CHECK(g.evaluationCount() == 5);
    CHECK(firstHalf[0] != whole[0]); // the mean over the window differs from the whole layer's

    // Back to the whole layer: the cached columns come back without an evaluation.
    MathSelection none;
    CHECK(g.setSelection(none).size() == 2);
    CHECK(g.status(test::keyOf(g, "mean2")).state == MathState::Ok);
    CHECK(test::values(g, "mean2") == whole);
    CHECK(g.evaluationCount() == 5);
    CHECK(g.taskKey(test::keyOf(g, "mean")).text == wholeKey.text);

    // A selection with the same key changes nothing.
    CHECK(g.setSelection(none).empty());
}
