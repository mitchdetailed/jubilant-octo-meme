// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "app_context.hpp"

#include "cli_common.hpp"

#include <esses/base/paths.hpp>

#include <system_error>
#include <vector>

namespace esses::cli {
namespace {

// Candidate data directories in precedence order (see the header).
std::vector<std::filesystem::path> dataDirs(const GlobalOptions& o) {
    std::vector<std::filesystem::path> out;
    if (o.dataDir) {
        out.push_back(*o.dataDir);
        return out;
    }
    PathOverrides po;
    if (auto paths = Paths::resolve(po); paths && !paths->dataDir().empty()) {
        out.push_back(paths->dataDir());
    }
    out.push_back(AppContext::sourceDir() / "data");
    return out;
}

bool isFile(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

bool isDir(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_directory(p, ec);
}

} // namespace

AppContext::AppContext(GlobalOptions options) : options_(std::move(options)) {}

AppContext::~AppContext() {
    registry_.reset(); // before the units it borrows
}

std::filesystem::path AppContext::sourceDir() {
#ifdef ESSES_CLI_SOURCE_DIR
    return p8(ESSES_CLI_SOURCE_DIR);
#else
    return {};
#endif
}

Result<const units::UnitRegistry*> AppContext::units() {
    if (options_.noUnits) {
        return static_cast<const units::UnitRegistry*>(nullptr);
    }
    if (!unitsLoaded_) {
        std::filesystem::path file;
        if (options_.unitsFile) {
            file = *options_.unitsFile;
        } else {
            for (const auto& d : dataDirs(options_)) {
                if (isFile(d / "units.json")) {
                    file = d / "units.json";
                    break;
                }
            }
        }
        if (file.empty()) {
            return fail(ErrorCode::NotFound,
                        "units.json not found (looked in the data directory and the source tree); pass "
                        "--units <file> or --no-units");
        }
        auto r = units::UnitRegistry::loadFiles(file);
        if (!r) {
            Error e = r.error();
            e.message = u8(file) + ": " + e.message;
            return Failure{std::move(e)};
        }
        units_ = std::make_unique<units::UnitRegistry>(std::move(r).value());
        unitsLoaded_ = true;
    }
    return static_cast<const units::UnitRegistry*>(units_.get());
}

Result<formats::ImporterRegistry*> AppContext::registry() {
    if (!registry_) {
        auto u = units();
        if (!u) {
            return Failure{u.error()};
        }
        registry_ = formats::ImporterRegistry::withBuiltins(*u);
    }
    return registry_.get();
}

store::ChunkCache& AppContext::cache() {
    if (!cache_) {
        cache_ = std::make_unique<store::ChunkCache>();
    }
    return *cache_;
}

formats::OpenContext AppContext::openContext(bool withUnits, bool loadSidecar) {
    formats::OpenContext c;
    c.units = withUnits && !options_.noUnits ? units_.get() : nullptr;
    c.cache = &cache();
    c.loadSidecar = loadSidecar;
    c.mergeCompanion = true;
    return c;
}

Result<std::filesystem::path> AppContext::schemaDir() const {
    if (options_.schemaDir) {
        return *options_.schemaDir;
    }
    for (const auto& d : dataDirs(options_)) {
        if (isDir(d / "schemas")) {
            return d / "schemas";
        }
    }
    if (isDir(sourceDir() / "schemas")) {
        return sourceDir() / "schemas";
    }
    return fail(ErrorCode::NotFound, "schema directory not found; pass --schemas <dir>");
}

} // namespace esses::cli
