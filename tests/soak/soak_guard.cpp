// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "soak_guard.hpp"

#include <esses/base/assert.hpp>

#include <QRegularExpression>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

namespace esses::soak {
namespace {

std::atomic<Guard::Mode> gMode{Guard::Mode::Fatal};
std::atomic<int> gCount{0};
std::mutex gMutex;
std::vector<std::string> gRecorded;   // guarded by gMutex
std::function<void()> gHook;          // guarded by gMutex
QtMessageHandler gPrevious = nullptr; // the handler before ours (Qt Test's log)
std::atomic<bool> gInstalled{false};
void* volatile gLeakSink = nullptr; // leakOnPurpose()'s allocation passes through here, then is dropped

void violation(const char* kind, const std::string& text) {
    if (gMode.load() == Guard::Mode::Fatal) {
        std::fprintf(stderr, "soak: fatal %s: %s\n", kind, text.c_str());
        std::fflush(stderr);
        std::abort();
    }
    gCount.fetch_add(1);
    std::function<void()> hook;
    {
        const std::lock_guard lock(gMutex);
        gRecorded.push_back(std::string(kind) + ": " + text);
        hook = gHook;
    }
    if (hook) {
        hook();
    }
}

void assertHandler(const char* expression, const char* file, int line, const char* message) {
    std::string text = std::string(file != nullptr ? file : "?") + ":" + std::to_string(line) + ": " +
                       (expression != nullptr ? expression : "");
    if (message != nullptr) {
        text += std::string(" - ") + message;
    }
    violation(Guard::assertClass(file, message), text);
}

void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& text) {
    if (gPrevious != nullptr) {
        gPrevious(type, context, text);
    }
    if (Guard::isThreadViolationMessage(type, text)) {
        violation("qt-thread", text.toStdString());
    }
}

} // namespace

void Guard::install(Mode mode) {
    gMode.store(mode);
    if (gInstalled.exchange(true)) {
        return;
    }
    quietAbort();
    setAssertHandler(&assertHandler);
    gPrevious = qInstallMessageHandler(&messageHandler);
}

Guard::Mode Guard::mode() {
    return gMode.load();
}

int Guard::violations() {
    return gCount.load();
}

std::vector<std::string> Guard::recorded() {
    const std::lock_guard lock(gMutex);
    return gRecorded;
}

void Guard::reset() {
    const std::lock_guard lock(gMutex);
    gRecorded.clear();
    gCount.store(0);
}

void Guard::setRecordHook(std::function<void()> hook) {
    const std::lock_guard lock(gMutex);
    gHook = std::move(hook);
}

bool Guard::isThreadViolationMessage(QtMsgType type, const QString& text) {
    if (type == QtDebugMsg || type == QtInfoMsg) {
        return false;
    }
    // Qt's thread-affinity diagnostics (qobject.cpp, qbasictimer.cpp, qtimer.cpp, qsocketnotifier.cpp,
    // qpixmap.cpp, qcoreapplication.cpp) and the queued-connection type check: all name the other thread, or
    // the queued argument that cannot cross to it.
    static const QRegularExpression re(
        QStringLiteral("in a different thread|from another thread|from a different thread|"
                       "owned by a different thread|is not the object's thread|"
                       "threads started with QThread|outside the GUI thread|Cannot queue arguments of type"));
    return re.match(text).hasMatch();
}

const char* Guard::assertClass(const char* file, const char* message) {
    const std::string_view f = file != nullptr ? std::string_view(file) : std::string_view();
    const std::string_view m = message != nullptr ? std::string_view(message) : std::string_view();
    const bool dispatcherFile = f.size() >= 14 && f.substr(f.size() - 14) == "dispatcher.cpp";
    return dispatcherFile || m.find("NoBlock") != std::string_view::npos ? "dispatcher" : "assert";
}

bool Guard::enableCrtHeapChecks(int every) {
#if defined(_MSC_VER) && defined(_DEBUG)
    int flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    flags |= _CRTDBG_ALLOC_MEM_DF;
    flags &= 0x0000FFFF; // clear the check frequency, then choose one
    if (every <= 1) {
        flags |= _CRTDBG_CHECK_ALWAYS_DF;
    } else if (every <= 16) {
        flags |= _CRTDBG_CHECK_EVERY_16_DF;
    } else if (every <= 128) {
        flags |= _CRTDBG_CHECK_EVERY_128_DF;
    } else {
        flags |= _CRTDBG_CHECK_EVERY_1024_DF;
    }
    _CrtSetDbgFlag(flags);
    for (const int report : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
    return true;
#else
    static_cast<void>(every);
    return false;
#endif
}

bool Guard::crtHeapConsistent() {
#if defined(_MSC_VER) && defined(_DEBUG)
    return _CrtCheckMemory() != 0;
#else
    return true;
#endif
}

void leakOnPurpose() {
    gLeakSink = new char[4096];
    gLeakSink = nullptr; // the only pointer is gone: a direct leak
}

void Guard::quietAbort() {
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#if defined(_DEBUG)
    for (const int report : {_CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
#endif
#endif
}

} // namespace esses::soak
