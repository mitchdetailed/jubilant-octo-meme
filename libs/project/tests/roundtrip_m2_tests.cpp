// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AC-M2-07: the canonical round trip of 02 section 6.5 rule 3 for every file kind the project folder and the
// sidecar hold (project-1, workbook-1, overrides-1, aliases-1, mappings-1, maths-1, scheme-1, keymap-1,
// sidecar-1, journal-1; profile-1 too; unitmap-1, owned by esses_formats, is in
// tests/document_roundtrip_tests):
//   * save(load(f)) == f byte for byte for the canonical, current-version documents (shipped data and the
//     schema examples written in the canonical form);
//   * save(load(save(load(f)))) == save(load(f)) for every valid example, and the output validates against
//     its schema through SchemaRegistry;
//   * an unknown component type and an x-i2 bag survive load -> save unchanged.
// The maths-1 kind reads through esses_maths' MathModuleIo; while that is an M2 stub of its owner the maths
// checks are skipped (the test names the stub).

#include "test_support.hpp"

#include <esses/maths/math_module_io.hpp>
#include <esses/model/sidecar.hpp>
#include <esses/project/project.hpp>
#include <esses/units/unit_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::project;
namespace pt = esses::project::test;

namespace {

using Rewrite = std::function<Result<std::string>(const std::string&)>;

template <class T> Result<std::string> written(Result<T> parsed) {
    if (!parsed) {
        return Failure{parsed.error()};
    }
    return parsed->toJson();
}

const units::UnitRegistry& unitRegistry() {
    static const units::UnitRegistry registry = [] {
        auto loaded = units::UnitRegistry::loadFiles(pt::repoRoot() / "data" / "units.json");
        REQUIRE(loaded.ok());
        return std::move(loaded).value();
    }();
    return registry;
}

const std::map<std::string, Rewrite>& kinds() {
    static const std::map<std::string, Rewrite> all = {
        {"project-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return written(ProjectFile::parse(t, log));
         }},
        {"workbook-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return written(WorkbookFile::parse(t, "Example.workbook.json", log));
         }},
        {"overrides-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return written(ChannelOverrides::parse(t, log));
         }},
        {"aliases-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return written(Aliases::parse(t, log));
         }},
        {"mappings-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return written(Mappings::parse(t, log));
         }},
        {"scheme-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return written(Scheme::parse(t, log));
         }},
        {"keymap-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return written(Keymap::parse(t, log));
         }},
        {"profile-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return written(Profile::parse(t, log));
         }},
        {"journal-1",
         [](const std::string& t) {
             std::vector<std::string> log;
             return written(JournalFile::parse(t, log));
         }},
        {"sidecar-1",
         [](const std::string& t) -> Result<std::string> {
             auto doc = model::SidecarDocument::parse(t);
             if (!doc) {
                 return Failure{doc.error()};
             }
             return doc->toJson();
         }},
        {"maths-1",
         [](const std::string& t) -> Result<std::string> {
             std::vector<std::string> log;
             auto module = maths::MathModuleIo::parse(t, unitRegistry(), log);
             if (!module) {
                 return Failure{module.error()};
             }
             return maths::MathModuleIo::toJson(*module, unitRegistry());
         }},
    };
    return all;
}

// The canonical, current-version files that must come back byte for byte.
const std::vector<std::pair<std::string, std::string>>& canonicalFiles() {
    static const std::vector<std::pair<std::string, std::string>> files = {
        {"project-1", "schemas/examples/project-1/valid/full.json"},
        {"workbook-1", "data/workbooks/circuit-default.workbook.json"},
        {"overrides-1", "schemas/examples/overrides-1/valid/full.json"},
        {"overrides-1", "schemas/examples/overrides-1/valid/unknown-members.json"},
        {"aliases-1", "schemas/examples/aliases-1/valid/full.json"},
        {"mappings-1", "schemas/examples/mappings-1/valid/full.json"},
        {"mappings-1", "schemas/examples/mappings-1/valid/minimal.json"},
        {"scheme-1", "data/schemes/light.scheme.json"},
        {"scheme-1", "data/schemes/dark.scheme.json"},
        {"keymap-1", "data/keymaps/default.json"},
        {"profile-1", "data/profiles/circuit.profile.json"},
        {"journal-1", "schemas/examples/journal-1/valid/full.json"},
        {"journal-1", "schemas/examples/journal-1/valid/minimal.json"},
        {"sidecar-1", "libs/model/tests/data/rich.sidecar.json"},
        {"keymap-1", "schemas/examples/keymap-1/valid/user.json"},
    };
    return files;
}

} // namespace

TEST_CASE(
    "roundtrip: canonical current-version documents of every kind are byte-identical after load and save",
    "[unit]") {
    std::map<std::string, int> perKind;
    for (const auto& [schemaId, rel] : canonicalFiles()) {
        INFO(rel);
        const auto path = pt::repoRoot() / rel;
        if (!std::filesystem::exists(path)) {
            FAIL_CHECK("missing canonical file " << rel);
            continue;
        }
        const std::string bytes = pt::readAll(path);
        REQUIRE(JsonDoc::parse(bytes)->canonical() == bytes); // the file is in the canonical form
        const auto out = kinds().at(schemaId)(bytes);
        REQUIRE(out.ok());
        CHECK(*out == bytes);
        CHECK(SchemaRegistry::instance().validate(*JsonDoc::parse(*out), schemaId).ok());
        ++perKind[schemaId];
    }
    CHECK(perKind.size() == 10); // every kind of AC-M2-07 except maths-1 (below) and unitmap-1 (formats)
}

TEST_CASE("roundtrip: every valid example of every kind saves idempotently to a schema-valid document",
          "[unit]") {
    std::size_t checked = 0;
    for (const auto& [schemaId, rewrite] : kinds()) {
        const auto dir = pt::repoRoot() / "schemas/examples" / schemaId / "valid";
        for (const auto& path : pt::jsonFiles(dir)) {
            INFO(path.string());
            const auto once = rewrite(pt::readAll(path));
            REQUIRE(once.ok()); // no owner's reader is a stub any more: a failure is a failure
            const auto twice = rewrite(*once);
            REQUIRE(twice.ok());
            CHECK(*twice == *once);
            const auto valid = SchemaRegistry::instance().validate(*JsonDoc::parse(*once), schemaId);
            INFO((valid.ok() ? std::string() : valid.error().message));
            CHECK(valid.ok());
            ++checked;
        }
    }
    CHECK(checked >= 25);
}

TEST_CASE("roundtrip: maths-1 modules come back byte-identical", "[unit]") {
    const std::string text = pt::readAll(pt::repoRoot() / "schemas/examples/maths-1/valid/full.json");
    const auto once = kinds().at("maths-1")(text);
    REQUIRE(once.ok());
    CHECK(kinds().at("maths-1")(*once).value() == *once);
    CHECK(SchemaRegistry::instance().validate(*JsonDoc::parse(*once), "maths-1").ok());
}

TEST_CASE("roundtrip: an unknown component type and the x-i2 bags survive load and save unchanged",
          "[unit]") {
    const std::string text = pt::readAll(pt::repoRoot() / "schemas/examples/workbook-1/valid/full.json");
    const auto out = kinds().at("workbook-1")(text);
    REQUIRE(out.ok());
    CHECK(kinds().at("workbook-1")(*out).value() == *out); // and the written form is stable
    const auto in = nlohmann::json::parse(text);
    const auto back = nlohmann::json::parse(*out);
    CHECK(back.at("x-i2") == in.at("x-i2"));
    const auto& inComponents = in.at("worksheets").at(0).at("components");
    const auto& outComponents = back.at("worksheets").at(0).at("components");
    REQUIRE(inComponents.size() == outComponents.size());
    std::size_t unknownTypes = 0;
    std::size_t bags = 0;
    for (std::size_t i = 0; i < inComponents.size(); ++i) {
        INFO(i);
        if (!isKnownComponentType(inComponents[i].at("type").get<std::string>())) {
            // Every member the file wrote comes back unchanged, the body verbatim; the writer adds the
            // header members the file left at their defaults (the written form of 02 section 6.4).
            for (auto it = inComponents[i].begin(); it != inComponents[i].end(); ++it) {
                INFO(it.key());
                CHECK(outComponents[i].at(it.key()) == it.value());
            }
            ++unknownTypes;
        }
        if (inComponents[i].contains("x-i2")) {
            CHECK(outComponents[i].at("x-i2") == inComponents[i].at("x-i2"));
            ++bags;
        }
    }
    CHECK(unknownTypes >= 1);
    CHECK(bags >= 1);
}
