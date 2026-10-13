// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// WorkbookFile and WorksheetDef: workbooks/<name>.workbook.json (schema workbook-1; 02 section 6.4, 04
// section 7.3, 06 section 4, D-019). The shipped default is data/workbooks/circuit-default.workbook.json (07
// section 4.1; esses-render's first argument in the M1 demo). A file without `id` gets
// uuidFromHash(hash128({"workbook", fileName})) on load and is marked dirty, so its perspective key is
// stable; likewise a worksheet without `id` gets uuidFromHash(hash128({"worksheet", workbook id, index})) and
// a component without `id` uuidFromHash(hash128({"component", worksheet id, index})).
//
// Every document type of esses_project offers the Document<T> operations of 02 section 6.1 as static members
// (parse/load, toJson, save): validate -> migrate -> parse on load (SchemaRegistry and Migrations arrive in
// M2), canonical bytes (02 section 6.5 rule 1) written atomically on save, unknown top-level members
// preserved. M1 checks the "$schema" id on load: another schema is Invalid, another major Unsupported
// (context.schemaId), a missing "$schema" is read as version 1 and logged (02 section 6.6 rule 4); "$schema"
// is always written in the urn:esses: form. Members written: id, name, profile, worksheets (id, name, linkTo,
// domain, grid, components, hotkey when set), activeWorksheet when set, x-i2 when not null, then the
// preserved unknown members.
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/base/uuid.hpp>
#include <esses/model/opaque_json.hpp> // compatibility: model::OpaqueJson names JsonDoc
#include <esses/project/component_spec.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::project {

struct WorksheetDef {
    Uuid id;
    std::string name;
    std::string linkTo;          // data view title of the profile: "Laps", "Outings"
    std::string domain = "time"; // "time" | "distance"
    std::array<int, 2> grid{100, 100};
    std::vector<ComponentSpec> components;
    std::optional<std::string> hotkey;
    friend bool operator==(const WorksheetDef&, const WorksheetDef&) = default;
};

struct WorkbookFile {
    Uuid id; // key of the layout-1 perspective (D-019)
    std::string name;
    std::string profile = "circuit";
    std::optional<std::string>
        activeWorksheet; // used only when .local/layout.json has no entry (06 section 4.1)
    std::vector<WorksheetDef> worksheets;
    JsonDoc xI2;     // JSON key "x-i2"
    JsonDoc unknown; // unrecognised top-level members, preserved (02 section 6.1)
    // The minor written on save: the newest this build writes, or the newer minor the document was read with
    // (R-38: never downgraded).
    int schemaVersion = 1;
    bool dirty = false; // set when load() derived an id

    // `fileName` (without directory) derives the id when the document has none.
    static Result<WorkbookFile> parse(std::string_view json, std::string_view fileName,
                                      std::vector<std::string>& log);
    static Result<WorkbookFile> load(const std::filesystem::path& path, std::vector<std::string>& log);
    std::string toJson() const;                                 // canonical text; "" on a non-finite number
    Result<void> save(const std::filesystem::path& path) const; // canonical bytes, atomic (AtomicFile)

    // The worksheet called `name`, else nullptr.
    const WorksheetDef* worksheet(std::string_view name) const noexcept;

    friend bool operator==(const WorkbookFile&, const WorkbookFile&) = default;
};

} // namespace esses::project
