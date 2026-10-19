// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The soak's guard (AC-M4-03 "no Dispatcher thread violation"; 01 section 5.3 rules 1, 3 and 4; D-213): what
// counts as a thread violation and what happens on one.
//   * The Dispatcher's own checks are ESSES_ASSERTs: Dispatcher::drain() asserts that it runs on the thread
//   that
//     owns the Dispatcher (the GUI thread), and the GUI thread, marked ThreadRole::NoBlock as in esses,
//     asserts that it never waits for a Future, requires a Column or builds a summary (taskpool.cpp,
//     column.cpp). The guard's handler (setAssertHandler) receives every ESSES_ASSERT of the process on any
//     thread; the ones in dispatcher.cpp or about a NoBlock thread are classed "dispatcher", the rest
//     "assert".
//   * Qt's thread-affinity warnings (a QObject created, parented, timed or sent events from a thread that
//   does not
//     own it; a timer on a thread Qt did not start, such as a TaskPool worker; a pixmap off the GUI thread; a
//     queued argument type that cannot cross threads) reach the guard's message handler, which forwards every
//     message to the previous handler (the Qt Test log) and classes those "qt-thread".
// In Fatal mode (the soak) a violation prints "soak: fatal <class>: <text>" on stderr and aborts at once, so
// the failure carries the stack of the offending thread (ASan with handle_abort=1 prints it; a debugger stops
// there). In Record mode (the guard's own tests) it is counted, kept, and the record hook runs on the thread
// that hit it. Debug-CRT heap checks (MSVC debug builds only, ESSES_SOAK_CRT_CHECK) are switched on here too.
#pragma once

#include <QString>
#include <QtGlobal>
#include <functional>
#include <string>
#include <vector>

namespace esses::soak {

class Guard {
public:
    enum class Mode { Record, Fatal };

    // Installs the assertion handler and the Qt message handler once (later calls only change the mode).
    static void install(Mode mode);
    static Mode mode();
    // Record mode: the violations so far ("<class>: <text>"), oldest first; reset() forgets them.
    static int violations();
    static std::vector<std::string> recorded();
    static void reset();
    // Record mode: runs after each recorded violation, on the thread that hit it (empty: none).
    static void setRecordHook(std::function<void()> hook);

    // The Qt messages the guard treats as thread violations (the texts of Qt's thread-affinity warnings).
    static bool isThreadViolationMessage(QtMsgType type, const QString& text);
    // "dispatcher" or "assert" for a failed ESSES_ASSERT at `file` with `message`.
    static const char* assertClass(const char* file, const char* message);

    // MSVC debug runtime: _CRTDBG_ALLOC_MEM_DF plus a full heap check every `every` allocations (1: every
    // allocation; 16, 128 or 1024 otherwise), reports to stderr instead of a dialog. False where the debug
    // CRT is not used (nothing changes).
    static bool enableCrtHeapChecks(int every);
    // _CrtCheckMemory() under the debug CRT, else true.
    static bool crtHeapConsistent();
    // No dialog on abort() (MSVC runtime); called by install().
    static void quietAbort();
};

// The soak job's check of its own leak pipeline (ESSES_SOAK_SELFTEST=leak, never in a normal run): 4,096
// bytes allocated here, in an esses frame, and dropped, so LeakSanitizer must report them and lsan_check.py
// must count them however the caller got here (the test calls it from a Qt timer's callback).
void leakOnPurpose();

} // namespace esses::soak
