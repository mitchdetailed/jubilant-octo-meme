// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Helpers of the M3 geometry tests: the DrawList JSON goldens of
// tests/golden/drawlist/<type>/<case>@<dpr>x.json (platform independent, checked on every OS;
// ESSES_UPDATE_GOLDENS=1 rewrites them, a mismatch writes <type>_<case>@<dpr>x.actual.json next to the test
// binary), resident columns for the summary path, and whether an esses_analysis function the geometry depends
// on is still an M3 stub (its tests then skip).
#pragma once

#include "td_golden_cases.hpp"

#include <esses/analysis/band_axis.hpp>
#include <esses/analysis/histogram_binner.hpp>
#include <esses/analysis/scatter_binner.hpp>
#include <esses/base/env.hpp>
#include <esses/plotcore/plotcore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace esses::plot::test {

inline std::filesystem::path drawListGoldenDir(const std::string& type) {
    return std::filesystem::path(ESSES_TEST_REPO_ROOT) / "tests" / "golden" / "drawlist" / type;
}

inline std::string readGoldenText(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string out;
    for (const char c : ss.str()) {
        if (c != '\r') {
            out += c;
        }
    }
    return out;
}

inline void writeGoldenText(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// The golden text of the three layers of one case.
inline std::string layersJson(const std::string& name, float dpr, const DrawList& staticList,
                              const DrawList& dynamicList, const DrawList& chromeList) {
    std::string s = "{\"case\":\"" + name + "\",\"dpr\":" + std::to_string(static_cast<int>(dpr)) + ",\n";
    s += "\"static\":" + staticList.toJson();
    s += ",\"dynamic\":" + dynamicList.toJson();
    s += ",\"chrome\":" + chromeList.toJson();
    s += "}\n";
    return s;
}

// Compares (or with ESSES_UPDATE_GOLDENS=1 rewrites) tests/golden/drawlist/<type>/<name>@<dpr>x.json.
inline void checkGolden(const std::string& type, const std::string& name, float dpr,
                        const std::string& actual) {
    const std::string file = name + "@" + std::to_string(static_cast<int>(dpr)) + "x.json";
    const std::filesystem::path path = drawListGoldenDir(type) / file;
    if (getEnv(envName("UPDATE_GOLDENS")).value_or("") == "1") {
        writeGoldenText(path, actual);
        return;
    }
    const std::string expected = readGoldenText(path);
    if (expected != actual) {
        const std::filesystem::path out =
            std::filesystem::path(ESSES_TEST_BINARY_DIR) /
            (type + "_" + name + "@" + std::to_string(static_cast<int>(dpr)) + "x.actual.json");
        writeGoldenText(out, actual);
        FAIL_CHECK("DrawList golden " << type << "/" << file << " differs; actual written to " << out.string()
                                      << (expected.empty() ? " (golden missing)" : ""));
    }
}

// Seals every chunk of a series' values into the cache (and so fills its SummaryPyramid); the summary path
// then refines its edges from resident chunks, deterministically.
inline void warm(const store::Series& series) {
    const store::Column& c = store::seriesValues(series);
    if (c.size() > 0) {
        REQUIRE(c.require(0, c.size() - 1, Priority::Visible, CancelToken{}).ok());
    }
}

inline void warmAll(const SyntheticSource& src) {
    for (const auto& [name, ch] : src.channels) {
        warm(ch.series);
    }
}

// Whether the esses_analysis functions the scatter and histogram builders call are still M3 stubs.
inline bool bandAxisIsStub() {
    const Result<analysis::BandAxis> r = analysis::BandAxis::fromJson(JsonDoc::parse("{}").value());
    return !r.ok() && r.error().code == ErrorCode::Unsupported;
}

inline bool scatterBinnerIsStub() {
    const Result<analysis::IntervalAverage> r = analysis::ScatterBinner::intervalAverage({}, {}, 1.0);
    return !r.ok() && r.error().code == ErrorCode::Unsupported;
}

inline bool histogramBinnerIsStub() {
    analysis::HistogramRequest request;
    request.bins = {analysis::Band{0.0, 1.0, 0, false, false}};
    const Result<analysis::HistogramResult> r =
        analysis::HistogramBinner::bin(analysis::HistogramInput{}, request, Priority::Visible, CancelToken{});
    return !r.ok() && r.error().code == ErrorCode::Unsupported;
}

} // namespace esses::plot::test
