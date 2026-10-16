// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Hostile input (05 section 9.3 invariants, the fuzz targets' in-tree companion): seeded mutations of the
// text fixtures under every shipped profile never crash, never throw and decode every channel they open;
// the user's private sample files, when present, open and decode (aggregate checks only, [oracle]).
#include "text_test_support.hpp"

#include <esses/base/env.hpp>

#include <cstdlib>
#include <random>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::texttest;

namespace {

std::uint64_t seed() {
    if (const auto s = getEnv("ESSES_TEST_SEED"); s && !s->empty()) {
        return std::strtoull(s->c_str(), nullptr, 10);
    }
    return 20261012;
}

std::vector<std::string> seeds() {
    std::vector<std::string> out;
    for (const char* dir : {"motec.csv", "generic.csv"}) {
        const auto root = repoRoot() / "fixtures" / dir;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (it->is_regular_file() && it->path().filename().string().starts_with("input.")) {
                std::string text = readText(it->path());
                text.resize(
                    std::min<std::size_t>(text.size(), 6000)); // the head: header rules and a few rows
                out.push_back(std::move(text));
            }
        }
    }
    out.emplace_back("Time,A\n0,1\n1,2\n");
    out.emplace_back("\"Format\",\"x\"\r\n\r\n\"Time\",\"A\"\r\n\"s\",\"m\"\r\n\r\n\"0\",\"1\"\r\n");
    return out;
}

void decodeAll(const OpenedSource& o) {
    for (const model::DataLayer& l : o.source->layers) {
        for (const model::Channel& ch : l.channels) {
            const store::Column& v = store::seriesValues(ch.series);
            if (v.size() > 0) {
                CHECK(v.require(0, v.size() - 1, Priority::Background, {}).ok());
            }
        }
    }
}

} // namespace

TEST_CASE("text hostile: seeded mutations never crash the text importers", "[property]") {
    std::mt19937_64 rng(seed());
    Env env(true);
    const std::vector<std::string> base = seeds();
    REQUIRE(base.size() >= 2);
    const std::string alphabet = ",;\t|\"\r\n .-+eE0123456789:#xNaN";
    int opened = 0;
    for (int round = 0; round < 300; ++round) {
        std::string s = base[rng() % base.size()];
        const int edits = 1 + static_cast<int>(rng() % 12);
        for (int e = 0; e < edits && !s.empty(); ++e) {
            const std::size_t at = rng() % s.size();
            switch (rng() % 4) {
            case 0:
                s[at] = alphabet[rng() % alphabet.size()];
                break;
            case 1:
                s.insert(at, 1, alphabet[rng() % alphabet.size()]);
                break;
            case 2:
                s.erase(at, 1 + rng() % 16);
                break;
            default:
                s.insert(at, s.substr(rng() % s.size(), rng() % 64));
                break;
            }
        }
        for (const char* importer : {"motec.csv", "text.profile:generic.csv"}) {
            ImportOptions opt;
            opt.importerId = importer;
            auto o = env.open(s, "fuzz.csv", opt);
            if (o.ok()) {
                ++opened;
                decodeAll(*o);
            } else {
                CHECK(o.error().code != ErrorCode::Internal);
            }
        }
        env.cache.clear();
    }
    CHECK(opened > 0);
}

TEST_CASE("text hostile: the private sample files open and decode", "[oracle]") {
    // Local only: a directory of third-party sample text files (never committed, AGENTS.md section 6) named
    // by ESSES_PRIVATE_TEXT_SAMPLES. Only aggregate facts are asserted.
    const auto dir = getEnv("ESSES_PRIVATE_TEXT_SAMPLES");
    if (!dir || dir->empty() || !std::filesystem::is_directory(pathFromUtf8(*dir))) {
        SKIP("skipped (private): ESSES_PRIVATE_TEXT_SAMPLES is not set");
    }
    Env env(true);
    int files = 0;
    for (const auto& e : std::filesystem::directory_iterator(pathFromUtf8(*dir))) {
        if (!e.is_regular_file()) {
            continue;
        }
        auto o = env.registry->open(e.path(), {}, env.context());
        if (!o.ok()) {
            WARN("not opened: " << e.path().filename().string() << ": " << o.error().toString());
            continue;
        }
        ++files;
        CHECK_FALSE(o->source->layers.at(0).channels.empty());
        decodeAll(*o);
        env.cache.clear();
    }
    CHECK(files > 0);
}

TEST_CASE("text hostile: a first line longer than the header window fails at once with Format", "[unit]") {
    // A newline-less file of delimiters (a binary or a truncated export renamed .csv): no cell is split.
    const std::string line(std::size_t{16} << 20, ',');
    Env env(true);
    for (const char* importer : {"text.profile:generic.csv", "motec.csv"}) {
        ImportOptions opt;
        opt.importerId = importer;
        auto o = env.open(line, "long.csv", opt);
        REQUIRE_FALSE(o.ok());
        CHECK(o.error().code == ErrorCode::Format);
        CHECK(o.error().message.find("exceeds the maximum line length") != std::string::npos);
    }
    // Blank lines before it change nothing.
    auto o = env.open("\n\r\n" + line + "\nTime,A\n0,1\n", "long.csv");
    REQUIRE_FALSE(o.ok());
    CHECK(o.error().code == ErrorCode::Format);
}

TEST_CASE("text hostile: a data row of 16 MiB of delimiters is one ragged row", "[unit]") {
    std::string s = "Time,A\n0,1\n1,2\n2,3";
    s.append(std::size_t{16} << 20, ',');
    s += "\n3,4\n";
    Env env(true);
    ImportOptions opt;
    opt.importerId = "text.profile:generic.csv";
    auto o = env.open(s, "wide.csv", opt);
    REQUIRE(o.ok());
    const Dump a = dumpSeries(channel(*o, "A").series);
    REQUIRE(a.values.size() == 4);
    CHECK(a.values[2] == 3.0);
    CHECK(a.values[3] == 4.0);
    const std::vector<ImportLogEntry> decoded = o->decodeLog->snapshot();
    const ImportLogEntry* ragged = entry(decoded, "text.ragged_row");
    REQUIRE(ragged != nullptr);
    CHECK(ragged->ctx.line == std::optional<std::uint64_t>(4));
}
