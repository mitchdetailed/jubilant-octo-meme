// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/env.hpp>
#include <esses/base/log.hpp>

#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

using esses::Log;
using esses::LogLevel;

namespace {

class CaptureSink final : public esses::ILogSink {
public:
    void log(const esses::LogRecord& record) override {
        std::lock_guard lock(mutex_);
        lines_.push_back(std::string(esses::logLevelName(record.level)) + " " + std::string(record.category) +
                         ": " + record.message);
    }
    std::vector<std::string> lines() const {
        std::lock_guard lock(mutex_);
        return lines_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

struct LogReset {
    LogReset() { Log::reset(); }
    ~LogReset() { Log::reset(); }
    LogReset(const LogReset&) = delete;
    LogReset& operator=(const LogReset&) = delete;
};

int gEvaluations = 0;
int expensive() {
    return ++gEvaluations;
}

} // namespace

TEST_CASE("log: default level is info and sinks receive records", "[unit][log]") {
    LogReset reset;
    auto sink = std::make_shared<CaptureSink>();
    Log::addSink(sink);
    ESSES_LOG(Info, "formats.ld", "opened ", 3, " channels");
    ESSES_LOG(Debug, "formats.ld", "not shown");
    ESSES_LOG(Warning, "store", "budget");
    CHECK(sink->lines() ==
          std::vector<std::string>{"info formats.ld: opened 3 channels", "warning store: budget"});
    Log::removeSink(sink);
    ESSES_LOG(Error, "store", "after removal");
    CHECK(sink->lines().size() == 2u);
}

TEST_CASE("log: category patterns, most specific wins", "[unit][log]") {
    LogReset reset;
    REQUIRE(Log::configure("formats.*=debug, store=trace ,*=warning").ok());
    CHECK(Log::enabled(LogLevel::Debug, "formats"));
    CHECK(Log::enabled(LogLevel::Debug, "formats.ld"));
    CHECK_FALSE(Log::enabled(LogLevel::Trace, "formats.ld"));
    CHECK(Log::enabled(LogLevel::Trace, "store"));
    CHECK_FALSE(Log::enabled(LogLevel::Trace, "store.cache"));
    CHECK_FALSE(Log::enabled(LogLevel::Info, "maths.parse"));
    CHECK(Log::enabled(LogLevel::Warning, "maths.parse"));
    CHECK_FALSE(Log::enabled(LogLevel::Debug, "formatsx"));
    Log::setLevel("formats.ld", LogLevel::Error);
    CHECK_FALSE(Log::enabled(LogLevel::Warning, "formats.ld"));
    CHECK(Log::enabled(LogLevel::Debug, "formats.csv"));
    CHECK_FALSE(Log::configure("formats=loud").ok());
    CHECK(Log::configure("debug").ok()); // bare level means "*"
    CHECK(Log::enabled(LogLevel::Debug, "anything"));
    CHECK(esses::parseLogLevel("WARN") == std::optional<LogLevel>(LogLevel::Warning));
}

TEST_CASE("log: arguments are evaluated only when enabled", "[unit][log]") {
    LogReset reset;
    auto sink = std::make_shared<CaptureSink>();
    Log::addSink(sink);
    gEvaluations = 0;
    ESSES_LOG(Debug, "store", "value ", expensive());
    CHECK(gEvaluations == 0);
    ESSES_LOG(Error, "store", "value ", expensive());
    CHECK(gEvaluations == 1);
    ESSES_LOG(Trace, "store", "value ", expensive()); // compiled out with NDEBUG, filtered otherwise
    CHECK(gEvaluations == 1);
}

TEST_CASE("log: environment configuration and concurrent writers", "[unit][log]") {
    LogReset reset;
    const std::string var = esses::envName("LOG");
    CHECK(var == "ESSES_LOG");
    REQUIRE(esses::setEnv(var, std::string("maths.*=debug")));
    REQUIRE(Log::configureFromEnvironment().ok());
    REQUIRE(esses::setEnv(var, std::nullopt));
    CHECK(Log::enabled(LogLevel::Debug, "maths.eval"));
    auto sink = std::make_shared<CaptureSink>();
    Log::addSink(sink);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 100; ++i) {
                ESSES_LOG(Info, "base.pool", "thread ", t, " record ", i);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    CHECK(sink->lines().size() == 400u);
}
