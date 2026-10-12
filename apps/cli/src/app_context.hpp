// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// What every esses-cli command shares: the shipped data (data/units.json, schemas/), the unit registry, the
// importer registry with the built-in importers and writers, the circuit range rules (data/rules) and the
// chunk cache the opened sources' columns use. Data lookup, first hit wins: an explicit flag (--data-dir,
// --units, --schemas); the data directory of esses_base's Paths (<prefix>_DATA_DIR, else <exe dir>/data,
// where the build copies data/ and schemas/); the source tree this binary was built from (developer builds).
#pragma once

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/result.hpp>
#include <esses/formats/registry.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/units/unit_registry.hpp>

#include <filesystem>
#include <memory>
#include <optional>

namespace esses::cli {

struct GlobalOptions {
    std::optional<std::filesystem::path> dataDir;   // --data-dir
    std::optional<std::filesystem::path> unitsFile; // --units
    std::optional<std::filesystem::path> schemaDir; // --schemas
    bool noUnits = false;                           // --no-units: vendor units, no unit rule lookups
};

class AppContext {
public:
    explicit AppContext(GlobalOptions options = {});
    ~AppContext();
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    const GlobalOptions& options() const noexcept { return options_; }

    // The unit registry, loaded on first use; nullptr with --no-units. NotFound / Format from the load.
    Result<const units::UnitRegistry*> units();
    // The importer registry built over units(); a unit table that fails to load is an error of the command.
    Result<formats::ImporterRegistry*> registry();
    store::ChunkCache& cache();
    // The open pipeline's context: the chunk cache, units when `withUnits` (and not --no-units), the sidecar
    // when `loadSidecar`. Requires units() to have been called (registry() does).
    formats::OpenContext openContext(bool withUnits, bool loadSidecar);

    // The phase-1/phase-2 range rules of the circuit profile (AC-M1-07): circuit.whole-outing and
    // circuit.beacon-pair loaded from <data dir>/rules when that directory holds them, else the same rules
    // built in code (analysis::RuleRunner::circuitDefault). Format/Invalid when a shipped rule file is
    // broken.
    Result<const analysis::RuleRunner*> rules();

    // Directory holding <name>-<major>.schema.json files.
    Result<std::filesystem::path> schemaDir() const;
    // The repository this binary was built from (tools/ld_ref_decode.py lives below it); may not exist.
    static std::filesystem::path sourceDir();

private:
    GlobalOptions options_;
    bool unitsLoaded_ = false;
    std::unique_ptr<units::UnitRegistry> units_;
    std::unique_ptr<formats::ImporterRegistry> registry_;
    std::unique_ptr<store::ChunkCache> cache_;
    std::unique_ptr<analysis::RuleRunner> rules_;
};

} // namespace esses::cli
