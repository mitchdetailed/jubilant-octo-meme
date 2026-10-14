// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "validate.hpp"

#include "cli_common.hpp"

#include <esses/base/json_nlohmann.hpp>
#include <esses/project/migrations.hpp>

#include <nlohmann/json-schema.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string_view>
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

namespace {

// "<name>-<major>": [a-z][a-z0-9-]* '-' [1-9][0-9]* (the major has no '-', so it follows the last one).
bool isSchemaKey(std::string_view key) {
    const std::size_t dash = key.rfind('-');
    if (dash == std::string_view::npos || dash == 0 || dash + 1 == key.size()) {
        return false;
    }
    const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    const std::string_view name = key.substr(0, dash);
    const std::string_view major = key.substr(dash + 1);
    return lower(name.front()) &&
           std::all_of(name.begin(), name.end(), [&](char c) { return lower(c) || digit(c) || c == '-'; }) &&
           major.front() != '0' && std::all_of(major.begin(), major.end(), digit);
}

} // namespace

// Matched by hand, not with std::regex: its recursive matcher overflowed the stack on a long "$schema" (an
// https form of 100 KB ended the whole process). The language is unchanged:
//   ^(?:urn:esses:|esses:)(KEY)$  and  ^https://[^/\s]+(?:/[^/\s]+)*/(KEY)(?:\.schema)?\.json$
std::optional<std::string> SchemaStore::schemaName(std::string_view uri) {
    std::string_view key;
    if (uri.starts_with("urn:esses:")) {
        key = uri.substr(10);
    } else if (uri.starts_with("esses:")) {
        key = uri.substr(6);
    } else if (uri.starts_with("https://")) {
        const std::string_view rest = uri.substr(8);
        const std::size_t last = rest.rfind('/');
        if (last == std::string_view::npos || rest.front() == '/' ||
            rest.find("//") != std::string_view::npos ||
            rest.find_first_of(" \t\n\v\f\r") != std::string_view::npos) {
            return std::nullopt; // no path, an empty host or segment, or whitespace
        }
        key = rest.substr(last + 1);
        if (!key.ends_with(".json")) {
            return std::nullopt;
        }
        key.remove_suffix(5);
        if (key.ends_with(".schema")) {
            key.remove_suffix(7);
        }
    } else {
        return std::nullopt;
    }
    if (!isSchemaKey(key)) {
        return std::nullopt;
    }
    return std::string(key);
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
    std::vector<std::string> log; // migration steps and notes (02 section 6.6 rule 5)
};

// The newest major below `major` that `dir` has a schema <name>-<major>.schema.json for, 0 when none. The
// directory is listed once, so a huge major costs one listing, not a file probe per number below it.
int newestMajorBelow(const std::filesystem::path& dir, const std::string& name, int major) {
    constexpr std::string_view kSuffix = ".schema.json";
    const std::string prefix = name + "-";
    int newest = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string file = u8(it->path().filename());
        if (file.size() <= prefix.size() + kSuffix.size() || !file.starts_with(prefix) ||
            !file.ends_with(kSuffix)) {
            continue;
        }
        const std::string_view number =
            std::string_view(file).substr(prefix.size(), file.size() - prefix.size() - kSuffix.size());
        int n = 0;
        const std::from_chars_result r = std::from_chars(number.data(), number.data() + number.size(), n);
        std::error_code fileEc;
        if (r.ec == std::errc() && r.ptr == number.data() + number.size() && number.front() != '0' &&
            n >= 1 && n < major && n > newest && it->is_regular_file(fileEc)) {
            newest = n;
        }
    }
    return newest;
}

// Steps 1-2 of 02 section 6.6 rule 2 on an Esses document: a major newer than the schemas of `dir` is
// refused, an older minor is migrated to the newest one the registered steps reach. Returns the errors
// (empty: go on validating `doc`, which may have been replaced by its migrated form).
std::vector<SchemaError> prepareForValidation(json& doc, const std::filesystem::path& dir, FileOutcome& r) {
    if (!doc.is_object() || !doc.contains("$schema") || !doc["$schema"].is_string()) {
        return {};
    }
    const auto key = SchemaStore::schemaName(doc["$schema"].get<std::string>());
    if (!key) {
        return {};
    }
    const std::size_t dash = key->rfind('-');
    const std::string name = key->substr(0, dash);
    // The major is all digits (schemaName's pattern) but need not fit an int; one that does not is newer than
    // any this build reads. Never an exception: one document's "$schema" must not stop the batch.
    const std::string_view digits = std::string_view(*key).substr(dash + 1);
    int major = 0;
    const std::from_chars_result parsed =
        std::from_chars(digits.data(), digits.data() + digits.size(), major);
    if (parsed.ec == std::errc::result_out_of_range) {
        major = std::numeric_limits<int>::max();
    } else if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size() || major < 1) {
        return {{"/$schema", *key + " does not end in a major version number"}};
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(dir / p8(*key + ".schema.json"), ec)) {
        if (const int older = newestMajorBelow(dir, name, major); older > 0) {
            const std::string known = name + "-" + std::to_string(older);
            return {{"/$schema", *key + " is a newer major than this build reads (" + known +
                                     "); the file is not opened (02 section 6.6 rule 2)"}};
        }
        return {}; // reported as an unknown schema by the validator
    }
    const project::Migrations& migrations = project::Migrations::instance();
    const int latest = migrations.latest(*key);
    int version = 1;
    std::string versionText = "1";
    if (const auto v = doc.find("schemaVersion"); v != doc.end() && v->is_number_integer()) {
        // Read without narrowing: a schemaVersion beyond int is newer than any, never wrapped round.
        constexpr int kMax = std::numeric_limits<int>::max();
        constexpr int kMin = std::numeric_limits<int>::min();
        version = v->is_number_unsigned()
                      ? static_cast<int>(std::min<std::uint64_t>(v->get<std::uint64_t>(), kMax))
                      : static_cast<int>(std::clamp<std::int64_t>(v->get<std::int64_t>(), kMin, kMax));
        versionText = v->dump();
    }
    if (version > latest) {
        r.log.push_back(*key + " schemaVersion " + versionText + " is newer than this build's " +
                        std::to_string(latest) + "; validated as it is");
        return {};
    }
    if (version == latest) {
        return {};
    }
    auto asDoc = jsonio::toDoc(doc);
    if (!asDoc) {
        return {{"", asDoc.error().message}};
    }
    auto upgraded = migrations.upgrade(std::move(asDoc).value(), *key, latest, r.log);
    if (!upgraded) {
        return {{upgraded.error().context.detail.value_or(""), upgraded.error().message}};
    }
    doc = jsonio::fromDoc(*upgraded);
    return {};
}

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

std::filesystem::path normalised(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    return (ec ? p : abs).lexically_normal();
}

bool excluded(const std::filesystem::path& dir, const std::vector<std::filesystem::path>& exclude) {
    const std::filesystem::path n = normalised(dir);
    return std::any_of(exclude.begin(), exclude.end(),
                       [&n](const std::filesystem::path& e) { return normalised(e) == n; });
}

void collectFiles(const std::filesystem::path& dir, std::vector<std::filesystem::path>& out,
                  const std::vector<std::filesystem::path>& exclude) {
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
            if (!excluded(p, exclude)) {
                collectFiles(p, out, exclude);
            }
        } else if (p.extension() == ".json") {
            out.push_back(p);
        }
    }
}

} // namespace

std::vector<std::filesystem::path> allValidatePaths(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> out;
    for (const char* dir : {"data", "fixtures", "tests/maths"}) {
        std::error_code ec;
        if (std::filesystem::is_directory(root / dir, ec)) {
            out.push_back(root / dir);
        }
    }
    // The schemas themselves against the draft-07 meta-schema; their examples (half of them deliberately
    // invalid) are tools/check_schemas.py's.
    std::vector<std::filesystem::path> schemas;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root / "schemas", ec), end; !ec && it != end;
         it.increment(ec)) {
        if (u8(it->path().filename()).ends_with(".schema.json")) {
            schemas.push_back(it->path());
        }
    }
    std::sort(schemas.begin(), schemas.end());
    out.insert(out.end(), schemas.begin(), schemas.end());
    return out;
}

std::vector<std::filesystem::path> allValidateExclusions(const std::filesystem::path& root) {
    return {root / "fixtures" / "private", root / "tests" / "maths" / "user_modules",
            root / "tests" / "maths" / "oracle"};
}

int runValidate(AppContext& ctx, const ValidateOptions& o, std::ostream& out, std::ostream& err) {
    std::optional<SchemaStore> store;
    std::filesystem::path schemaDir;
    std::optional<std::unique_ptr<nlohmann::json_schema::json_validator>> fixed;
    std::string fixedName; // the --schema file's name, reported with each result it validated
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
        fixedName = u8(o.schemaFile->filename());
    } else {
        auto dir = ctx.schemaDir();
        if (!dir) {
            err << "esses-cli: " << dir.error().toString() << "\n";
            return 2;
        }
        schemaDir = *dir;
        store.emplace(*dir);
    }
    std::vector<std::filesystem::path> paths = o.paths;
    std::vector<std::filesystem::path> exclude = o.exclude;
    if (o.all) {
        const std::filesystem::path root = o.root.empty() ? std::filesystem::path(".") : o.root;
        for (const auto& p : allValidatePaths(root)) {
            paths.push_back(p);
        }
        for (const auto& p : allValidateExclusions(root)) {
            exclude.push_back(p);
        }
        if (paths.empty()) {
            err << "esses-cli: validate --all: none of data/, fixtures/, tests/maths/, schemas/ under "
                << u8(root) << "\n";
            return 2;
        }
    }

    std::vector<FileOutcome> results;
    bool ioError = false;
    for (const auto& path : paths) {
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        const bool walked = std::filesystem::is_directory(path, ec);
        if (walked) {
            collectFiles(path, files, exclude);
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
                r.schema = fixedName;
            } else {
                const bool hasSchema = doc->is_object() && doc->contains("$schema");
                if (!hasSchema && walked) {
                    r.status = FileOutcome::Status::Skipped;
                    results.push_back(std::move(r));
                    continue;
                }
                if (auto refused = prepareForValidation(*doc, schemaDir, r); !refused.empty()) {
                    r.status = FileOutcome::Status::Invalid;
                    r.errors = std::move(refused);
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
            if (!r.log.empty()) {
                f["log"] = r.log;
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
            if (r.status == FileOutcome::Status::Valid && !o.verbose && r.log.empty()) {
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
            for (const auto& line : r.log) {
                out << "    " << line << "\n";
            }
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
