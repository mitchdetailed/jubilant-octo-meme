// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "component_json.hpp"
#include "doc_io.hpp"

#include <esses/base/hash.hpp>
#include <esses/project/workbook_file.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <utility>

namespace esses::project {

namespace {

constexpr detail::DocKind kKind{"workbook", 1};

using detail::json;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;

WorksheetDef readWorksheet(const json& j, const std::string& pointer, const Uuid& workbookId,
                           std::size_t index, bool& derived, std::vector<std::string>& log) {
    ObjectReader r(j, pointer);
    WorksheetDef w;
    w.name = asString(r.required("name"), r.child("name"));
    if (w.name.empty()) {
        violate(r.child("name"), "a worksheet name is not empty");
    }
    if (const json* id = r.optional("id")) {
        w.id = jsonio::asUuid(*id, r.child("id"));
    } else {
        w.id = uuidFromHash(hash128({"worksheet", workbookId.str(), std::to_string(index)}));
        derived = true;
        log.push_back("worksheet '" + w.name + "' had no id; derived " + w.id.str());
    }
    w.linkTo = detail::optString(r, "linkTo");
    if (const json* d = r.optional("domain")) {
        w.domain = detail::enumString(*d, r.child("domain"), {"time", "distance"});
    }
    if (const json* g = r.optional("grid")) {
        const std::string gp = r.child("grid");
        if (!g->is_array() || g->size() != 2) {
            violate(gp, "grid is [columns, rows]");
        }
        for (std::size_t i = 0; i < 2; ++i) {
            const int cells = jsonio::asInt32((*g)[i], jsonio::pointerIndex(gp, i));
            if (cells < 20 || cells > 200) {
                violate(jsonio::pointerIndex(gp, i), "grid cells lie in 20..200 (06 section 4.3 item 2)");
            }
            w.grid[i] = cells;
        }
    }
    if (const json* comps = r.optional("components")) {
        const std::string cp = r.child("components");
        if (!comps->is_array()) {
            violate(cp, "components is an array");
        }
        std::set<std::string, std::less<>> ids;
        for (std::size_t i = 0; i < comps->size(); ++i) {
            bool componentDerived = false;
            const Uuid fallback = uuidFromHash(hash128({"component", w.id.str(), std::to_string(i)}));
            ComponentSpec c = detail::componentFromJson((*comps)[i], jsonio::pointerIndex(cp, i), fallback,
                                                        &componentDerived);
            if (componentDerived) {
                derived = true;
                log.push_back("component " + std::to_string(i) + " of worksheet '" + w.name +
                              "' had no id; derived " + c.id.str());
            }
            if (!ids.insert(c.id.str()).second) {
                violate(jsonio::pointerIndex(cp, i), "component id " + c.id.str() + " is used twice");
            }
            w.components.push_back(std::move(c));
        }
    }
    if (const json* h = r.optional("hotkey")) {
        w.hotkey = asString(*h, r.child("hotkey"));
    }
    r.closed();
    return w;
}

json worksheetToJson(const WorksheetDef& w) {
    json j = json::object();
    j["id"] = w.id.str();
    j["name"] = w.name;
    j["linkTo"] = w.linkTo;
    j["domain"] = w.domain;
    j["grid"] = json::array({w.grid[0], w.grid[1]});
    json comps = json::array();
    for (const ComponentSpec& c : w.components) {
        comps.push_back(detail::componentToJson(c));
    }
    j["components"] = std::move(comps);
    if (w.hotkey) {
        j["hotkey"] = *w.hotkey;
    }
    return j;
}

json workbookToJson(const WorkbookFile& wb) {
    json root = detail::newDocument("workbook", std::max(wb.schemaVersion, kKind.current));
    root["id"] = wb.id.str();
    root["name"] = wb.name;
    root["profile"] = wb.profile;
    if (wb.activeWorksheet) {
        root["activeWorksheet"] = *wb.activeWorksheet;
    }
    json sheets = json::array();
    for (const WorksheetDef& w : wb.worksheets) {
        sheets.push_back(worksheetToJson(w));
    }
    root["worksheets"] = std::move(sheets);
    if (!wb.xI2.compact().empty()) {
        root["x-i2"] = jsonio::fromDoc(wb.xI2);
    }
    detail::putUnknown(root, wb.unknown);
    return root;
}

std::string stemOf(std::string_view fileName) {
    constexpr std::string_view kSuffix = ".workbook.json";
    if (fileName.size() > kSuffix.size() && fileName.ends_with(kSuffix)) {
        fileName.remove_suffix(kSuffix.size());
    } else if (fileName.ends_with(".json")) {
        fileName.remove_suffix(5);
    }
    return std::string(fileName);
}

} // namespace

Result<WorkbookFile> WorkbookFile::parse(std::string_view json, std::string_view fileName,
                                         std::vector<std::string>& log) {
    return detail::readDocument<WorkbookFile>(
        json, kKind, log,
        [&](const detail::json& root, const detail::DocVersion& version) -> Result<WorkbookFile> {
            ObjectReader r(root, "");
            detail::markSchemaHeader(r);
            WorkbookFile wb;
            wb.schemaVersion = version.write;
            if (const detail::json* idJson = r.optional("id")) {
                wb.id = jsonio::asUuid(*idJson, r.child("id"));
            } else {
                wb.id = uuidFromHash(hash128({"workbook", fileName}));
                wb.dirty = true;
                log.push_back("workbook '" + std::string(fileName) + "' had no id; derived " + wb.id.str() +
                              " from the file name");
            }
            if (const detail::json* n = r.optional("name")) {
                wb.name = asString(*n, r.child("name"));
            } else {
                wb.name = stemOf(fileName);
                wb.dirty = true;
                log.push_back("workbook '" + std::string(fileName) + "' had no name; named '" + wb.name +
                              "'");
            }
            wb.profile = detail::optString(r, "profile", "circuit");
            if (const detail::json* a = r.optional("activeWorksheet")) {
                wb.activeWorksheet = asString(*a, r.child("activeWorksheet"));
            }
            const detail::json& sheets = r.required("worksheets");
            const std::string sp = r.child("worksheets");
            if (!sheets.is_array() || sheets.empty()) {
                violate(sp, "worksheets is a non-empty array");
            }
            std::set<std::string, std::less<>> names;
            for (std::size_t i = 0; i < sheets.size(); ++i) {
                bool derived = false;
                WorksheetDef w =
                    readWorksheet(sheets[i], jsonio::pointerIndex(sp, i), wb.id, i, derived, log);
                wb.dirty = wb.dirty || derived;
                if (!names.insert(w.name).second) {
                    violate(jsonio::pointerIndex(sp, i), "worksheet name '" + w.name + "' is used twice");
                }
                wb.worksheets.push_back(std::move(w));
            }
            if (wb.activeWorksheet && names.find(*wb.activeWorksheet) == names.end()) {
                log.push_back("activeWorksheet '" + *wb.activeWorksheet +
                              "' names no worksheet; ignored on open");
            }
            if (const detail::json* x = r.optional("x-i2")) {
                wb.xI2 = detail::objectDoc(*x, r.child("x-i2"));
            }
            wb.unknown = detail::unknownMembers(r);
            return wb;
        },
        workbookToJson);
}

Result<WorkbookFile> WorkbookFile::load(const std::filesystem::path& path, std::vector<std::string>& log) {
    const std::string fileName = detail::fileNameUtf8(path);
    return detail::loadDocument<WorkbookFile>(
        path, [&](std::string_view text) { return WorkbookFile::parse(text, fileName, log); });
}

std::string WorkbookFile::toJson() const {
    return detail::canonicalOrEmpty(workbookToJson(*this));
}

Result<void> WorkbookFile::save(const std::filesystem::path& path) const {
    return detail::saveCanonical(workbookToJson(*this), path);
}

const WorksheetDef* WorkbookFile::worksheet(std::string_view sheetName) const noexcept {
    for (const WorksheetDef& w : worksheets) {
        if (w.name == sheetName) {
            return &w;
        }
    }
    return nullptr;
}

} // namespace esses::project
