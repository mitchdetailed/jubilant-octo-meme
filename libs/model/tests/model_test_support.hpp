// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Shared helpers for the esses_model tests.
#pragma once

#include <esses/base/utf8.hpp>
#include <esses/model/model.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/units/unit_registry.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::model::test {

inline std::filesystem::path repoRoot() {
    return (pathFromUtf8(ESSES_TEST_SOURCE_DIR) / ".." / "..").lexically_normal();
}

inline std::filesystem::path schemasDir() {
    return repoRoot() / "schemas";
}

inline std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline void writeFile(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// A fresh, empty directory under the test binary directory.
inline std::filesystem::path scratchDir(std::string_view name) {
    static std::atomic<int> counter{0};
    const auto dir = pathFromUtf8(ESSES_TEST_BINARY_DIR) / "model_scratch" /
                     (std::string(name) + "-" + std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
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

inline SourceId testSource() {
    return SourceId{0x0123456789abcdefull, 0xfedcba9876543210ull};
}

// The chunk cache the test columns use (the runtime installs none in a test executable).
inline store::ChunkCache& testCache() {
    static store::ChunkCache cache(std::size_t{64} << 20);
    return cache;
}

// A logged channel with a small in-memory F32 column.
inline Channel makeChannel(std::string_view name, std::string_view loggedName = {},
                           ChannelStatus status = ChannelStatus::Ok) {
    Channel c;
    c.desc.name = std::string(name);
    c.desc.loggedName = loggedName.empty() ? std::string(name) : std::string(loggedName);
    c.desc.id = channelIdLogged(testSource(), kNormalLayer, c.desc.loggedName, "test.importer");
    c.desc.rate = Rate::fromHz(10);
    store::ColumnOptions options;
    options.cache = &testCache();
    c.series = store::UniformSeries{
        Rate::fromHz(10), 0, store::Column(store::InMemoryProvider::fromF32({1.0f, 2.0f, 3.0f}), options)};
    c.setStatus(status, status == ChannelStatus::Error ? "failed" : "");
    return c;
}

// Validates JSON text against schemas/<id>.schema.json; returns "" when valid, else the errors.
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
        nlohmann::json::parse(readFile(schemasDir() / (std::string(schemaId) + ".schema.json"))));
    Collect errors;
    validator.validate(nlohmann::json::parse(text), errors);
    return errors.out.str();
}

inline std::vector<std::filesystem::path> exampleFiles(std::string_view schemaId, std::string_view kind) {
    std::vector<std::filesystem::path> out;
    const auto dir = schemasDir() / "examples" / std::string(schemaId) / std::string(kind);
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".json") {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace esses::model::test
