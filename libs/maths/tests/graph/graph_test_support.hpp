// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Helpers of the MathGraph M2 tests (tests/graph): maths-1 items and modules built in code, a source with a
// few logged channels, and require-and-read helpers that wait on the calling thread (the test thread is not a
// pool worker) and apply the completions as the owning thread would.
#pragma once

#include "../maths_test_support.hpp"

#include <esses/maths/math_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::maths::test {

inline MathItem expr(std::string name, std::string text) {
    MathItem item;
    item.kind = ItemKind::Expression;
    item.name = std::move(name);
    item.expr = std::move(text);
    return item;
}

inline MathModule module(std::string id, std::vector<MathItem> items,
                         MathModule::Origin origin = MathModule::Origin::Global) {
    MathModule m;
    m.id = std::move(id);
    m.items = std::move(items);
    m.origin = origin;
    return m;
}

// "rpm" (unitless, 100 Hz) = 1000 + 10 (i mod 500) + sin i, "Temp" (Temperature, K, 10 Hz, dps 1) = 300 + i,
// 20 s.
constexpr std::size_t kRpmSamples = 2000;

inline std::vector<float> rpmValues() {
    std::vector<float> v(kRpmSamples);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = static_cast<float>(1000.0 + 10.0 * static_cast<double>(i % 500) +
                                  std::sin(static_cast<double>(i)));
    }
    return v;
}

inline std::shared_ptr<model::DataSource> graphSource(store::ChunkCache& cache,
                                                      std::string_view tag = "graph-test-source") {
    std::vector<model::Channel> channels;
    channels.push_back(uniformF32(cache, "rpm", units::kNoUnit, Rate::fromHz(100), rpmValues()));
    std::vector<float> temp(200);
    for (std::size_t i = 0; i < temp.size(); ++i) {
        temp[i] = static_cast<float>(300.0 + static_cast<double>(i));
    }
    model::Channel t = uniformF32(cache, "Temp", unitOf("K"), Rate::fromHz(10), temp);
    t.desc.dps = 1;
    channels.push_back(std::move(t));
    return makeSource(std::move(channels), static_cast<Micros>(20'000'000), tag);
}

inline MathGraph makeGraph(Runtime& rt, std::shared_ptr<const model::DataSource> src) {
    return MathGraph(std::move(src), "", shippedUnits(), {}, &rt.pool, &rt.cache);
}

inline ChannelKey keyOf(const MathGraph& g, std::string_view name) {
    return ChannelKey::make(g.source()->id, g.layer(), name);
}

// require + wait + apply; the result of the head of `name`.
inline Result<ColumnHandle> evaluate(MathGraph& g, std::string_view name, Priority prio = Priority::Visible) {
    Future<ColumnHandle> f = g.require(keyOf(g, name), SampleRange::all(), prio, {});
    Result<ColumnHandle> r = f.get();
    static_cast<void>(g.applyReadyCompletions());
    return r;
}

// The evaluated values of the head of `name` (REQUIRE success).
inline std::vector<double> values(MathGraph& g, std::string_view name) {
    Result<ColumnHandle> r = evaluate(g, name);
    if (!r) {
        FAIL_CHECK(std::string(name) + ": " + r.error().toString());
        return {};
    }
    Result<analysis::UniformSignal> s = readWholeSignal(r->series, {});
    REQUIRE(s);
    return s->values;
}

inline const MathNodeInfo* nodeInfo(const std::vector<MathNodeInfo>& nodes, std::string_view moduleId,
                                    std::string_view name, std::size_t occurrence = 0) {
    for (const MathNodeInfo& n : nodes) {
        if (n.moduleId == moduleId && n.name == name) {
            if (occurrence == 0) {
                return &n;
            }
            --occurrence;
        }
    }
    return nullptr;
}

inline bool hasCode(const Diagnostics& diags, std::string_view code) {
    for (const Diagnostic& d : diags) {
        if (d.code == code) {
            return true;
        }
    }
    return false;
}

} // namespace esses::maths::test
