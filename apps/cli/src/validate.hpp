// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses-cli validate (05 section 9.2, 08 section 5, 02 section 6.6 rule 5): checks JSON documents against the
// schema their "$schema" member names (urn:esses:<name>-<major>, the bare esses: form or the https form,
// D-072), with the vendored json-schema-validator, after steps 1-2 of the loader without writing anything: a
// major newer than this build's schemas is refused, and an older minor is migrated through esses_project's
// Migrations first (each step is reported). Errors are reported with their JSON pointer. A document whose
// "$schema" is the draft-07 meta-schema is itself a schema and must load as one. Directories are walked
// recursively for *.json; there a file without "$schema" is skipped, while a file named explicitly must have
// one. --all validates data/, fixtures/, tests/maths/ and the schemas/*.schema.json files under --root (08
// section 5) and leaves out the git-ignored private checkouts (fixtures/private, tests/maths/user_modules,
// tests/maths/oracle).
#pragma once

#include "app_context.hpp"

#include <esses/base/result.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nlohmann::json_schema {
class json_validator;
} // namespace nlohmann::json_schema

namespace esses::cli {

struct SchemaError {
    std::string pointer; // JSON pointer of the instance location ("" = the root)
    std::string message;
};

class SchemaStore {
public:
    explicit SchemaStore(std::filesystem::path dir);
    ~SchemaStore();
    SchemaStore(const SchemaStore&) = delete;
    SchemaStore& operator=(const SchemaStore&) = delete;

    // "urn:esses:probe-1", "esses:probe-1", "https://host/x/probe-1.schema.json" -> "probe-1".
    static std::optional<std::string> schemaName(std::string_view schemaUri);
    static bool isMetaSchema(std::string_view schemaUri);

    // Errors of `doc` against <dir>/<name>.schema.json (empty = valid). NotFound when there is no such
    // schema, Format when it does not load.
    Result<std::vector<SchemaError>> validate(const nlohmann::json& doc, const std::string& name);
    // Validates against the schema its "$schema" names (or loads it as a schema for the meta-schema).
    // Invalid when "$schema" is missing or unknown.
    Result<std::vector<SchemaError>> validateDocument(const nlohmann::json& doc,
                                                      std::string* schemaUsed = nullptr);

private:
    Result<nlohmann::json_schema::json_validator*> validator(const std::string& name);
    std::filesystem::path dir_;
    std::map<std::string, std::unique_ptr<nlohmann::json_schema::json_validator>> cache_;
};

struct ValidateOptions {
    std::vector<std::filesystem::path> paths;
    std::optional<std::filesystem::path> schemaFile; // --schema: validate every file against this one
    bool json = false;
    bool verbose = false;       // list valid files too
    bool all = false;           // --all: data/, fixtures/, tests/maths/, schemas/*.schema.json
    std::filesystem::path root; // --root (default: the current directory)
    std::vector<std::filesystem::path> exclude; // --exclude: directories left out of a walk
};

// What --all validates under `root` (the directories that exist, then the schema files) and the private
// checkouts it leaves out.
std::vector<std::filesystem::path> allValidatePaths(const std::filesystem::path& root);
std::vector<std::filesystem::path> allValidateExclusions(const std::filesystem::path& root);

int runValidate(AppContext& ctx, const ValidateOptions& options, std::ostream& out, std::ostream& err);

} // namespace esses::cli
