// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// 01 section 11 item 13: Paths resolves every location under a non-ASCII override directory.
#include "test_support.hpp"

#include <esses/base/env.hpp>
#include <esses/base/paths.hpp>
#include <esses/base/utf8.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

namespace fs = std::filesystem;
using esses::Paths;

namespace {

// Saves and restores the four override variables.
class EnvGuard {
public:
    EnvGuard() {
        for (const char* suffix : kSuffixes) {
            saved_.push_back(esses::getEnv(esses::envName(suffix)));
        }
    }
    ~EnvGuard() {
        for (std::size_t i = 0; i < std::size(kSuffixes); ++i) {
            esses::setEnv(esses::envName(kSuffixes[i]), saved_[i]);
        }
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;
    static void clear() {
        for (const char* suffix : kSuffixes) {
            esses::setEnv(esses::envName(suffix), std::nullopt);
        }
    }

private:
    static constexpr const char* kSuffixes[] = {"CONFIG_DIR", "CACHE_DIR", "DATA_DIR", "LOG_DIR"};
    std::vector<std::optional<std::string>> saved_;
};

} // namespace

TEST_CASE("paths: every location resolves under a non-ASCII override directory", "[unit][paths]") {
    EnvGuard guard;
    esses::test::TempDir tmp("paths");
    // "M<u-umlaut>ller-<CJK>" in UTF-8.
    const fs::path root = tmp.path() / esses::pathFromUtf8("M\xC3\xBCller-\xE6\x97\xA5\xE6\x9C\xAC");
    fs::create_directories(root);
    REQUIRE(esses::setEnv(esses::envName("CONFIG_DIR"), esses::pathToUtf8(root / "config")));
    REQUIRE(esses::setEnv(esses::envName("CACHE_DIR"), esses::pathToUtf8(root / "cache")));
    REQUIRE(esses::setEnv(esses::envName("DATA_DIR"), esses::pathToUtf8(root / "data")));
    REQUIRE(esses::setEnv(esses::envName("LOG_DIR"), esses::pathToUtf8(root / "logs")));
    const esses::Result<Paths> r = Paths::resolve();
    REQUIRE(r.ok());
    const Paths& p = r.value();
    CHECK(p.configDir() == root / "config");
    CHECK(p.cacheDir() == root / "cache");
    CHECK(p.dataDir() == root / "data");
    CHECK(p.logDir() == root / "logs");
    CHECK(esses::pathToUtf8(p.configDir().parent_path().filename()) ==
          "M\xC3\xBCller-\xE6\x97\xA5\xE6\x9C\xAC");
    // Writing through the resolved path works (no code-page mangling).
    fs::create_directories(p.configDir());
    std::ofstream(p.configDir() / "settings.json") << "{}";
    CHECK(fs::exists(root / "config" / "settings.json"));
    REQUIRE_FALSE(p.pluginDirs().empty());
    CHECK(p.pluginDirs().back() == root / "config" / "plugins");
    CHECK_FALSE(p.portable());
}

TEST_CASE("paths: explicit overrides beat the environment", "[unit][paths]") {
    EnvGuard guard;
    REQUIRE(esses::setEnv(esses::envName("CONFIG_DIR"), std::string("/from/env")));
    esses::PathOverrides o;
    o.configDir = fs::path("flag-config");
    o.dataDir = fs::path("flag-data");
    const esses::Result<Paths> r = Paths::resolve(o);
    REQUIRE(r.ok());
    CHECK(r.value().configDir() == fs::path("flag-config"));
    CHECK(r.value().dataDir() == fs::path("flag-data"));
}

TEST_CASE("paths: portable mode keeps config, cache and logs next to the executable", "[unit][paths]") {
    EnvGuard guard;
    EnvGuard::clear();
    esses::test::TempDir tmp("portable");
    std::ofstream(tmp.path() / "portable.marker") << "";
    esses::PathOverrides o;
    o.exeDir = tmp.path();
    const esses::Result<Paths> r = Paths::resolve(o);
    REQUIRE(r.ok());
    CHECK(r.value().portable());
    CHECK(r.value().configDir() == tmp.path() / "config");
    CHECK(r.value().cacheDir() == tmp.path() / "cache");
    CHECK(r.value().logDir() == tmp.path() / "logs");
}

TEST_CASE("paths: OS defaults", "[unit][paths]") {
    EnvGuard guard;
    EnvGuard::clear();
    const esses::Result<Paths> r = Paths::resolve();
    REQUIRE(r.ok());
    const Paths& p = r.value();
    CHECK_FALSE(p.exeDir().empty());
    CHECK(p.configDir().filename() == "esses");
    CHECK(p.configDir().is_absolute());
    CHECK(p.cacheDir().is_absolute());
    CHECK(p.logDir().is_absolute());
    CHECK(p.projectsDir().filename() == "Projects");
    CHECK(p.projectsDir().parent_path().filename() == "esses");
#if defined(ESSES_OS_WINDOWS)
    CHECK(p.dataDir() == p.exeDir() / "data");
    CHECK(p.logDir() == p.configDir() / "logs");
    CHECK(p.cacheDir().filename() == "cache");
#endif
}
