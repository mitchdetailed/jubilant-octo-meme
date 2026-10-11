// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "base_build_config.hpp"

#include <esses/base/env.hpp>
#include <esses/base/paths.hpp>
#include <esses/base/utf8.hpp>

#include <string>
#include <system_error>

#if defined(ESSES_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <cstdint>
#include <vector>

#include <mach-o/dyld.h>
#endif

namespace esses {
namespace fs = std::filesystem;

namespace {

std::optional<fs::path> envPath(std::string_view suffix) {
    const std::optional<std::string> v = getEnv(envName(suffix));
    if (!v || v->empty()) {
        return std::nullopt;
    }
    return pathFromUtf8(*v);
}

#if defined(ESSES_OS_WINDOWS)
std::optional<fs::path> knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    std::optional<fs::path> result;
    if (SUCCEEDED(hr) && raw != nullptr) {
        result = fs::path(std::wstring(raw));
    }
    CoTaskMemFree(raw);
    return result;
}
#else
std::optional<fs::path> homeDir() {
    const std::optional<std::string> home = getEnv("HOME");
    if (!home || home->empty()) {
        return std::nullopt;
    }
    return pathFromUtf8(*home);
}

#if !defined(__APPLE__)
// XDG base directory: the variable when it holds an absolute path, else $HOME/<fallback>.
std::optional<fs::path> xdgDir(std::string_view variable, std::string_view fallback) {
    if (const std::optional<std::string> v = getEnv(variable); v && !v->empty()) {
        fs::path p = pathFromUtf8(*v);
        if (p.is_absolute()) {
            return p;
        }
    }
    const std::optional<fs::path> home = homeDir();
    if (!home) {
        return std::nullopt;
    }
    return *home / pathFromUtf8(fallback);
}
#endif // !__APPLE__
#endif

Failure notFound(const char* what) {
    return fail(ErrorCode::NotFound, std::string("cannot determine the ") + what + " directory");
}

} // namespace

fs::path Paths::executableDir() {
#if defined(ESSES_OS_WINDOWS)
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf).parent_path();
        }
        buf.resize(buf.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        return {};
    }
    std::error_code ec;
    const fs::path exe = fs::weakly_canonical(fs::path(buf.data()), ec);
    return ec ? fs::path(buf.data()).parent_path() : exe.parent_path();
#else
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : exe.parent_path();
#endif
}

Result<Paths> Paths::resolve(const PathOverrides& overrides) {
    Paths p;
    const fs::path dirName = pathFromUtf8(ESSES_BUILD_DIR_NAME);
    p.exe_ = overrides.exeDir ? *overrides.exeDir : executableDir();
    std::error_code ec;
    p.portable_ = !p.exe_.empty() && fs::exists(p.exe_ / "portable.marker", ec);

    // ---- OS defaults ----
    std::optional<fs::path> defConfig;
    std::optional<fs::path> defCache;
    std::optional<fs::path> defLogs;
    std::optional<fs::path> documents;
    fs::path defData;
    std::vector<fs::path> plugins;
#if defined(ESSES_OS_WINDOWS)
    if (const auto roaming = knownFolder(FOLDERID_RoamingAppData)) {
        defConfig = *roaming / dirName;
    }
    if (const auto local = knownFolder(FOLDERID_LocalAppData)) {
        defCache = *local / dirName / "cache";
    }
    documents = knownFolder(FOLDERID_Documents);
    defData = p.exe_ / "data";
    plugins.push_back(p.exe_ / "plugins");
#elif defined(__APPLE__)
    if (const auto home = homeDir()) {
        defConfig = *home / "Library" / "Application Support" / dirName;
        defCache = *home / "Library" / "Caches" / dirName;
        defLogs = *home / "Library" / "Logs" / dirName;
        documents = *home / "Documents";
    }
    const bool inBundle = p.exe_.filename() == "MacOS" && p.exe_.parent_path().filename() == "Contents";
    if (inBundle) {
        defData = (p.exe_ / ".." / "Resources" / "data").lexically_normal();
        plugins.push_back((p.exe_ / ".." / "PlugIns" / dirName).lexically_normal());
    } else {
        defData = (p.exe_ / ".." / "share" / dirName / "data").lexically_normal();
        plugins.push_back((p.exe_ / ".." / "lib" / dirName / "plugins").lexically_normal());
    }
#else
    if (const auto base = xdgDir("XDG_CONFIG_HOME", ".config")) {
        defConfig = *base / dirName;
    }
    if (const auto base = xdgDir("XDG_CACHE_HOME", ".cache")) {
        defCache = *base / dirName;
    }
    if (const auto base = xdgDir("XDG_STATE_HOME", ".local/state")) {
        defLogs = *base / dirName / "logs";
    }
    documents = xdgDir("XDG_DOCUMENTS_DIR", "Documents");
    defData = (p.exe_ / ".." / "share" / dirName / "data").lexically_normal();
    plugins.push_back((p.exe_ / ".." / "lib" / dirName / "plugins").lexically_normal());
#endif

    // ---- config ----
    if (overrides.configDir) {
        p.config_ = *overrides.configDir;
    } else if (auto e = envPath("CONFIG_DIR")) {
        p.config_ = *e;
    } else if (p.portable_) {
        p.config_ = p.exe_ / "config";
    } else if (defConfig) {
        p.config_ = *defConfig;
    } else {
        return notFound("configuration");
    }

    // ---- cache ----
    if (overrides.cacheDir) {
        p.cache_ = *overrides.cacheDir;
    } else if (auto e = envPath("CACHE_DIR")) {
        p.cache_ = *e;
    } else if (p.portable_) {
        p.cache_ = p.exe_ / "cache";
    } else if (defCache) {
        p.cache_ = *defCache;
    } else {
        return notFound("cache");
    }

    // ---- logs ----
    if (overrides.logDir) {
        p.log_ = *overrides.logDir;
    } else if (auto e = envPath("LOG_DIR")) {
        p.log_ = *e;
    } else if (p.portable_) {
        p.log_ = p.exe_ / "logs";
    } else if (defLogs) {
        p.log_ = *defLogs;
    } else {
        p.log_ = p.config_ / "logs"; // Windows: <config>\logs\ (01 section 9)
    }

    // ---- shipped data ----
    if (overrides.dataDir) {
        p.data_ = *overrides.dataDir;
    } else if (auto e = envPath("DATA_DIR")) {
        p.data_ = *e;
    } else {
        p.data_ = defData;
    }

    // ---- projects and plugins ----
    if (documents) {
        p.projects_ = *documents / dirName / "Projects";
    } else {
        p.projects_ = p.config_ / "Projects";
    }
    plugins.push_back(p.config_ / "plugins");
    p.plugins_ = std::move(plugins);
    return p;
}

} // namespace esses
