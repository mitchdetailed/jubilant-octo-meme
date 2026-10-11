// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared helpers for the esses_units tests.
#pragma once

#include <esses/base/env.hpp>
#include <esses/base/utf8.hpp>
#include <esses/units/unit_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace esses::units::test {

inline std::filesystem::path repoRoot() {
    return (pathFromUtf8(ESSES_TEST_SOURCE_DIR) / ".." / "..").lexically_normal();
}

inline std::filesystem::path shippedTablePath() {
    return repoRoot() / "data" / "units.json";
}

inline std::filesystem::path schemaPath() {
    return repoRoot() / "schemas" / "units-1.schema.json";
}

inline std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The shipped table, loaded once per test executable.
inline const UnitRegistry& shipped() {
    static const UnitRegistry registry = [] {
        auto r = UnitRegistry::loadFiles(shippedTablePath());
        if (!r) {
            FAIL("data/units.json does not load: " << r.error().toString());
        }
        return std::move(r).value();
    }();
    return registry;
}

// Id of a spelling that must resolve.
inline UnitId id(const UnitRegistry& reg, std::string_view spelling) {
    const auto found = reg.find(spelling);
    INFO("spelling '" << spelling << "'");
    REQUIRE(found.has_value());
    return *found;
}

// Property-test seed (08 section 6.2): ESSES_TEST_SEED when set, else a fixed default.
inline std::uint64_t testSeed() {
    if (const auto v = getEnv(envName("TEST_SEED")); v && !v->empty()) {
        return std::stoull(*v, nullptr, 0);
    }
    return 0x5EED'0000'2026'1010ull;
}

// The smallest valid shipped table: the unitless quantity only. User documents in the tests extend it.
inline constexpr std::string_view kMinimalTable = R"({
  "$schema": "urn:esses:units-1", "schemaVersion": 1,
  "quantities": [ { "name": "Unitless", "family": "Unitless", "dim": [0,0,0,0,0,0,0], "base": "",
                    "units": [ { "symbol": "" } ] } ] })";

} // namespace esses::units::test
