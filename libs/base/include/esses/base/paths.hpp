// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Paths (01 section 9, D-055, P-5, P-15): per-user and shipped-data locations without Qt, shared by the GUI
// and the CLI. Precedence for each location: explicit override (CLI flag) > environment override
// (<prefix>_CONFIG_DIR, _CACHE_DIR, _DATA_DIR, _LOG_DIR) > portable mode > the OS default.
//   Windows  config %APPDATA%\<dir>\   cache %LOCALAPPDATA%\<dir>\cache\   logs <config>\logs\ (folders)
//            data <exe dir>\data\      projects Documents\<dir>\Projects\  (SHGetKnownFolderPath)
//   Linux    config $XDG_CONFIG_HOME/<dir>/ (~/.config)   cache $XDG_CACHE_HOME/<dir>/ (~/.cache)
//            logs $XDG_STATE_HOME/<dir>/logs/ (~/.local/state)   data <prefix>/share/<dir>/data/
//            projects $XDG_DOCUMENTS_DIR or ~/Documents, then <dir>/Projects/
//   macOS    config ~/Library/Application Support/<dir>/   cache ~/Library/Caches/<dir>/
//            logs ~/Library/Logs/<dir>/   data <App>.app/Contents/Resources/data/ (or
//            <prefix>/share/<dir>/data/)
// Portable mode: a file named portable.marker next to the executable puts config, cache and logs under
// <exe dir>/config, /cache and /logs. <dir> is ESSES_DIR_NAME of cmake/naming.cmake. Directories are
// resolved, not created.
#pragma once

#include <esses/base/config.hpp>
#include <esses/base/result.hpp>

#include <filesystem>
#include <optional>
#include <vector>

namespace esses {

struct PathOverrides { // from CLI flags --config-dir, --data-dir, ...
    std::optional<std::filesystem::path> configDir;
    std::optional<std::filesystem::path> cacheDir;
    std::optional<std::filesystem::path> dataDir;
    std::optional<std::filesystem::path> logDir;
    std::optional<std::filesystem::path> exeDir; // tests: pretend the executable lives here
};

class Paths {
public:
    static Result<Paths> resolve(const PathOverrides& overrides = {});

    const std::filesystem::path& configDir() const noexcept { return config_; }
    const std::filesystem::path& cacheDir() const noexcept { return cache_; }
    const std::filesystem::path& logDir() const noexcept { return log_; }
    const std::filesystem::path& dataDir() const noexcept { return data_; }
    const std::filesystem::path& projectsDir() const noexcept { return projects_; }
    const std::filesystem::path& exeDir() const noexcept { return exe_; }
    const std::vector<std::filesystem::path>& pluginDirs() const noexcept { return plugins_; } // v1.0
    bool portable() const noexcept { return portable_; }

    // Directory of the running executable (empty if it cannot be determined).
    static std::filesystem::path executableDir();

private:
    std::filesystem::path config_;
    std::filesystem::path cache_;
    std::filesystem::path log_;
    std::filesystem::path data_;
    std::filesystem::path projects_;
    std::filesystem::path exe_;
    std::vector<std::filesystem::path> plugins_;
    bool portable_ = false;
};

} // namespace esses
