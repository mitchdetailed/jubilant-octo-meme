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
#include <system_error>

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
    // The build tree: CMake copies data/ beside every executable (apps/cli, libs/project), on every OS.
    std::error_code ec;
    if (fs::is_directory(p.exeDir() / "data", ec)) {
        CHECK(p.dataDir() == p.exeDir() / "data");
    }
#if defined(ESSES_OS_WINDOWS)
    CHECK(p.dataDir() == p.exeDir() / "data");
    CHECK(p.logDir() == p.configDir() / "logs");
    CHECK(p.cacheDir().filename() == "cache");
#endif
}

// Shipped data without an override (01 section 9): <exe dir>/data first on every OS (a build tree or a
// portable layout), then the installed location; the installed location when none exists. Each case fakes the
// layout in a temp directory and points exeDir at it.
namespace {

// ESSES_DIR_NAME: the last element of the default configuration directory (not portable, no override).
fs::path dirName(const fs::path& exe) {
    esses::PathOverrides o;
    o.exeDir = exe;
    const esses::Result<Paths> r = Paths::resolve(o);
    REQUIRE(r.ok());
    REQUIRE_FALSE(r.value().portable());
    return r.value().configDir().filename();
}

esses::Result<Paths> resolveFrom(const fs::path& exe) {
    esses::PathOverrides o;
    o.exeDir = exe;
    return Paths::resolve(o);
}

// The installed shipped-data location of this OS for an executable in `exe` (outside a macOS bundle).
fs::path installedData(const fs::path& exe, const fs::path& dir) {
#if defined(ESSES_OS_WINDOWS)
    static_cast<void>(dir);
    return exe / "data";
#else
    return (exe / ".." / "share" / dir / "data").lexically_normal();
#endif
}

} // namespace

TEST_CASE("paths: shipped data beside the executable wins on every OS", "[unit][paths]") {
    EnvGuard guard;
    EnvGuard::clear();
    esses::test::TempDir tmp("data-beside");
    const fs::path exe = tmp.path() / "bin";
    const fs::path dir = dirName(exe);
    fs::create_directories(exe / "data");
    fs::create_directories(tmp.path() / "share" / dir / "data"); // an installed copy loses to the build tree
    const esses::Result<Paths> r = resolveFrom(exe);
    REQUIRE(r.ok());
    CHECK(r.value().dataDir() == exe / "data");
    CHECK_FALSE(r.value().portable());
}

TEST_CASE("paths: a portable layout finds its shipped data beside the executable", "[unit][paths]") {
    EnvGuard guard;
    EnvGuard::clear();
    esses::test::TempDir tmp("data-portable");
    const fs::path exe = tmp.path() / "esses";
    fs::create_directories(exe / "data");
    std::ofstream(exe / "portable.marker") << "";
    const esses::Result<Paths> r = resolveFrom(exe);
    REQUIRE(r.ok());
    CHECK(r.value().portable());
    CHECK(r.value().dataDir() == exe / "data");
    CHECK(r.value().configDir() == exe / "config");
}

TEST_CASE("paths: shipped data falls back to the installed location", "[unit][paths]") {
    EnvGuard guard;
    EnvGuard::clear();
    esses::test::TempDir tmp("data-installed");
    const fs::path exe = tmp.path() / "bin";
    fs::create_directories(exe);
    const fs::path dir = dirName(exe);
    const fs::path installed = installedData(exe, dir);
    fs::create_directories(installed);
    const esses::Result<Paths> r = resolveFrom(exe);
    REQUIRE(r.ok());
    CHECK(r.value().dataDir().lexically_normal() == installed);
#if !defined(ESSES_OS_WINDOWS)
    CHECK(r.value().dataDir().lexically_normal() == (tmp.path() / "share" / dir / "data").lexically_normal());
#endif
}

TEST_CASE("paths: without any shipped data directory the installed location is named", "[unit][paths]") {
    EnvGuard guard;
    EnvGuard::clear();
    esses::test::TempDir tmp("data-none");
    const fs::path exe = tmp.path() / "bin";
    fs::create_directories(exe);
    const fs::path dir = dirName(exe);
    const esses::Result<Paths> r = resolveFrom(exe);
    REQUIRE(r.ok());
    CHECK(r.value().dataDir().lexically_normal() == installedData(exe, dir));
    CHECK_FALSE(fs::exists(r.value().dataDir())); // resolved, not created
}

TEST_CASE("paths: the data flag and the environment still beat data beside the executable", "[unit][paths]") {
    EnvGuard guard;
    EnvGuard::clear();
    esses::test::TempDir tmp("data-override");
    const fs::path exe = tmp.path() / "bin";
    fs::create_directories(exe / "data");
    REQUIRE(esses::setEnv(esses::envName("DATA_DIR"), esses::pathToUtf8(tmp.path() / "env-data")));
    esses::PathOverrides o;
    o.exeDir = exe;
    {
        const esses::Result<Paths> r = Paths::resolve(o);
        REQUIRE(r.ok());
        CHECK(r.value().dataDir() == tmp.path() / "env-data");
    }
    o.dataDir = tmp.path() / "flag-data";
    const esses::Result<Paths> r = Paths::resolve(o);
    REQUIRE(r.ok());
    CHECK(r.value().dataDir() == tmp.path() / "flag-data");
}

TEST_CASE("paths: a data directory that is a file is not taken", "[unit][paths]") {
    EnvGuard guard;
    EnvGuard::clear();
    esses::test::TempDir tmp("data-file");
    const fs::path exe = tmp.path() / "bin";
    fs::create_directories(exe);
    std::ofstream(exe / "data") << "not a directory";
#if defined(ESSES_OS_WINDOWS)
    const esses::Result<Paths> r = resolveFrom(exe);
    REQUIRE(r.ok());
    CHECK(r.value().dataDir() == exe / "data"); // the installed location, named even though unusable
#else
    const fs::path dir = dirName(exe);
    fs::create_directories(installedData(exe, dir));
    const esses::Result<Paths> r = resolveFrom(exe);
    REQUIRE(r.ok());
    CHECK(r.value().dataDir().lexically_normal() == installedData(exe, dir));
#endif
}

#if defined(ESSES_OS_MACOS)
TEST_CASE("paths: a macOS bundle reads Resources, then the data beside the bundle", "[unit][paths]") {
    EnvGuard guard;
    EnvGuard::clear();
    esses::test::TempDir tmp("data-bundle");
    const fs::path contents = tmp.path() / "bin" / "Esses.app" / "Contents";
    const fs::path exe = contents / "MacOS";
    fs::create_directories(exe);
    // Neither exists: the bundle's Resources is the installed location.
    {
        const esses::Result<Paths> r = resolveFrom(exe);
        REQUIRE(r.ok());
        CHECK(r.value().dataDir() == (contents / "Resources" / "data").lexically_normal());
    }
    // A bundle in the build tree: <bin>/Esses.app beside <bin>/data.
    fs::create_directories(tmp.path() / "bin" / "data");
    {
        const esses::Result<Paths> r = resolveFrom(exe);
        REQUIRE(r.ok());
        CHECK(r.value().dataDir() == (tmp.path() / "bin" / "data").lexically_normal());
    }
    // An installed bundle: its own Resources wins over anything beside it.
    fs::create_directories(contents / "Resources" / "data");
    const esses::Result<Paths> r = resolveFrom(exe);
    REQUIRE(r.ok());
    CHECK(r.value().dataDir() == (contents / "Resources" / "data").lexically_normal());
}
#endif
