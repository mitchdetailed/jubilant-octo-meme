// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "validate.hpp"

#include "cli_common.hpp"

#include <nlohmann/json-schema.hpp>

#include <algorithm>
#include <ostream>
#include <regex>
#include <system_error>

namespace esses::cli {
namespace {

using nlohmann::json;

class Collect : public nlohmann::json_schema::error_handler {
public:
    void error(const json::json_pointer& ptr, const json& /*instance*/, const std::string& message) override {
        errors.push_back({ptr.to_string(), message});
    }
    std::vector<SchemaError> errors;
};

Result<std::unique_ptr<nlohmann::json_schema::json_validator>> makeValidator(const json& schema) {
    try {
        auto v = std::make_unique<nlohmann::json_schema::json_validator>(
            nullptr, nlohmann::json_schema::default_string_format_check);
        v->set_root_schema(schema);
        return v;
    } catch (const std::exception& e) {
        return fail(ErrorCode::Format, std::string("not a valid draft-07 schema: ") + e.what());
    }
}

} // namespace

SchemaStore::SchemaStore(std::filesystem::path dir) : dir_(std::move(dir)) {}
SchemaStore::~SchemaStore() = default;

std::optional<std::string> SchemaStore::schemaName(std::string_view uri) {
    static const std::regex kUrn("^(?:urn:esses:|esses:)([a-z][a-z0-9-]*-[1-9][0-9]*)$");
    static const std::regex kHttps(
        R"(^https://[^/\s]+(?:/[^/\s]+)*/([a-z][a-z0-9-]*-[1-9][0-9]*)(?:\.schema)?\.json$)");
    std::match_results<std::string_view::const_iterator> m;
    if (std::regex_match(uri.begin(), uri.end(), m, kUrn) ||
        std::regex_match(uri.begin(), uri.end(), m, kHttps)) {
        return m[1].str();
    }
    return std::nullopt;
}

bool SchemaStore::isMetaSchema(std::string_view uri) {
    return uri == "http://json-schema.org/draft-07/schema#" ||
           uri == "http://json-schema.org/draft-07/schema";
}

Result<nlohmann::json_schema::json_validator*> SchemaStore::validator(const std::string& name) {
    if (auto it = cache_.find(name); it != cache_.end()) {
        return it->second.get();
    }
    const std::filesystem::path file = dir_ / p8(name + ".schema.json");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        ErrorContext c;
        c.schemaId = name;
        c.path = file;
        return fail(ErrorCode::NotFound, "no schema " + name + " (looked for " + u8(file) + ")",
                    std::move(c));
    }
    auto schema = loadJson(file);
    if (!schema) {
        return Failure{schema.error()};
    }
    auto v = makeValidator(*schema);
    if (!v) {
        Error e = v.error();
        e.message = u8(file) + ": " + e.message;
        return Failure{std::move(e)};
    }
    auto* raw = v->get();
    cache_.emplace(name, std::move(v).value());
    return raw;
}

Result<std::vector<SchemaError>> SchemaStore::validate(const json& doc, const std::string& name) {
    auto v = validator(name);
    if (!v) {
        return Failure{v.error()};
    }
    Collect errors;
    try {
        (*v)->validate(doc, errors);
    } catch (const std::exception& e) {
        errors.errors.push_back({"", std::string("validator error: ") + e.what()});
    }
    return std::move(errors.errors);
}

Result<std::vector<SchemaError>> SchemaStore::validateDocument(const json& doc, std::string* schemaUsed) {
    if (!doc.is_object() || !doc.contains("$schema") || !doc["$schema"].is_string()) {
        return fail(ErrorCode::Invalid, "the document has no \"$schema\" member");
    }
    const std::string uri = doc["$schema"].get<std::string>();
    if (isMetaSchema(uri)) {
        if (schemaUsed != nullptr) {
            *schemaUsed = "draft-07";
        }
        auto v = makeValidator(doc);
        if (!v) {
            return std::vector<SchemaError>{{"", v.error().message}};
        }
        return std::vector<SchemaError>{};
    }
    const auto name = schemaName(uri);
    if (!name) {
        return fail(ErrorCode::Invalid, "\"$schema\" is not an Esses schema id: " + uri);
    }
    if (schemaUsed != nullptr) {
        *schemaUsed = *name;
    }
    return validate(doc, *name);
}

namespace {

struct FileOutcome {
    std::filesystem::path path;
    std::string schema;
    enum class Status : std::uint8_t { Valid, Invalid, Skipped, Error } status = Status::Valid;
    std::vector<SchemaError> errors;
};

std::string_view statusName(FileOutcome::Status s) {
    switch (s) {
    case FileOutcome::Status::Valid:
        return "valid";
    case FileOutcome::Status::Invalid:
        return "invalid";
    case FileOutcome::Status::Skipped:
        return "skipped";
    case FileOutcome::Status::Error:
        break;
    }
    return "error";
}

void collectFiles(const std::filesystem::path& dir, std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    std::vector<std::filesystem::path> entries;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end());
    for (const auto& p : entries) {
        const std::string name = u8(p.filename());
        if (!name.empty() && name.front() == '.') {
            continue;
        }
        std::error_code e2;
        if (std::filesystem::is_directory(p, e2)) {
            collectFiles(p, out);
        } else if (p.extension() == ".json") {
            out.push_back(p);
        }
    }
}

} // namespace

int runValidate(AppContext& ctx, const ValidateOptions& o, std::ostream& out, std::ostream& err) {
    std::optional<SchemaStore> store;
    std::optional<std::unique_ptr<nlohmann::json_schema::json_validator>> fixed;
    if (o.schemaFile) {
        auto schema = loadJson(*o.schemaFile);
        if (!schema) {
            err << "esses-cli: " << schema.error().toString() << "\n";
            return 2;
        }
        auto v = makeValidator(*schema);
        if (!v) {
            err << "esses-cli: " << u8(*o.schemaFile) << ": " << v.error().message << "\n";
            return 2;
        }
        fixed = std::move(v).value();
    } else {
        auto dir = ctx.schemaDir();
        if (!dir) {
            err << "esses-cli: " << dir.error().toString() << "\n";
            return 2;
        }
        store.emplace(*dir);
    }

    std::vector<FileOutcome> results;
    bool ioError = false;
    for (const auto& path : o.paths) {
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        const bool walked = std::filesystem::is_directory(path, ec);
        if (walked) {
            collectFiles(path, files);
        } else {
            files.push_back(path);
        }
        for (const auto& f : files) {
            FileOutcome r;
            r.path = f;
            auto doc = loadJson(f);
            if (!doc) {
                // Unreadable is an error of the run; unparsable is an invalid document.
                const bool io = doc.error().code == ErrorCode::Io;
                r.status = io ? FileOutcome::Status::Error : FileOutcome::Status::Invalid;
                r.errors.push_back({"", doc.error().message});
                ioError = ioError || io;
                results.push_back(std::move(r));
                continue;
            }
            Result<std::vector<SchemaError>> errs = std::vector<SchemaError>{};
            if (fixed) {
                Collect c;
                try {
                    (*fixed)->validate(*doc, c);
                } catch (const std::exception& e) {
                    c.errors.push_back({"", std::string("validator error: ") + e.what()});
                }
                errs = std::move(c.errors);
                r.schema = u8(o.schemaFile->filename());
            } else {
                const bool hasSchema = doc->is_object() && doc->contains("$schema");
                if (!hasSchema && walked) {
                    r.status = FileOutcome::Status::Skipped;
                    results.push_back(std::move(r));
                    continue;
                }
                errs = store->validateDocument(*doc, &r.schema);
            }
            if (!errs) {
                r.status = FileOutcome::Status::Invalid;
                r.errors.push_back({"", errs.error().message});
            } else if (!errs->empty()) {
                r.status = FileOutcome::Status::Invalid;
                r.errors = std::move(errs).value();
            }
            results.push_back(std::move(r));
        }
    }

    std::size_t valid = 0, invalid = 0, skipped = 0, errors = 0;
    for (const auto& r : results) {
        switch (r.status) {
        case FileOutcome::Status::Valid:
            ++valid;
            break;
        case FileOutcome::Status::Invalid:
            ++invalid;
            break;
        case FileOutcome::Status::Skipped:
            ++skipped;
            break;
        case FileOutcome::Status::Error:
            ++errors;
            break;
        }
    }
    if (o.json) {
        json doc = json::object();
        json files = json::array();
        for (const auto& r : results) {
            json f = json::object();
            f["path"] = u8(r.path);
            f["status"] = std::string(statusName(r.status));
            if (!r.schema.empty()) {
                f["schema"] = r.schema;
            }
            if (!r.errors.empty()) {
                json es = json::array();
                for (const auto& e : r.errors) {
                    es.push_back(json{{"pointer", e.pointer}, {"message", e.message}});
                }
                f["errors"] = std::move(es);
            }
            files.push_back(std::move(f));
        }
        doc["files"] = std::move(files);
        doc["summary"] =
            json{{"valid", valid}, {"invalid", invalid}, {"skipped", skipped}, {"errors", errors}};
        auto text = canonicalText(doc);
        if (text) {
            out << *text;
        }
    } else {
        for (const auto& r : results) {
            if (r.status == FileOutcome::Status::Valid && !o.verbose) {
                continue;
            }
            out << statusName(r.status) << "  " << u8(r.path);
            if (!r.schema.empty()) {
                out << "  (" << r.schema << ")";
            }
            if (r.status == FileOutcome::Status::Skipped) {
                out << "  no \"$schema\"";
            }
            out << "\n";
            for (const auto& e : r.errors) {
                out << "    at '" << e.pointer << "': " << e.message << "\n";
            }
        }
        out << "validate: " << valid << " valid, " << invalid << " invalid, " << skipped
            << " skipped (no \"$schema\"), " << errors << " unreadable\n";
    }
    if (ioError || errors > 0) {
        return 2;
    }
    return invalid > 0 ? 1 : 0;
}

} // namespace esses::cli
