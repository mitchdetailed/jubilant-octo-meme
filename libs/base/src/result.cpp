// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/result.hpp>
#include <esses/base/utf8.hpp>

#include <cstdio>
#include <cstdlib>
#include <ostream>

namespace esses {

std::string_view errorCodeName(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok:
        return "Ok";
    case ErrorCode::Io:
        return "Io";
    case ErrorCode::Format:
        return "Format";
    case ErrorCode::Unsupported:
        return "Unsupported";
    case ErrorCode::NotFound:
        return "NotFound";
    case ErrorCode::Invalid:
        return "Invalid";
    case ErrorCode::Cancelled:
        return "Cancelled";
    case ErrorCode::OutOfBudget:
        return "OutOfBudget";
    case ErrorCode::OutOfMemory:
        return "OutOfMemory";
    case ErrorCode::Cycle:
        return "Cycle";
    case ErrorCode::Parse:
        return "Parse";
    case ErrorCode::Internal:
        return "Internal";
    }
    return "Unknown";
}

std::string Error::toString() const {
    std::string out(errorCodeName(code));
    out += ": ";
    out += message;
    if (context.offset) {
        char buf[32];
        const int n =
            std::snprintf(buf, sizeof buf, " at 0x%llX", static_cast<unsigned long long>(*context.offset));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        }
    }
    std::string details;
    auto add = [&details](std::string_view label, std::string_view value) {
        if (!details.empty()) {
            details += "; ";
        }
        if (!label.empty()) {
            details.append(label).append(" ");
        }
        details += value;
    };
    if (context.path) {
        add("", pathToUtf8(context.path->filename()));
    }
    if (context.channel) {
        add("channel", "'" + *context.channel + "'");
    }
    if (context.layer) {
        add("layer", "'" + *context.layer + "'");
    }
    if (context.importer) {
        add("importer", *context.importer);
    }
    if (context.schemaId) {
        add("schema", *context.schemaId);
    }
    if (context.span) {
        add("span",
            "[" + std::to_string(context.span->begin) + "," + std::to_string(context.span->end) + ")");
    }
    if (context.detail) {
        add("", *context.detail);
    }
    if (!details.empty()) {
        out += " (" + details + ")";
    }
    return out;
}

std::ostream& operator<<(std::ostream& os, const Error& e) {
    return os << e.toString();
}

namespace detail {

void badResultAccess(const Error* error, const char* what) noexcept {
    if (error != nullptr) {
        try {
            const std::string text = error->toString();
            std::fprintf(stderr, "%s: %s\n", what, text.c_str());
        } catch (...) { // NOLINT(bugprone-empty-catch): best effort before aborting
        }
    }
    fatal(what, __FILE__, __LINE__);
}

} // namespace detail
} // namespace esses
