// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/config.hpp>
#include <esses/base/version.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("version: build identification is stamped", "[unit][version]") {
    const esses::VersionInfo& v = esses::versionInfo();
    CHECK(v.major == 0);
    CHECK(v.minor == 1);
    CHECK_FALSE(v.string.empty());
    CHECK(v.string.substr(0, 3) == "0.1");
    CHECK_FALSE(v.gitSha.empty());
    CHECK(v.buildDate.size() == 10u); // YYYY-MM-DD
    CHECK(v.appName == "Esses");
    CHECK(esses::versionString() == v.string);
    CHECK(esses::versionBanner().rfind("Esses ", 0) == 0);
}

TEST_CASE("version: esses::base is an alias of esses", "[unit][version]") {
    CHECK(&esses::base::versionInfo() == &esses::versionInfo());
}
