// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Private smoke test of the user's real .ld files (spike S-1, fixtures/private/s1; never committed) through
// the public API: ImporterRegistry::sniff -> probe -> open -> DataSource columns -> the motec.ld writer.
// Skipped cleanly when fixtures/private is absent (08 section 6.2, tag [oracle]).
//
// Per file: the registry picks motec.ld with confidence >= 90; probe_json validates against probe-1; the open
// pipeline builds one Column per probed channel, and the first and last chunk of every channel equal the
// independent oracle's first/last values (tools/ld_ref_decode.py, ref/<id>.json; integers exact, floats
// within one F32 ulp; samples the gap blocks mark invalid are skipped, the oracle has no validity); opened
// with the shipped unit registry, every converted sample equals the converter applied to the vendor value; a
// .ldx beside the file is merged as the companion; and files up to 32 MB go through the writer and back with
// identical stored bytes and validity. The fixtures root can be moved with ESSES_PRIVATE_FIXTURES.
#include "formats_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>

using namespace esses;
using namespace esses::formats;
using namespace esses::formats::test;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path s1Dir() {
    if (const auto v = getEnv(envName("PRIVATE_FIXTURES")); v && !v->empty()) {
        return pathFromUtf8(*v) / "s1";
    }
    return repoRoot() / "fixtures" / "private" / "s1";
}

std::optional<json> loadJson(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        return json::parse(ss.str());
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

struct CorpusFile {
    fs::path path;
    std::uint64_t size = 0;
    fs::path ref;
};

// The .ld entries of corpus.json with their ref/<nn>_*.json (the same pairing as the core's oracle test).
std::vector<CorpusFile> corpus() {
    const fs::path dir = s1Dir();
    const auto c = loadJson(dir / "corpus.json");
    if (!c || !c->is_array()) {
        return {};
    }
    std::map<std::size_t, fs::path> refs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir / "ref", ec)) {
        const std::string stem = pathToUtf8(e.path().filename());
        const std::size_t us = stem.find('_');
        if (us == std::string::npos) {
            continue;
        }
        try {
            refs[static_cast<std::size_t>(std::stoul(stem.substr(0, us)))] = e.path();
        } catch (const std::exception&) {
        }
    }
    std::vector<CorpusFile> out;
    std::size_t k = 0;
    for (const auto& e : *c) {
        if (e.value("ext", "") != ".ld") {
            continue;
        }
        CorpusFile f;
        f.path = pathFromUtf8(e.value("path", ""));
        f.size = e.value("size", std::uint64_t{0});
        if (const auto it = refs.find(k); it != refs.end()) {
            f.ref = it->second;
        }
        ++k;
        out.push_back(std::move(f));
    }
    return out;
}

std::optional<double> num(const json& v) {
    if (v.is_number()) {
        return v.get<double>();
    }
    return std::nullopt; // "nan"/"inf" strings: not compared here
}

// Values and validity of samples [first, last] of a column.
ColumnDump dumpRange(const store::Column& col, store::SampleIdx first, store::SampleIdx last) {
    ColumnDump d;
    auto h = col.require(first, last, Priority::Background, {});
    REQUIRE(h.ok());
    for (store::SampleIdx i = first; i <= last; ++i) {
        d.values.push_back(h->at(i));
        d.valid.push_back(h->validAt(i));
    }
    return d;
}

} // namespace

TEST_CASE("private corpus: every real file through the public API", "[oracle][ld_private][formats_api]") {
    const auto files = corpus();
    if (files.empty()) {
        SKIP("fixtures/private/s1 is absent; the public-API smoke test needs the user's files");
    }
    const units::UnitRegistry& units = shippedUnits();
    const auto plain = ImporterRegistry::withBuiltins();
    const auto withUnits = ImporterRegistry::withBuiltins(&units);
    ld::test::TempDir tmp("private-api");
    std::size_t opened = 0, channels = 0, compared = 0, mismatches = 0, lowConfidence = 0, schemaFailures = 0,
                converted = 0, conversionMismatches = 0, unknownUnits = 0, companions = 0, roundTrips = 0;
    for (const CorpusFile& cf : files) {
        std::error_code ec;
        if (!fs::exists(cf.path, ec) || fs::file_size(cf.path, ec) != cf.size) {
            continue;
        }
        INFO(pathToUtf8(cf.path));
        const std::string fileName = pathToUtf8(cf.path.filename());

        // 1. sniff
        auto ranked = plain->sniff(cf.path);
        REQUIRE(ranked.ok());
        REQUIRE(!ranked->empty());
        CHECK(ranked->front().first->id() == kLdImporterId);
        if (ranked->front().second.confidence < 90) {
            ++lowConfidence;
            WARN(fileName << ": confidence " << ranked->front().second.confidence << " ("
                          << ranked->front().second.reason << ")");
        }

        // 2. open in vendor units; probe_json against the schema.
        auto o = plain->open(cf.path, {}, context());
        REQUIRE(o.ok());
        ++opened;
        const std::string probeText = o->probe.canonicalJson().value();
        if (const std::string errors = schemaErrors("probe-1", probeText); !errors.empty()) {
            ++schemaFailures;
            WARN(fileName << ": probe_json fails probe-1:\n" << errors);
        }
        const model::DataLayer& layer = o->source->layers.at(0);
        REQUIRE(layer.channels.size() == o->probe.layers.at(0).channels.size());
        channels += layer.channels.size();

        // 3. first and last chunk of every channel against the oracle
        const auto ref = cf.ref.empty() ? std::nullopt : loadJson(cf.ref);
        if (ref && ref->at("channels").size() == layer.channels.size()) {
            for (std::size_t k = 0; k < layer.channels.size(); ++k) {
                const model::Channel& ch = layer.channels[k];
                const json& stats = ref->at("channels")[k].at("stats");
                const json& s = stats.at(stats.contains("afterMul") ? "afterMul" : "beforeMul");
                const store::Column& col = columnOf(ch);
                const store::SampleIdx n = col.size();
                if (n == 0) {
                    continue;
                }
                const bool exactInt = ch.desc.dtype == store::Dtype::I32;
                const auto check = [&](const json& list, store::SampleIdx first) {
                    const store::SampleIdx last =
                        std::min(n - 1, first + static_cast<store::SampleIdx>(list.size()) - 1);
                    const ColumnDump d = dumpRange(col, first, last);
                    for (std::size_t i = 0; i < d.values.size(); ++i) {
                        const auto oracle = num(list[i]);
                        if (!oracle || !d.valid[i]) {
                            continue;
                        }
                        ++compared;
                        const bool same = exactInt ? d.values[i] == *oracle
                                                   : ld::test::f32UlpDistance(d.values[i], *oracle) <= 1;
                        if (!same) {
                            ++mismatches;
                            WARN(fileName << " | " << ch.desc.name << " | sample "
                                          << first + static_cast<store::SampleIdx>(i) << ": " << d.values[i]
                                          << " != oracle " << *oracle);
                        }
                    }
                };
                check(s.at("first"), 0);
                const json& tail = s.at("last");
                check(tail, n - static_cast<store::SampleIdx>(tail.size()));
            }
        }

        // 4. with the unit registry: conversions applied at decode equal the converter on the vendor value.
        auto u = withUnits->open(cf.path, {}, context(&units));
        REQUIRE(u.ok());
        const model::DataLayer& ul = u->source->layers.at(0);
        REQUIRE(ul.channels.size() == layer.channels.size());
        for (std::size_t k = 0; k < ul.channels.size(); ++k) {
            const model::Channel& base = ul.channels[k];
            if (base.desc.statusText.rfind("Unit unknown", 0) == 0) {
                ++unknownUnits;
            }
            const auto& info = u->probe.layers[0].channels[k];
            const auto m = units.match(info.vendorUnit);
            if (base.desc.unit == units::kNoUnit || !m || units.toBase(m->unit).identity()) {
                continue;
            }
            ++converted;
            const model::Channel* vendor = layer.findLogged(base.desc.loggedName);
            REQUIRE(vendor != nullptr);
            const store::SampleIdx n = std::min<store::SampleIdx>(columnOf(base).size(), 256);
            if (n == 0) {
                continue;
            }
            const ColumnDump b = dumpRange(columnOf(base), 0, n - 1);
            const ColumnDump v = dumpRange(columnOf(*vendor), 0, n - 1);
            const units::Converter conv = units.toBase(m->unit);
            for (store::SampleIdx i = 0; i < n; ++i) {
                const auto ix = static_cast<std::size_t>(i);
                if (!b.valid[ix]) {
                    continue;
                }
                const double expect = conv(v.values[ix]);
                const double tol = 4.0 * std::abs(expect) * 0x1p-24 + 1e-30;
                const bool same =
                    !std::isfinite(expect)
                        ? b.values[ix] == expect || (std::isnan(expect) && std::isnan(b.values[ix]))
                        : std::abs(b.values[ix] - expect) <= tol;
                if (!same) {
                    ++conversionMismatches;
                    WARN(fileName << " | " << base.desc.name << " | " << b.values[ix] << " != " << expect);
                    break;
                }
            }
        }

        // 5. the companion
        if (findCompanion(listSiblings(cf.path), cf.path, ".ldx")) {
            ++companions;
            CHECK(u->source->prov.companions.size() == 1);
            const model::Detail* laps = model::findDetail(u->source->details, "Total Laps");
            CHECK(laps != nullptr);
            if (laps != nullptr) {
                CHECK(laps->prov == model::Provenance::Companion);
            }
        }

        // 6. through the writer and back (files up to 32 MB)
        if (cf.size <= (std::uint64_t{32} << 20)) {
            ExportView view;
            view.source = u->source;
            const fs::path out = tmp.path() / "out.ld";
            auto rep = withUnits->writer("motec.ld")->write(out, view, {}, {}, {});
            REQUIRE(rep.ok());
            auto f1 = ld::LdFile::open(cf.path);
            auto f2 = ld::LdFile::open(out);
            REQUIRE(f1.ok());
            REQUIRE(f2.ok());
            REQUIRE((*f2)->channels().size() == (*f1)->channels().size());
            for (std::size_t k = 0; k < (*f1)->channels().size(); ++k) {
                const ld::LdChannel& a = (*f1)->channels()[k];
                const ld::LdChannel& b = (*f2)->channels()[k];
                CHECK(b.invalid == a.invalid);
                const auto da =
                    (*f1)->bytes().subspan(a.enc.dataOffset, a.sampleCount * a.enc.bytesPerSample);
                const auto db =
                    (*f2)->bytes().subspan(b.enc.dataOffset, b.sampleCount * b.enc.bytesPerSample);
                CHECK(std::equal(da.begin(), da.end(), db.begin(), db.end()));
            }
            CHECK((*f2)->deviceEvents().size() == (*f1)->deviceEvents().size());
            ++roundTrips;
            std::error_code rm;
            fs::remove(out, rm);
            fs::remove(tmp.path() / "out.ldx", rm);
        }
    }
    WARN("public API over the private corpus: "
         << opened << " files, " << channels << " channels, " << compared
         << " samples compared with the oracle (" << mismatches << " differences), " << converted
         << " converted channels (" << conversionMismatches << " differences), " << unknownUnits
         << " channels with an unknown unit, " << companions << " companions, " << roundTrips
         << " writer round trips, " << lowConfidence << " low sniff confidences, " << schemaFailures
         << " probe_json schema failures");
    CHECK(opened > 0);
    CHECK(mismatches == 0);
    CHECK(conversionMismatches == 0);
    CHECK(schemaFailures == 0);
    CHECK(lowConfidence == 0);
}

TEST_CASE("private corpus: the shift/mul logger channel reads 75.2 deg F as kelvin",
          "[oracle][ld_private][formats_api]") {
    fs::path file;
    for (const CorpusFile& cf : corpus()) {
        std::error_code ec;
        if (cf.path.filename() == fs::path("20251009-1552000.ld") && fs::exists(cf.path, ec)) {
            file = cf.path;
        }
    }
    if (file.empty()) {
        SKIP("20251009-1552000.ld is not available");
    }
    const units::UnitRegistry& units = shippedUnits();
    const auto reg = ImporterRegistry::withBuiltins(&units);
    auto o = reg->open(file, {}, context(&units));
    REQUIRE(o.ok());
    const model::Channel* oil = o->source->layers[0].findLogged("Gbox Oil Temp");
    REQUIRE(oil != nullptr);
    CHECK(units.unit(oil->desc.unit).symbol == "K");
    // C-series unit rule (c): with the registry, the 8-byte field "F" is the unit (05 section 3.4).
    const auto& infos = o->probe.layers[0].channels;
    const auto info = std::find_if(infos.begin(), infos.end(),
                                   [](const ChannelInfo& c) { return c.loggedName == "Gbox Oil Temp"; });
    REQUIRE(info != infos.end());
    CHECK(info->vendorUnit == "F");
    CHECK((info->flags & channel_flags::kUnitFromShortName) != 0);
    const ColumnDump d = dumpRange(columnOf(*oil), 0, 0);
    REQUIRE(d.valid[0]);
    CHECK(std::abs(d.values[0] - ((75.2 - 32.0) * 5.0 / 9.0 + 273.15)) < 1e-3); // 24.0 deg C (S-1 section 7)
}
