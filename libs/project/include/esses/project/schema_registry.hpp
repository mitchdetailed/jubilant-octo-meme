// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SchemaRegistry (02 sections 6.1 and 6.6, D-019, D-072): the JSON schemas of every persisted document kind,
// loaded from the schemas directory (schemas/*.schema.json in the source tree, <exe dir>/data/schemas next to
// the executables). validate() checks a document against `<name>-<major>`; schemaOf() reads "$schema" and
// "schemaVersion" in the urn:esses:, esses: and https forms (normalised to urn:esses: on save). esses-cli
// validate, the document loaders (validate -> migrate -> parse) and MathModuleIo's callers use it.
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace esses::project {

struct SchemaRef {         // "urn:esses:sidecar-1" -> {"sidecar", 1}
    std::string id;        // the schema name without the major ("sidecar", "builtin-esses.gps_corrections")
    int version = 0;       // the major (the "-1" of the id)
    int schemaVersion = 0; // the document's minor ("schemaVersion"; 0 when absent)
    friend bool operator==(const SchemaRef&, const SchemaRef&) = default;
};

class SchemaRegistry {
public:
    SchemaRegistry();
    ~SchemaRegistry();
    SchemaRegistry(const SchemaRegistry&) = delete;
    SchemaRegistry& operator=(const SchemaRegistry&) = delete;
    SchemaRegistry(SchemaRegistry&&) noexcept;
    SchemaRegistry& operator=(SchemaRegistry&&) noexcept;

    // The process-wide registry over findSchemasDir() (loaded on first use; empty when no directory exists).
    static SchemaRegistry& instance();
    // <dataDir>/schemas, then <exe dir>/data/schemas, then the source tree's schemas/; empty when none
    // exists.
    static std::filesystem::path findSchemasDir(const std::filesystem::path& dataDir = {});

    // Loads every *.schema.json of `dir`. Errors: Io, Format (a file that is not a usable schema).
    Result<void> loadDirectory(const std::filesystem::path& dir);
    // Ids known to this registry ("sidecar-1", "maths-1", "builtin-esses.corrected_speed_distance-1", ...).
    std::vector<std::string> ids() const;

    // `schemaId` in any accepted form ("project-1", "urn:esses:project-1", "esses:project-1", the https
    // form). Errors: NotFound (schema id unknown; context.schemaId), Invalid (context.detail = JSON pointer
    // of the first error, context.schemaId, the message lists every error with its pointer).
    Result<void> validate(const JsonDoc& doc, std::string_view schemaId) const;
    // From "$schema" + "schemaVersion". Errors: Invalid (not an object, missing or malformed "$schema";
    // context.detail = "/$schema").
    Result<SchemaRef> schemaOf(const JsonDoc& doc) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace esses::project
