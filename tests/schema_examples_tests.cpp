// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The JSON Schemas under schemas/ (sidecar-1, probe-1, markers-1, fixture-1, units-1; 07 section 3) checked
// with the vendored json-schema-validator, the validator esses-cli validate uses (08 section 5): every schema
// is a valid draft-07 schema whose patterns compile as ECMAScript std::regex, every example under
// schemas/examples/<schema-id>/valid validates, and every example under .../invalid fails with an error at
// the JSON pointer its expect.json entry names. tools/check_schemas.py runs the same examples through its own
// draft-07 subset validator, so the two validators are cross-checked on every example.
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

fs::path schemasDir() {
    return fs::path(ESSES_TEST_SOURCE_DIR).parent_path() / "schemas";
}

json readJson(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return json::parse(text);
}

std::vector<fs::path> sortedFiles(const fs::path& dir, const std::string& suffix) {
    std::vector<fs::path> out;
    if (!fs::is_directory(dir)) {
        return out;
    }
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// schema id ("sidecar-1") -> schema document
std::map<std::string, json> loadSchemas() {
    std::map<std::string, json> out;
    const std::string suffix = ".schema.json";
    for (const auto& path : sortedFiles(schemasDir(), suffix)) {
        const std::string name = path.filename().string();
        out.emplace(name.substr(0, name.size() - suffix.size()), readJson(path));
    }
    return out;
}

class CollectingErrors : public nlohmann::json_schema::error_handler {
public:
    void error(const json::json_pointer& ptr, const json&, const std::string& message) override {
        errors.emplace_back(ptr.to_string(), message);
    }
    std::string describe() const {
        std::ostringstream os;
        for (const auto& [ptr, message] : errors) {
            os << "  at '" << ptr << "': " << message << "\n";
        }
        return os.str();
    }
    std::vector<std::pair<std::string, std::string>> errors;
};

// The draft-07 meta-schema uses "format": "regex" for patterns; compile them as the validator will.
void checkFormat(const std::string& format, const std::string& value) {
    if (format == "regex") {
        const std::regex compiled(value, std::regex::ECMAScript);
        static_cast<void>(compiled);
    }
}

} // namespace

TEST_CASE("schemas: the M0 schemas exist and are valid draft-07 schemas", "[unit]") {
    const auto schemas = loadSchemas();
    for (const char* id : {"sidecar-1", "probe-1", "markers-1", "fixture-1", "units-1"}) {
        INFO("schema " << id);
        CHECK(schemas.count(id) == 1);
    }
    nlohmann::json_schema::json_validator meta(nullptr, checkFormat);
    meta.set_root_schema(nlohmann::json_schema::draft7_schema_builtin);
    for (const auto& [id, schema] : schemas) {
        CollectingErrors errors;
        meta.validate(schema, errors);
        INFO("schema " << id << "\n" << errors.describe());
        CHECK(errors.errors.empty());
        CHECK(schema.value("$id", std::string()) == "urn:esses:" + id);
        nlohmann::json_schema::json_validator validator;
        CHECK_NOTHROW(validator.set_root_schema(schema));
    }
}

TEST_CASE("schemas: valid examples validate", "[unit]") {
    std::size_t checked = 0;
    for (const auto& [id, schema] : loadSchemas()) {
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(schema);
        const auto files = sortedFiles(schemasDir() / "examples" / id / "valid", ".json");
        INFO("schema " << id);
        CHECK_FALSE(files.empty());
        for (const auto& path : files) {
            CollectingErrors errors;
            validator.validate(readJson(path), errors);
            INFO(path.filename().string() << "\n" << errors.describe());
            CHECK(errors.errors.empty());
            ++checked;
        }
    }
    CHECK(checked > 0);
}

TEST_CASE("schemas: invalid examples fail at the expected JSON pointer", "[unit]") {
    std::size_t checked = 0;
    for (const auto& [id, schema] : loadSchemas()) {
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(schema);
        const auto expect = readJson(schemasDir() / "examples" / id / "expect.json");
        const auto files = sortedFiles(schemasDir() / "examples" / id / "invalid", ".json");
        INFO("schema " << id);
        CHECK_FALSE(files.empty());
        for (const auto& path : files) {
            const std::string name = path.filename().string();
            INFO(name);
            REQUIRE(expect.contains(name));
            const std::string pointer = expect.at(name).at("pointer").get<std::string>();
            CollectingErrors errors;
            validator.validate(readJson(path), errors);
            INFO("expected an error at '" << pointer << "'\n" << errors.describe());
            CHECK(std::any_of(errors.errors.begin(), errors.errors.end(),
                              [&](const auto& error) { return error.first == pointer; }));
            ++checked;
        }
        CHECK(expect.size() == files.size());
    }
    CHECK(checked > 0);
}
