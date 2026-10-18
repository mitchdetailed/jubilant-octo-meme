// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Helpers of the text-importer tests (formats_text.*): in-memory files opened through the registry, column
// dumps, the shipped unit table, temporary directories.
#pragma once

#include <esses/base/taskpool.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/formats.hpp>
#include <esses/model/model.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/units/unit_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace esses::formats::texttest {

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

inline std::vector<std::byte> bytesOf(std::string_view text) {
    std::vector<std::byte> b(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        b[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return b;
}

inline std::shared_ptr<const ByteSource> memory(std::string_view text, std::string name = "input.csv") {
    return makeMemorySource(bytesOf(text), pathFromUtf8(name));
}

// One cache per test case, so chunk residency checks see only their own columns.
struct Env {
    store::ChunkCache cache{std::size_t{1} << 30};
    std::unique_ptr<ImporterRegistry> registry;
    const units::UnitRegistry* units = nullptr;

    explicit Env(bool withUnits = true) : units(withUnits ? &shippedUnits() : nullptr) {
        registry = ImporterRegistry::withBuiltins(units);
    }
    OpenContext context() {
        OpenContext c;
        c.units = units;
        c.cache = &cache;
        c.loadSidecar = false;
        return c;
    }
    Result<OpenedSource> open(std::string_view text, std::string name = "input.csv",
                              ImportOptions options = {}) {
        return registry->open(memory(text, std::move(name)), std::move(options), context());
    }
};

struct Dump {
    std::vector<double> values;
    std::vector<bool> valid;
    std::vector<Micros> times; // timestamped series only
};

inline Dump dumpColumn(const store::Column& col) {
    Dump d;
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

inline Dump dumpSeries(const store::Series& s) {
    if (const auto* u = std::get_if<store::UniformSeries>(&s)) {
        return dumpColumn(u->values);
    }
    const auto& t = std::get<store::TimestampedSeries>(s);
    Dump d = dumpColumn(t.values);
    if (t.timesUs.size() > 0) {
        auto h = t.timesUs.require(0, t.timesUs.size() - 1, Priority::Background, {});
        REQUIRE(h.ok());
        for (store::SampleIdx i = 0; i < t.timesUs.size(); ++i) {
            d.times.push_back(*h->int64At(i));
        }
    }
    return d;
}

inline const model::Channel& channel(const OpenedSource& o, std::string_view name) {
    const model::Channel* c = o.source->layers.at(0).find(name);
    if (c == nullptr) {
        FAIL("no channel '" << std::string(name) << "'");
    }
    return *c;
}

inline bool hasCode(const std::vector<ImportLogEntry>& log, std::string_view code) {
    for (const ImportLogEntry& e : log) {
        if (e.code == code) {
            return true;
        }
    }
    return false;
}

inline const ImportLogEntry* entry(const std::vector<ImportLogEntry>& log, std::string_view code) {
    for (const ImportLogEntry& e : log) {
        if (e.code == code) {
            return &e;
        }
    }
    return nullptr;
}

// The detail of the first entry with `code`, "<missing>" when there is none.
inline std::string detailOf(const std::vector<ImportLogEntry>& log, std::string_view code) {
    const ImportLogEntry* e = entry(log, code);
    return e != nullptr && e->ctx.detail ? *e->ctx.detail : std::string("<missing>");
}

// A fresh, empty directory under the system temporary directory, removed on destruction. This constructor
// creates it (create_directory) and never takes a name that exists, so test processes running at once, in one
// build tree or in several, never share one.
class TempDir {
public:
    explicit TempDir(std::string_view tag) {
        static std::atomic<unsigned> counter{0};
        static const unsigned salt = std::random_device{}();
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        for (;;) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = base / pathFromUtf8("esses-text-" + std::string(tag) + "-" + std::to_string(salt) + "-" +
                                        std::to_string(now) + "-" + std::to_string(++counter));
            std::error_code ec;
            if (std::filesystem::create_directory(path_, ec)) {
                break;
            }
            if (ec) {
                FAIL("cannot create " << pathToUtf8(path_) << ": " << ec.message());
            }
        }
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace esses::formats::texttest
