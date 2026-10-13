// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-cli validate in M2 (02 section 6.6 rule 5, 08 section 5, AC-M2-08): migrations run before the
// schema check and are reported, a newer major is refused at "/$schema", every error carries its JSON
// pointer, and --all walks data/, fixtures/, tests/maths/ and schemas/ without the private checkouts.

#include "validate.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace esses::cli;

class TempTree {
public:
    TempTree() {
        static std::atomic<int> counter{0};
        root_ = fs::temp_directory_path() /
                ("esses-cli-validate-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(++counter));
        fs::create_directories(root_);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    void write(const std::string& rel, const std::string& text) const {
        const fs::path p = root_ / rel;
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << text;
    }
    const fs::path& root() const { return root_; }

private:
    fs::path root_;
};

nlohmann::json runJson(const ValidateOptions& base, int& exitCode) {
    GlobalOptions global;
    global.schemaDir = fs::path(ESSES_TEST_REPO_ROOT) / "schemas";
    AppContext ctx(global);
    ValidateOptions o = base;
    o.json = true;
    std::ostringstream out;
    std::ostringstream err;
    exitCode = runValidate(ctx, o, out, err);
    INFO(err.str());
    return nlohmann::json::parse(out.str());
}

const nlohmann::json* fileNamed(const nlohmann::json& report, const std::string& suffix) {
    for (const auto& f : report.at("files")) {
        if (f.at("path").get<std::string>().ends_with(suffix)) {
            return &f;
        }
    }
    return nullptr;
}

constexpr const char* kProjectV1 =
    R"({"$schema": "urn:esses:project-1", "schemaVersion": 1, "name": "P",)"
    R"( "dataWindow": {"filters": {"0123456789abcdef0123456789abcdef": {"kind": "all"}}}})";

} // namespace

TEST_CASE("validate: an older minor is migrated first and the steps are reported", "[unit]") {
    const TempTree tree;
    tree.write("p/project.json", kProjectV1);
    // Without the migration the bare key fails the version-2 key pattern; with it the document is valid.
    ValidateOptions o;
    o.paths = {tree.root() / "p/project.json"};
    int code = -1;
    const auto report = runJson(o, code);
    CHECK(code == 0);
    const auto* f = fileNamed(report, "project.json");
    REQUIRE(f != nullptr);
    CHECK(f->at("status") == "valid");
    CHECK(f->at("log") ==
          nlohmann::json::array({"project-1: migrated schemaVersion 1 -> 2 (02 section 6.6)"}));
}

TEST_CASE("validate: a newer major is refused and errors carry their JSON pointer", "[unit]") {
    const TempTree tree;
    tree.write("a/major.json", R"({"$schema": "urn:esses:mappings-7", "schemaVersion": 1, "mappings": []})");
    tree.write("a/broken.json", R"({"$schema": "urn:esses:mappings-1", "schemaVersion": 1,)"
                                R"( "mappings": [{"from": "x", "to": "y", "unit": ""}]})");
    tree.write("a/newer-minor.json",
               R"({"$schema": "urn:esses:mappings-1", "schemaVersion": 4, "mappings": []})");
    ValidateOptions o;
    o.paths = {tree.root() / "a"};
    int code = -1;
    const auto report = runJson(o, code);
    CHECK(code == 1);
    const auto* major = fileNamed(report, "major.json");
    REQUIRE(major != nullptr);
    CHECK(major->at("status") == "invalid");
    CHECK(major->at("errors").at(0).at("pointer") == "/$schema");
    const auto* broken = fileNamed(report, "broken.json");
    REQUIRE(broken != nullptr);
    CHECK(broken->at("errors").at(0).at("pointer") == "/mappings/0/unit");
    const auto* newer = fileNamed(report, "newer-minor.json");
    REQUIRE(newer != nullptr);
    CHECK(newer->at("status") == "valid"); // validated as it is, with a note
    CHECK(newer->at("log").size() == 1);
}

TEST_CASE("validate: --all walks the trees and the schema files and leaves the private checkouts out",
          "[unit]") {
    const TempTree tree;
    const std::string good = R"({"$schema": "urn:esses:mappings-1", "schemaVersion": 1, "mappings": []})";
    const std::string bad = R"({"$schema": "urn:esses:mappings-1", "schemaVersion": 1})";
    tree.write("data/a.json", good);
    tree.write("fixtures/fam/expected.json", good);
    tree.write("fixtures/private/secret.json", bad);
    tree.write("tests/maths/user_modules/mine.json", bad);
    tree.write("tests/maths/oracle/case.json", bad);
    tree.write("tests/maths/public.json", good);
    tree.write("elsewhere/b.json", bad); // not one of the four trees
    tree.write("schemas/thing-1.schema.json",
               R"({"$schema": "http://json-schema.org/draft-07/schema#", "$id": "urn:esses:thing-1"})");
    tree.write("schemas/examples/thing-1/invalid/bad.json", bad); // examples are check_schemas.py's
    CHECK(allValidatePaths(tree.root()).size() == 4);             // three trees and one schema file
    ValidateOptions o;
    o.all = true;
    o.root = tree.root();
    int code = -1;
    const auto report = runJson(o, code);
    CHECK(code == 0);
    CHECK(report.at("summary").at("valid") == 4);
    CHECK(fileNamed(report, "secret.json") == nullptr);
    CHECK(fileNamed(report, "mine.json") == nullptr);
    CHECK(fileNamed(report, "case.json") == nullptr);

    // --exclude leaves out any directory of a walk.
    ValidateOptions ex;
    ex.paths = {tree.root() / "fixtures"};
    ex.exclude = {tree.root() / "fixtures" / "fam"};
    const auto onlyPrivate = runJson(ex, code);
    CHECK(code == 1);
    CHECK(fileNamed(onlyPrivate, "expected.json") == nullptr);
    CHECK(fileNamed(onlyPrivate, "secret.json") != nullptr);
}
