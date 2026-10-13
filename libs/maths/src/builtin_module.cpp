// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/maths/builtin_module.hpp>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <exception>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace esses::maths {

namespace {

using nlohmann::json;

class CollectErrors final : public nlohmann::json_schema::error_handler {
public:
    void error(const json::json_pointer& ptr, const json& /*instance*/, const std::string& message) override {
        errors.emplace_back(ptr.to_string(), message);
    }
    std::vector<std::pair<std::string, std::string>> errors;
};

// Patterns compile as ECMAScript std::regex, the regex the validator uses (tools/check_schemas.py rule 3).
void checkFormat(const std::string& format, const std::string& value) {
    if (format == "regex") {
        const std::regex compiled(value, std::regex::ECMAScript);
        static_cast<void>(compiled);
    }
}

} // namespace

Result<void> validateSettings(const IBuiltinModule& module, const model::OpaqueJson& settings) {
    const model::OpaqueJson& schemaText = module.settingsSchema();
    ErrorContext context;
    context.schemaId = module.settingsSchemaId();
    if (schemaText.isNull()) {
        return fail(ErrorCode::NotFound, "the settings schema of " + module.id() + " is not loaded",
                    std::move(context));
    }
    nlohmann::json_schema::json_validator validator(nullptr, checkFormat);
    try {
        validator.set_root_schema(json::parse(schemaText.compact()));
    } catch (const std::exception& e) {
        return fail(ErrorCode::Format, std::string("not a usable settings schema: ") + e.what(),
                    std::move(context));
    }
    json instance;
    try {
        instance = settings.isNull() ? json(nullptr) : json::parse(settings.compact());
    } catch (const std::exception& e) {
        return fail(ErrorCode::Format, std::string("settings are not JSON: ") + e.what(), std::move(context));
    }
    CollectErrors errors;
    try {
        validator.validate(instance, errors);
    } catch (const std::exception& e) {
        errors.errors.emplace_back("", std::string("validator error: ") + e.what());
    }
    if (errors.errors.empty()) {
        return ok();
    }
    std::string message = "the settings of " + module.id() + " do not match " + module.settingsSchemaId();
    for (const auto& [ptr, text] : errors.errors) {
        message.append("; at '").append(ptr).append("': ").append(text);
    }
    context.detail = errors.errors.front().first;
    return fail(ErrorCode::Invalid, std::move(message), std::move(context));
}

} // namespace esses::maths
