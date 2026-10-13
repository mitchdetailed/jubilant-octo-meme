// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SchemaRegistry (02 sections 6.1 and 6.6, D-019, D-072): every schemas/*.schema.json compiled once with the
// vendored json-schema-validator (the validator esses-cli validate has used since M0), keyed by
// "<name>-<major>". Read-only after loading, so concurrent validate() calls need no lock.

#include <esses/base/json_nlohmann.hpp>
#include <esses/base/paths.hpp>
#include <esses/project/schema_registry.hpp>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace esses::project {

namespace {

using nlohmann::json;

constexpr std::string_view kSchemaSuffix = ".schema.json";
constexpr std::string_view kMarkerFile = "project-1.schema.json"; // a directory holding it is a schemas dir

// "<name>-<major>": a lower-case name of [a-z0-9_] runs separated by single '.' or '-', then "-<major>".
bool isSchemaKey(std::string_view key) noexcept {
    const std::size_t dash = key.rfind('-');
    if (dash == std::string_view::npos || dash == 0 || dash + 1 >= key.size()) {
        return false;
    }
    const std::string_view major = key.substr(dash + 1);
    if (major.front() < '1' || major.front() > '9' || major.size() > 6) {
        return false;
    }
    for (const char c : major) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    const std::string_view name = key.substr(0, dash);
    if (name.front() < 'a' || name.front() > 'z') {
        return false;
    }
    char previous = 'a';
    for (const char c : name) {
        const bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        const bool sep = c == '.' || c == '-';
        if (!word && !sep) {
            return false;
        }
        if (sep && (previous == '.' || previous == '-')) {
            return false;
        }
        previous = c;
    }
    return previous != '.' && previous != '-';
}

// "urn:esses:<key>", "esses:<key>", "https://host/.../<key>[.schema].json" or the bare key -> the key.
std::optional<std::string> schemaKey(std::string_view uri) {
    std::string_view key = uri;
    if (key.starts_with("urn:esses:")) {
        key.remove_prefix(10);
    } else if (key.starts_with("esses:")) {
        key.remove_prefix(6);
    } else if (key.starts_with("https://")) {
        const std::string_view rest = key.substr(8);
        const std::size_t slash = rest.find('/');
        if (slash == 0 || slash == std::string_view::npos ||
            rest.find_first_of(" \t\r\n") != std::string_view::npos) {
            return std::nullopt;
        }
        key = rest.substr(rest.rfind('/') + 1);
        if (!key.ends_with(".json")) {
            return std::nullopt;
        }
        key.remove_suffix(5);
        if (key.ends_with(".schema")) {
            key.remove_suffix(7);
        }
    }
    if (!isSchemaKey(key)) {
        return std::nullopt;
    }
    return std::string(key);
}

class CollectErrors : public nlohmann::json_schema::error_handler {
public:
    void error(const json::json_pointer& ptr, const json& /*instance*/, const std::string& message) override {
        errors.emplace_back(ptr.to_string(), message);
    }
    std::vector<std::pair<std::string, std::string>> errors;
};

// "<file> (<why>)", one entry of loadDirectory's Format message.
std::string unusableNote(const std::string& file, const std::string& why) {
    std::string note = file;
    note += " (";
    note += why;
    note += ')';
    return note;
}

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

} // namespace

struct SchemaRegistry::Impl {
    std::map<std::string, std::unique_ptr<nlohmann::json_schema::json_validator>, std::less<>>
        schemas; // "project-1" -> the compiled schema
};

SchemaRegistry::SchemaRegistry() : impl_(std::make_unique<Impl>()) {}
SchemaRegistry::~SchemaRegistry() = default;
SchemaRegistry::SchemaRegistry(SchemaRegistry&&) noexcept = default;
SchemaRegistry& SchemaRegistry::operator=(SchemaRegistry&&) noexcept = default;

SchemaRegistry& SchemaRegistry::instance() {
    static SchemaRegistry registry = [] {
        SchemaRegistry r;
        if (const std::filesystem::path dir = findSchemasDir(); !dir.empty()) {
            (void)r.loadDirectory(dir); // a schema that does not load is reported by esses-cli validate
        }
        return r;
    }();
    return registry;
}

std::filesystem::path SchemaRegistry::findSchemasDir(const std::filesystem::path& dataDir) {
    std::vector<std::filesystem::path> candidates;
    if (!dataDir.empty()) {
        candidates.push_back(dataDir / "schemas");
    }
    if (auto paths = Paths::resolve(); paths && !paths->dataDir().empty()) {
        candidates.push_back(paths->dataDir() / "schemas");
    }
    if (const std::filesystem::path exe = Paths::executableDir(); !exe.empty()) {
        candidates.push_back(exe / "data" / "schemas");
    }
#ifdef ESSES_PROJECT_SOURCE_DIR
    candidates.push_back(std::filesystem::path(ESSES_PROJECT_SOURCE_DIR) / "schemas");
#endif
    const std::filesystem::path marker{std::string(kMarkerFile)};
    for (const std::filesystem::path& dir : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(dir / marker, ec)) {
            return dir;
        }
    }
    return {};
}

Result<void> SchemaRegistry::loadDirectory(const std::filesystem::path& dir) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = utf8(it->path().filename());
        if (name.size() > kSchemaSuffix.size() && name.ends_with(kSchemaSuffix)) {
            files.push_back(it->path());
        }
    }
    if (ec) {
        ErrorContext ctx;
        ctx.path = dir;
        return fail(ErrorCode::Io, "cannot list the schemas directory: " + ec.message(), std::move(ctx));
    }
    std::sort(files.begin(), files.end());
    std::vector<std::string> unusable;
    for (const std::filesystem::path& file : files) {
        const std::string fileName = utf8(file.filename());
        const std::string key = fileName.substr(0, fileName.size() - kSchemaSuffix.size());
        auto text = jsonio::readFile(file);
        if (!text) {
            unusable.push_back(unusableNote(fileName, text.error().message));
            continue;
        }
        auto schema = jsonio::parseJson(*text);
        if (!schema) {
            unusable.push_back(unusableNote(fileName, schema.error().message));
            continue;
        }
        const std::string expectedId = "urn:esses:" + key;
        if (!isSchemaKey(key) || !schema->is_object() || schema->value("$id", std::string()) != expectedId) {
            unusable.push_back(unusableNote(fileName, "\"$id\" is not " + expectedId));
            continue;
        }
        try {
            auto validator = std::make_unique<nlohmann::json_schema::json_validator>(
                nullptr, nlohmann::json_schema::default_string_format_check);
            validator->set_root_schema(*schema);
            impl_->schemas.insert_or_assign(key, std::move(validator));
        } catch (const std::exception& e) {
            unusable.push_back(unusableNote(fileName, e.what()));
        }
    }
    if (!unusable.empty()) {
        std::string list;
        for (const std::string& u : unusable) {
            list += list.empty() ? "" : "; ";
            list += u;
        }
        ErrorContext ctx;
        ctx.path = dir;
        return fail(ErrorCode::Format, "not usable as draft-07 schemas: " + list, std::move(ctx));
    }
    return ok();
}

std::vector<std::string> SchemaRegistry::ids() const {
    std::vector<std::string> out;
    out.reserve(impl_->schemas.size());
    for (const auto& entry : impl_->schemas) {
        out.push_back(entry.first);
    }
    return out;
}

Result<void> SchemaRegistry::validate(const JsonDoc& doc, std::string_view schemaId) const {
    const std::optional<std::string> key = schemaKey(schemaId);
    const auto it = key ? impl_->schemas.find(*key) : impl_->schemas.end();
    if (it == impl_->schemas.end()) {
        ErrorContext ctx;
        ctx.schemaId = std::string(schemaId);
        return fail(ErrorCode::NotFound, "no schema " + std::string(schemaId) + " in the schema registry",
                    std::move(ctx));
    }
    CollectErrors errors;
    try {
        it->second->validate(jsonio::fromDoc(doc), errors);
    } catch (const std::exception& e) {
        errors.errors.emplace_back("", std::string("validator error: ") + e.what());
    }
    if (errors.errors.empty()) {
        return ok();
    }
    std::string message = std::to_string(errors.errors.size()) +
                          (errors.errors.size() == 1 ? " error" : " errors") + " against " + it->first;
    for (std::size_t i = 0; i < errors.errors.size(); ++i) {
        message += (i == 0 ? ": " : "; ");
        message += "at '" + errors.errors[i].first + "': " + errors.errors[i].second;
    }
    ErrorContext ctx;
    ctx.detail = errors.errors.front().first;
    ctx.schemaId = "urn:esses:" + it->first;
    return fail(ErrorCode::Invalid, std::move(message), std::move(ctx));
}

Result<SchemaRef> SchemaRegistry::schemaOf(const JsonDoc& doc) const {
    if (!doc.isObject()) {
        ErrorContext ctx;
        ctx.detail = "";
        return fail(ErrorCode::Invalid, "the document is not a JSON object", std::move(ctx));
    }
    const json root = jsonio::fromDoc(doc);
    const auto s = root.find("$schema");
    if (s == root.end() || !s->is_string()) {
        ErrorContext ctx;
        ctx.detail = "/$schema";
        return fail(ErrorCode::Invalid, "the document has no \"$schema\" string", std::move(ctx));
    }
    const std::string uri = s->get<std::string>();
    const std::optional<std::string> key = schemaKey(uri);
    if (!key) {
        ErrorContext ctx;
        ctx.detail = "/$schema";
        return fail(ErrorCode::Invalid, "\"$schema\" is not an Esses schema id: " + uri, std::move(ctx));
    }
    const std::size_t dash = key->rfind('-');
    SchemaRef ref;
    ref.id = key->substr(0, dash);
    ref.version = std::stoi(key->substr(dash + 1));
    ref.schemaVersion = doc.schemaVersion();
    return ref;
}

} // namespace esses::project
