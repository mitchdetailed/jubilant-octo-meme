// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Migrations (02 section 6.6, D-015, D-019): pure JSON -> JSON steps (schema, fromVersion) -> fromVersion +
// 1, registered per document type and run in sequence by the loaders before validation; a migrated document
// is marked dirty and the original is copied to backup/ before the first overwrite. A step never deletes
// information: members that no longer exist move into "x-esses-legacy". M2 ships the mechanism with one real
// migration, project-1 1 -> 2 (07 section 5.1, D-162: the Data-window filter keys). Steps register before
// the first load (instance() registers the shipped ones); the set is read-only afterwards.
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::project {

class Migrations {
public:
    using Fn = std::function<Result<JsonDoc>(JsonDoc)>;

    // The process-wide set with every registered step of the shipped document kinds (steps register at
    // start-up; read-only afterwards).
    static Migrations& instance();

    // Registers the step (schema, fromVersion) -> fromVersion + 1; a second registration replaces the first.
    void add(std::string_view schema, int fromVersion, Fn fn);
    // Runs the steps from the document's schemaVersion (1 when absent) up to `toVersion`, stamping
    // schemaVersion after each step and appending one line per step to `log`. `schema` is "<name>-<major>"
    // ("project-1"). Errors: Invalid (not an object), Unsupported (a missing step, or a document newer than
    // toVersion; context.schemaId), Internal (a step that returned no object), a step's error.
    Result<JsonDoc> upgrade(JsonDoc doc, std::string_view schema, int toVersion,
                            std::vector<std::string>& log) const;
    // The newest minor the registered steps reach for `schema`: one above its highest step, 1 without steps
    // (esses-cli validate migrates every document to it, 02 section 6.6 rule 5).
    int latest(std::string_view schema) const;

private:
    std::map<std::pair<std::string, int>, Fn, std::less<>> steps_;
};

} // namespace esses::project
