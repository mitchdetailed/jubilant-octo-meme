// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared helpers for esses_formats_tests (the importer layer through its public API). Synthetic inputs are
// made with the core's LdWriter (src/ld/), which these tests reach directly.
#pragma once

#include "ld_test_support.hpp"

#include <esses/base/env.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/formats.hpp>
#include <esses/model/model.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/units/unit_registry.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace esses::formats::test {

inline std::filesystem::path repoRoot() {
    return pathFromUtf8(ESSES_TEST_REPO_ROOT);
}

inline std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline void writeText(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

inline void writeBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Writes an .ld built by the core's writer; REQUIREs success.
inline void writeLd(const std::filesystem::path& path, const ld::LdWriteInput& in,
                    const ld::LdWriteOptions& opt = {}) {
    auto bytes = ld::LdWriter::build(in, opt);
    REQUIRE(bytes.ok());
    writeBytes(path, *bytes);
}

// The shipped unit table, loaded once.
inline const units::UnitRegistry& shippedUnits() {
    static const units::UnitRegistry registry = [] {
        auto r = units::UnitRegistry::loadFiles(repoRoot() / "data" / "units.json");
        if (!r) {
            FAIL("data/units.json does not load: " << r.error().toString());
        }
        return std::move(r).value();
    }();
    return registry;
}

// The chunk cache the test columns use (a test executable installs none).
inline store::ChunkCache& testCache() {
    static store::ChunkCache cache(std::size_t{512} << 20);
    return cache;
}

inline OpenContext context(const units::UnitRegistry* units = nullptr) {
    OpenContext c;
    c.units = units;
    c.cache = &testCache();
    return c;
}

// Validates JSON text against schemas/<id>.schema.json; "" when valid, else the errors.
inline std::string schemaErrors(std::string_view schemaId, const std::string& text) {
    class Collect : public nlohmann::json_schema::error_handler {
    public:
        void error(const nlohmann::json::json_pointer& ptr, const nlohmann::json&,
                   const std::string& msg) override {
            out << "  at '" << ptr.to_string() << "': " << msg << "\n";
        }
        std::ostringstream out;
    };
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(
        nlohmann::json::parse(readText(repoRoot() / "schemas" / (std::string(schemaId) + ".schema.json"))));
    Collect errors;
    validator.validate(nlohmann::json::parse(text), errors);
    return errors.out.str();
}

// Every sample of a column as double (NaN where invalid) and its validity.
struct ColumnDump {
    std::vector<double> values;
    std::vector<bool> valid;
};
inline ColumnDump dumpColumn(const store::Column& col) {
    ColumnDump d;
    const store::SampleIdx n = col.size();
    if (n == 0) {
        return d;
    }
    auto h = col.require(0, n - 1, Priority::Background, {});
    REQUIRE(h.ok());
    for (store::SampleIdx i = 0; i < n; ++i) {
        d.values.push_back(h->at(i));
        d.valid.push_back(h->validAt(i));
    }
    return d;
}

inline const model::Channel& channelNamed(const model::DataSource& src, std::string_view name) {
    const model::Channel* c = src.layers.at(0).find(name);
    REQUIRE(c != nullptr);
    return *c;
}

inline const store::Column& columnOf(const model::Channel& c) {
    return store::seriesValues(c.series);
}

inline bool logHas(const std::vector<ImportLogEntry>& log, std::string_view code) {
    for (const ImportLogEntry& e : log) {
        if (e.code == code) {
            return true;
        }
    }
    return false;
}

inline const ImportLogEntry* logEntry(const std::vector<ImportLogEntry>& log, std::string_view code) {
    for (const ImportLogEntry& e : log) {
        if (e.code == code) {
            return &e;
        }
    }
    return nullptr;
}

inline const model::Detail* detailOf(const std::vector<model::Detail>& details, std::string_view id) {
    return model::findDetail(details, id);
}

} // namespace esses::formats::test
