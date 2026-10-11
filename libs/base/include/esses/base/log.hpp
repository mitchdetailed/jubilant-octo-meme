// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Developer-facing log (01 section 8.1, P-6). Sinks are thread-safe and may be called from any thread.
// Per-category levels: "formats.ld=debug" sets one category, "formats.*=debug" a category and its children,
// "*=warning" everything; the most specific pattern wins; the default level is Info. The environment variable
// <prefix>_LOG (ESSES_LOG, see cmake/naming.cmake) uses the same syntax: "formats.*=debug,store=trace".
//
//   ESSES_LOG(Debug, "formats.ld", "channel ", name, " has ", n, " samples");
// formats lazily (arguments are streamed only when the level is enabled) and compiles Trace records out of
// builds with NDEBUG. The log never contains sample values.
#pragma once

#include <esses/base/config.hpp>
#include <esses/base/result.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace esses {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warning, Error, Off };

std::string_view logLevelName(LogLevel level) noexcept;
std::optional<LogLevel> parseLogLevel(std::string_view text) noexcept; // case-insensitive

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::string_view category;
    std::string message;
    std::chrono::system_clock::time_point time;
    std::thread::id thread;
};

class ILogSink {
public:
    ILogSink() = default;
    ILogSink(const ILogSink&) = delete;
    ILogSink& operator=(const ILogSink&) = delete;
    virtual ~ILogSink() = default;
    virtual void log(const LogRecord& record) = 0;
};

// Writes "HH:MM:SS.mmm LEVEL category: message" lines to stderr (CLI; --verbose raises the level).
class StderrSink final : public ILogSink {
public:
    void log(const LogRecord& record) override;
};

class Log {
public:
    static void addSink(std::shared_ptr<ILogSink> sink);
    static void removeSink(const std::shared_ptr<ILogSink>& sink);
    static void clearSinks();
    static void setLevel(std::string_view categoryPattern, LogLevel level);
    static Result<void> configure(std::string_view spec); // "formats.*=debug,store=trace"
    static Result<void> configureFromEnvironment();       // reads <prefix>_LOG when set
    static bool enabled(LogLevel level, std::string_view category) noexcept;
    static void write(LogLevel level, std::string_view category, std::string message);
    static void reset(); // default levels, no sinks (tests)
};

namespace detail {
template <class... Args> std::string logConcat(Args&&... args) {
    std::ostringstream os;
    (os << ... << std::forward<Args>(args));
    return std::move(os).str();
}
} // namespace detail

} // namespace esses

#define ESSES_LOG_EMIT_(lvl, category, ...)                                                                  \
    do {                                                                                                     \
        if (::esses::Log::enabled((lvl), (category))) {                                                      \
            ::esses::Log::write((lvl), (category), ::esses::detail::logConcat(__VA_ARGS__));                 \
        }                                                                                                    \
    } while (false)

#define ESSES_LOG_LEVEL_Debug_(category, ...) ESSES_LOG_EMIT_(::esses::LogLevel::Debug, category, __VA_ARGS__)
#define ESSES_LOG_LEVEL_Info_(category, ...) ESSES_LOG_EMIT_(::esses::LogLevel::Info, category, __VA_ARGS__)
#define ESSES_LOG_LEVEL_Warning_(category, ...)                                                              \
    ESSES_LOG_EMIT_(::esses::LogLevel::Warning, category, __VA_ARGS__)
#define ESSES_LOG_LEVEL_Error_(category, ...) ESSES_LOG_EMIT_(::esses::LogLevel::Error, category, __VA_ARGS__)
#if defined(NDEBUG)
#define ESSES_LOG_LEVEL_Trace_(category, ...) static_cast<void>(0)
#else
#define ESSES_LOG_LEVEL_Trace_(category, ...) ESSES_LOG_EMIT_(::esses::LogLevel::Trace, category, __VA_ARGS__)
#endif

// level is one of Trace, Debug, Info, Warning, Error (bare name).
#define ESSES_LOG(level, category, ...) ESSES_LOG_LEVEL_##level##_(category, __VA_ARGS__)
