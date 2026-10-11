// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/env.hpp>
#include <esses/base/log.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <vector>

namespace esses {
namespace {

constexpr LogLevel kDefaultLevel = LogLevel::Info;

struct Pattern {
    std::string pattern; // "formats.ld", "formats.*" or "*"
    LogLevel level;
};

struct LogState {
    std::mutex mutex;
    std::vector<std::shared_ptr<ILogSink>> sinks;
    std::vector<Pattern> patterns;
    std::atomic<LogLevel> minEnabled{kDefaultLevel};
};

LogState gLog; // constant-initialised (constexpr constructors only)

// Specificity of `pattern` for `category`: -1 when it does not match; an exact match beats any wildcard and a
// longer wildcard prefix beats a shorter one.
int specificity(std::string_view pattern, std::string_view category) noexcept {
    if (pattern == "*") {
        return 0;
    }
    if (pattern.size() >= 2 && pattern.ends_with(".*")) {
        const std::string_view stem = pattern.substr(0, pattern.size() - 2);
        if (category == stem ||
            (category.starts_with(stem) && category.size() > stem.size() && category[stem.size()] == '.')) {
            return 1 + static_cast<int>(stem.size());
        }
        return -1;
    }
    return pattern == category ? 1'000'000 : -1;
}

LogLevel levelForLocked(std::string_view category) noexcept {
    int best = -1;
    LogLevel level = kDefaultLevel;
    for (const Pattern& p : gLog.patterns) {
        const int s = specificity(p.pattern, category);
        if (s > best) {
            best = s;
            level = p.level;
        }
    }
    return level;
}

void recomputeMinLocked() noexcept {
    LogLevel m = kDefaultLevel;
    bool hasStar = false;
    for (const Pattern& p : gLog.patterns) {
        m = std::min(m, p.level);
        hasStar = hasStar || p.pattern == "*";
    }
    if (hasStar) { // "*" replaces the default, so the default no longer bounds the minimum
        m = LogLevel::Off;
        for (const Pattern& p : gLog.patterns) {
            m = std::min(m, p.level);
        }
    }
    gLog.minEnabled.store(m, std::memory_order_relaxed);
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

std::string_view logLevelName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    case LogLevel::Off:
        return "off";
    }
    return "unknown";
}

std::optional<LogLevel> parseLogLevel(std::string_view text) noexcept {
    std::string lower;
    for (const char c : text) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    for (const LogLevel l : {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warning,
                             LogLevel::Error, LogLevel::Off}) {
        if (lower == logLevelName(l)) {
            return l;
        }
    }
    if (lower == "warn") {
        return LogLevel::Warning;
    }
    return std::nullopt;
}

void StderrSink::log(const LogRecord& record) {
    static std::mutex outputMutex;
    using namespace std::chrono;
    const auto sinceEpoch = duration_cast<milliseconds>(record.time.time_since_epoch()).count();
    const long long msOfDay = ((sinceEpoch % 86'400'000) + 86'400'000) % 86'400'000; // UTC
    const std::string category(record.category);
    std::lock_guard lock(outputMutex);
    std::fprintf(stderr, "%02lld:%02lld:%02lld.%03lld %-7s %s: %s\n", msOfDay / 3'600'000,
                 (msOfDay / 60'000) % 60, (msOfDay / 1'000) % 60, msOfDay % 1'000,
                 std::string(logLevelName(record.level)).c_str(), category.c_str(), record.message.c_str());
}

void Log::addSink(std::shared_ptr<ILogSink> sink) {
    if (!sink) {
        return;
    }
    std::lock_guard lock(gLog.mutex);
    gLog.sinks.push_back(std::move(sink));
}

void Log::removeSink(const std::shared_ptr<ILogSink>& sink) {
    std::lock_guard lock(gLog.mutex);
    gLog.sinks.erase(std::remove(gLog.sinks.begin(), gLog.sinks.end(), sink), gLog.sinks.end());
}

void Log::clearSinks() {
    std::lock_guard lock(gLog.mutex);
    gLog.sinks.clear();
}

void Log::setLevel(std::string_view categoryPattern, LogLevel level) {
    std::lock_guard lock(gLog.mutex);
    const auto it = std::find_if(gLog.patterns.begin(), gLog.patterns.end(),
                                 [&](const Pattern& p) { return p.pattern == categoryPattern; });
    if (it != gLog.patterns.end()) {
        it->level = level;
    } else {
        gLog.patterns.push_back(Pattern{std::string(categoryPattern), level});
    }
    recomputeMinLocked();
}

Result<void> Log::configure(std::string_view spec) {
    std::vector<Pattern> parsed;
    while (!spec.empty()) {
        const std::size_t comma = spec.find(',');
        const std::string_view item = trim(spec.substr(0, comma));
        spec = comma == std::string_view::npos ? std::string_view() : spec.substr(comma + 1);
        if (item.empty()) {
            continue;
        }
        const std::size_t eq = item.find('=');
        const std::string_view pattern =
            eq == std::string_view::npos ? std::string_view("*") : trim(item.substr(0, eq));
        const std::string_view levelText = eq == std::string_view::npos ? item : trim(item.substr(eq + 1));
        const std::optional<LogLevel> level = parseLogLevel(levelText);
        if (!level || pattern.empty()) {
            ErrorContext ctx;
            ctx.detail = std::string(item);
            return fail(ErrorCode::Invalid, "bad log specification item", std::move(ctx));
        }
        parsed.push_back(Pattern{std::string(pattern), *level});
    }
    for (const Pattern& p : parsed) {
        setLevel(p.pattern, p.level);
    }
    return ok();
}

Result<void> Log::configureFromEnvironment() {
    const std::optional<std::string> spec = getEnv(envName("LOG"));
    if (!spec) {
        return ok();
    }
    return configure(*spec);
}

bool Log::enabled(LogLevel level, std::string_view category) noexcept {
    if (level == LogLevel::Off || level < gLog.minEnabled.load(std::memory_order_relaxed)) {
        return false;
    }
    std::lock_guard lock(gLog.mutex);
    return level >= levelForLocked(category);
}

void Log::write(LogLevel level, std::string_view category, std::string message) {
    std::vector<std::shared_ptr<ILogSink>> sinks;
    {
        std::lock_guard lock(gLog.mutex);
        sinks = gLog.sinks;
    }
    if (sinks.empty()) {
        return;
    }
    LogRecord record;
    record.level = level;
    record.category = category;
    record.message = std::move(message);
    record.time = std::chrono::system_clock::now();
    record.thread = std::this_thread::get_id();
    for (const auto& sink : sinks) {
        try {
            sink->log(record);
        } catch (...) { // NOLINT(bugprone-empty-catch): a failing sink must not break the caller
        }
    }
}

void Log::reset() {
    std::lock_guard lock(gLog.mutex);
    gLog.sinks.clear();
    gLog.patterns.clear();
    recomputeMinLocked();
}

} // namespace esses
