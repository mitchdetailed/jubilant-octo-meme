#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Judge a sanitizer run of the Saturday soak (AC-M4-03; 07 section 7.1 "Soak"; 08 section 6.10; D-213).

The soak job runs tests/soak's executable under ASan + UBSan with LeakSanitizer, its reports in log files
(ASAN_OPTIONS/UBSAN_OPTIONS log_path) and tests/soak/lsan.supp as LeakSanitizer's suppressions. This script then
decides the run:

  * fail on an AddressSanitizer or UndefinedBehaviorSanitizer error (a crash, an ESSES_ASSERT or thread
    violation, which the soak turns into an abort reported by ASan with handle_abort=1); the first such report is
    printed from its rule line to its SUMMARY line, paths as logged, so the job log shows where it happened
    without the soak-logs artefact (a heap-use-after-free report with its four stacks fits in REPORT_LINES lines;
    a longer report is cut there and says how many lines it left out);
  * fail on a failed or missing Qt Test result (--qtest, the -o <file>,txt log: its "Totals:" line);
  * fail on a leak report whose allocation stack has an esses frame: a function in namespace esses (any
    "esses::" in the frame's function), a source file under the checkout's libs/ or apps/ (--source-root), or an
    unsymbolized frame of an esses executable or library (module name esses* or libesses*);
  * a leak report without an esses frame does not count (08 section 4.1); it is listed as a warning, so that its
    library can be added to lsan.supp after review;
  * a non-zero exit status (--exit-status) that none of the above explains (no leak report, no sanitizer error,
    the tests passed) fails too.
The suppression file is checked first (--suppressions): every pattern must belong to a family listed below, the
code of Qt, fontconfig and glib that allocates for itself and never calls back into the application, because
LeakSanitizer drops a leak when any frame of its stack matches, and a pattern that can appear in the stack of an
allocation Esses made would hide an Esses leak.

Usage:
  lsan_check.py --logs <file or dir> [...] [--suppressions lsan.supp] [--qtest result.txt] [--exit-status N]
                [--source-root DIR] [--expect-esses-leak TEXT] [--github]
  lsan_check.py --check-suppressions lsan.supp
  lsan_check.py --self-test [--suppressions lsan.supp]
--expect-esses-leak inverts the leak verdict for the soak job's pipeline check: the run passes only when a
counted leak has a frame containing TEXT (the deliberate leak of ESSES_SOAK_SELFTEST=leak).
Exit status: 0 the run passes, 1 it fails, 2 usage errors.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# ---- the suppression families -----------------------------------------------------------------------------------

# Modules that never call back into the application: suppressible as a whole.
ALLOWED_MODULES = ("libfontconfig.so",)
# Function-name prefixes of fontconfig (it calls no one).
FONTCONFIG_FUNCTION = re.compile(r"^Fc[A-Z][A-Za-z0-9]*$")
# glib's process-lifetime tables: the type system, quarks, interned strings (never the main loop).
ALLOWED_GLIB = (
    "g_type_register_static",
    "g_type_register_fundamental",
    "g_type_add_interface_static",
    "g_type_class_ref",
    "g_type_init_with_debug_flags",
    "g_quark_from_static_string",
    "g_quark_from_string",
    "g_intern_static_string",
    "g_intern_string",
)
# Qt's font database and engines, platform plugin and plugin loader: whole classes ("Class::") or one function.
ALLOWED_QT_CLASSES = (
    "QFontconfigDatabase::",
    "QFontDatabasePrivate::",
    "QFontEngineFT::",
    "QFreetypeFace::",
    "QOffscreenIntegration::",
    "QFactoryLoader::",
    "QLibraryPrivate::",
    "QLibraryStore::",
    "QIconLoader::",
)
ALLOWED_QT_FUNCTIONS = ("QGuiApplicationPrivate::createPlatformIntegration",)

# Frames through which Qt and glib call the application (events, signals, timers, painting, models, the main
# loop, the test harness). No suppression pattern may match one of them (LeakSanitizer matches substrings).
CALLBACK_FRAMES = (
    "QObject::event(QEvent*)",
    "QWidget::event(QEvent*)",
    "QCoreApplication::notifyInternal2(QObject*, QEvent*)",
    "QCoreApplicationPrivate::sendPostedEvents(QObject*, int, QThreadData*)",
    "QApplication::notify(QObject*, QEvent*)",
    "QApplicationPrivate::notify_helper(QObject*, QEvent*)",
    "QMetaObject::activate(QObject*, QMetaObject const*, int, void**)",
    "void doActivate<false>(QObject*, int, void**)",
    "QtPrivate::QSlotObjectBase::call(QObject*, void**)",
    "QSingleShotTimer::timerEvent(QTimerEvent*)",
    "QTimerInfoList::activateTimers()",
    "QEventLoop::exec(QFlags<QEventLoop::ProcessEventsFlag>)",
    "QEventDispatcherGlib::processEvents(QFlags<QEventLoop::ProcessEventsFlag>)",
    "QEventDispatcherUNIX::processEvents(QFlags<QEventLoop::ProcessEventsFlag>)",
    "postEventSourceDispatch(_GSource*, int (*)(void*), void*)",
    "timerSourceDispatch(_GSource*, int (*)(void*), void*)",
    "g_main_context_dispatch",
    "g_main_context_iteration",
    "g_main_dispatch",
    "QWidgetPrivate::drawWidget(QPaintDevice*, QRegion const&, QPoint const&, QFlags<QWidgetPrivate::DrawWidgetFlag>, QPainter*, QWidgetRepaintManager*)",
    "QWidgetPrivate::sendPaintEvent(QRegion const&)",
    "QWidgetRepaintManager::paintAndFlush()",
    "QAbstractItemView::viewportEvent(QEvent*)",
    "QSortFilterProxyModel::data(QModelIndex const&, int) const",
    "QIdentityProxyModel::data(QModelIndex const&, int) const",
    "QTest::qExec(QObject*, int, char**)",
    "QTest::qWait(int)",
    "QTestPrivate::qWaitFor",
    "QThreadPrivate::start(void*)",
    "QFontDatabase::addApplicationFont(QString const&)",
    "esses::qt::SchemeFonts::registerApplicationFonts",
    "main",
)

SUPPRESSION_LINE = re.compile(r"^(?P<type>[a-z_]+):(?P<pattern>.*)$")


def pattern_regex(pattern: str) -> re.Pattern[str]:
    """LeakSanitizer's template match: a substring match with '*' as any text and '^'/'$' anchors."""
    anchored_start = pattern.startswith("^")
    anchored_end = pattern.endswith("$") and not pattern.endswith("\\$")
    body = pattern[1 if anchored_start else 0 : len(pattern) - (1 if anchored_end else 0)]
    rx = ".*".join(re.escape(part) for part in body.split("*"))
    return re.compile(("^" if anchored_start else "") + rx + ("$" if anchored_end else ""))


def suppression_problem(pattern: str) -> str | None:
    """Why `pattern` may not be in lsan.supp, or None when it belongs to an allowed family."""
    stripped = pattern.strip()
    if not stripped or stripped.replace("*", "").strip("^$") == "":
        return "an empty or wildcard-only pattern suppresses everything"
    lowered = stripped.lower()
    for word in ("esses", "tst_", "/libs/", "/apps/", "/tests/", "soak"):
        if word in lowered:
            return f"it names Esses code ({word!r})"
    rx = pattern_regex(stripped)
    for frame in CALLBACK_FRAMES:
        if rx.search(frame):
            return f"it matches {frame!r}, a frame through which the application is called"
    if stripped in ALLOWED_MODULES or FONTCONFIG_FUNCTION.match(stripped):
        return None
    if stripped in ALLOWED_GLIB:
        return None
    if stripped in ALLOWED_QT_CLASSES or stripped in ALLOWED_QT_FUNCTIONS:
        return None
    if any(stripped.startswith(c) and "*" not in stripped for c in ALLOWED_QT_CLASSES):
        return None  # one function of an allowed class
    return "it is not in an allowed family (fontconfig, glib's tables, Qt's font database, engines, platform plugin, plugin loader)"


def check_suppressions(text: str) -> list[str]:
    """Problems of a suppression file's text (empty when every line is allowed)."""
    problems = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = SUPPRESSION_LINE.match(line)
        if m is None:
            problems.append(f"line {number}: not a suppression: {line!r}")
            continue
        if m.group("type") != "leak":
            problems.append(f"line {number}: only leak: suppressions belong here: {line!r}")
            continue
        why = suppression_problem(m.group("pattern"))
        if why is not None:
            problems.append(f"line {number}: {line!r}: {why}")
    return problems


# ---- the sanitizer logs -----------------------------------------------------------------------------------------


@dataclass
class Frame:
    index: int
    function: str  # "" when unsymbolized
    file: str  # "" when unknown
    module: str  # "" when the frame shows a file instead


@dataclass
class Leak:
    kind: str  # "Direct" or "Indirect"
    bytes: int
    objects: int
    frames: list[Frame] = field(default_factory=list)
    log: str = ""


LEAK_HEADER = re.compile(r"^(Direct|Indirect) leak of (\d+) byte\(s\) in (\d+) object\(s\) allocated from:")
FRAME = re.compile(r"^\s*#(\d+)\s+0x[0-9a-fA-F]+\s*(.*)$")
BUILD_ID = re.compile(r"\s*\(BuildId: [0-9a-fA-F]+\)\s*$")
MODULE_TAIL = re.compile(r"^(?P<head>.*?)\s*\((?P<module>[^()]+?)\+0x[0-9a-fA-F]+\)$")
FILE_TAIL = re.compile(r"^(?P<head>.*)\s+(?P<file>\S+?)(?::\d+)?(?::\d+)?$")
ERROR_PATTERNS = (
    re.compile(r"ERROR: AddressSanitizer"),
    re.compile(r"ERROR: UndefinedBehaviorSanitizer"),
    re.compile(r"SUMMARY: UndefinedBehaviorSanitizer"),
    re.compile(r": runtime error: "),
    re.compile(r"ERROR: ThreadSanitizer"),
)
# Where an error report starts (the "=====" rule above it included) and the line that ends it.
REPORT_START = re.compile(r"ERROR: (?:AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer)|: runtime error: ")
REPORT_RULE = re.compile(r"^=+$")
REPORT_END = re.compile(r"^SUMMARY: ")
# How much of the first report the verdict prints: up to REPORT_LINES lines, enough for a whole heap-use-after-free
# report (its use, freed-by, allocated-by and thread-creation stacks: 129 lines in the soak's first one), and of a
# longer line (a frame of a deeply templated function) its head and its tail, where the file and line are.
REPORT_LINES = 200
REPORT_LINE_CHARS = 600


def parse_frame(line: str) -> Frame | None:
    m = FRAME.match(line)
    if m is None:
        return None
    rest = BUILD_ID.sub("", m.group(2)).strip()
    function = ""
    if rest.startswith("in "):
        rest = rest[3:]
        mm = MODULE_TAIL.match(rest)
        if mm is not None:
            return Frame(int(m.group(1)), mm.group("head").strip(), "", mm.group("module"))
        mf = FILE_TAIL.match(rest)
        if mf is not None and ("/" in mf.group("file") or "\\" in mf.group("file")):
            return Frame(int(m.group(1)), mf.group("head").strip(), mf.group("file"), "")
        return Frame(int(m.group(1)), rest, "", "")
    mm = MODULE_TAIL.match(rest)
    if mm is not None:
        return Frame(int(m.group(1)), function, "", mm.group("module"))
    return Frame(int(m.group(1)), function, "", rest.strip("()"))


def parse_log(text: str, name: str = "") -> tuple[list[Leak], list[str]]:
    """The leak reports of a sanitizer log and its error lines (ASan or UBSan errors other than leaks)."""
    leaks: list[Leak] = []
    errors: list[str] = []
    current: Leak | None = None
    for line in text.splitlines():
        header = LEAK_HEADER.match(line.strip())
        if header is not None:
            current = Leak(header.group(1), int(header.group(2)), int(header.group(3)), [], name)
            leaks.append(current)
            continue
        if current is not None:
            frame = parse_frame(line)
            if frame is not None:
                current.frames.append(frame)
                continue
            if line.strip():
                current = None
        if any(p.search(line) for p in ERROR_PATTERNS):
            errors.append(line.strip())
    return leaks, errors


def error_reports(text: str) -> list[list[str]]:
    """The ASan, UBSan and TSan error reports of a log, each from its error line (with the rule above it) to its
    SUMMARY line, else to the next report or the end of the log. LeakSanitizer's report is not one of them."""
    lines = text.splitlines()
    reports: list[list[str]] = []
    i = 0
    while i < len(lines):
        if REPORT_START.search(lines[i]) is None:
            i += 1
            continue
        start = i - 1 if i > 0 and REPORT_RULE.match(lines[i - 1].strip()) else i
        end = i + 1
        while end < len(lines) and REPORT_START.search(lines[end]) is None:
            end += 1
            if REPORT_END.match(lines[end - 1].strip()):
                break
        reports.append([line.rstrip() for line in lines[start:end]])
        i = end
    return reports


def shown_report(name: str, report: list[str]) -> list[str]:
    """The verdict's lines for a report: a heading, then its first REPORT_LINES lines, indented."""
    heading = f"the first sanitizer report ({name}, {len(report)} line(s)"
    heading += f"; the first {REPORT_LINES} shown):" if len(report) > REPORT_LINES else "):"
    out = [heading]
    for line in report[:REPORT_LINES]:
        if len(line) > REPORT_LINE_CHARS:
            keep = REPORT_LINE_CHARS // 2
            line = f"{line[:keep]} [... {len(line) - 2 * keep} characters ...] {line[-keep:]}"
        out.append(f"  | {line}")
    if len(report) > REPORT_LINES:
        out.append(f"  | ... {len(report) - REPORT_LINES} more line(s) in {name}")
    return out


ENTRY_FUNCTIONS = {"main", "qMain", "__libc_start_main", "__libc_start_call_main", "_start"}
ALLOCATORS = ("operator new", "operator delete", "malloc", "calloc", "realloc", "posix_memalign", "aligned_alloc",
              "memalign", "valloc", "strdup", "strndup", "__interceptor_", "___interceptor_", "__asan_", "__sanitizer_")


def first_frame_label(leak: Leak) -> str:
    """Where a leak was allocated: the first frame after the allocator's, as a module or file name."""
    for f in leak.frames:
        if f.function.startswith(ALLOCATORS):
            continue
        if f.module or f.file:
            return (f.module or f.file).replace("\\", "/").rsplit("/", 1)[-1]
        if f.function:
            return f.function
    return "?"


def is_esses_frame(frame: Frame, source_root: str | None) -> bool:
    if "esses::" in frame.function:
        return True
    if frame.file:
        path = frame.file.replace("\\", "/")
        if source_root:
            root = source_root.replace("\\", "/").rstrip("/") + "/"
            if path.startswith(root):
                rel = path[len(root) :]
                return rel.startswith("libs/") or rel.startswith("apps/")
            return False
        return "/libs/" in path or "/apps/" in path
    if frame.module and frame.function.split("(")[0] not in ENTRY_FUNCTIONS:
        base = frame.module.replace("\\", "/").rsplit("/", 1)[-1]
        return not frame.function and (base.startswith("esses") or base.startswith("libesses"))
    return False


def esses_frame_of(leak: Leak, source_root: str | None) -> Frame | None:
    for f in leak.frames:
        if is_esses_frame(f, source_root):
            return f
    return None


QTEST_TOTALS = re.compile(r"Totals:\s+(\d+) passed,\s+(\d+) failed")


def qtest_verdict(text: str) -> tuple[bool, str]:
    m = None
    for m in QTEST_TOTALS.finditer(text):
        pass
    if m is None:
        return False, "no Qt Test totals: the soak did not finish (a crash or an abort)"
    passed, failed = int(m.group(1)), int(m.group(2))
    if failed:
        return False, f"Qt Test: {failed} failed, {passed} passed"
    return True, f"Qt Test: {passed} passed, 0 failed"


@dataclass
class Verdict:
    ok: bool
    lines: list[str]
    counted: list[tuple[Leak, Frame]]
    foreign: list[Leak]
    errors: list[str]
    report: list[str] = field(default_factory=list)  # the first error report, as the verdict prints it


def judge(logs: list[tuple[str, str]], qtest: str | None, exit_status: int | None, source_root: str | None,
          expect_leak: str | None = None) -> Verdict:
    lines: list[str] = []
    ok = True
    counted: list[tuple[Leak, Frame]] = []
    foreign: list[Leak] = []
    errors: list[str] = []
    report: list[str] = []
    for name, text in logs:
        leaks, errs = parse_log(text, name)
        errors.extend(f"{name}: {e}" for e in errs)
        if errs and not report:
            reports = error_reports(text)
            if reports:
                report = shown_report(name, reports[0])
        for leak in leaks:
            frame = esses_frame_of(leak, source_root)
            if frame is not None:
                counted.append((leak, frame))
            else:
                foreign.append(leak)
    if errors:
        ok = False
        lines.append(f"FAIL: {len(errors)} sanitizer error line(s):")
        lines.extend(f"  {e}" for e in errors[:20])
        lines.extend(report)
    if qtest is not None:
        passed, why = qtest_verdict(qtest)
        lines.append(("" if passed else "FAIL: ") + why)
        ok = ok and passed
    if expect_leak is None:
        if counted:
            ok = False
            lines.append(f"FAIL: {len(counted)} leak(s) with an esses frame:")
            for leak, frame in counted[:20]:
                where = frame.function or frame.module
                lines.append(f"  {leak.kind} leak of {leak.bytes} byte(s) in {leak.objects} object(s), at {where}"
                             + (f" ({frame.file})" if frame.file else ""))
        else:
            lines.append("no leak with an esses frame")
    else:
        hit = [c for c in counted if any(expect_leak in (f.function + " " + f.file) for f in c[0].frames)]
        if hit:
            lines.append(f"the deliberate leak was reported and counted ({len(hit)} report(s) at {expect_leak})")
        else:
            ok = False
            lines.append(f"FAIL: no counted leak at {expect_leak}: the leak pipeline would miss an esses leak")
    if foreign:
        libs = sorted({first_frame_label(lk) for lk in foreign})
        lines.append(f"warning: {len(foreign)} leak(s) without an esses frame (not counted, 08 section 4.1), "
                     f"allocated in: {', '.join(libs[:10])}")
    if exit_status not in (None, 0) and ok and not counted and not foreign and expect_leak is None:
        ok = False
        lines.append(f"FAIL: the soak exited with {exit_status} without a leak report or sanitizer error to explain it")
    return Verdict(ok, lines, counted, foreign, errors, report)


def read_logs(paths: list[str]) -> list[tuple[str, str]]:
    out = []
    for p in paths:
        path = Path(p)
        files = sorted(x for x in path.rglob("*") if x.is_file()) if path.is_dir() else [path]
        for f in files:
            try:
                out.append((str(f), f.read_text(encoding="utf-8", errors="replace")))
            except OSError as e:
                out.append((str(f), f"ERROR: AddressSanitizer: (lsan_check could not read the log: {e})"))
    return out


# ---- self-test --------------------------------------------------------------------------------------------------

SELF_LOG = """\
=================================================================
==4711==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 4096 byte(s) in 1 object(s) allocated from:
    #0 0x55d0c1 in operator new[](unsigned long) /src/llvm-project/compiler-rt/lib/asan/asan_new_delete.cpp:89:3
    #1 0x55d1c2 in esses::soak::leakOnPurpose() /work/repo/tests/soak/soak_guard.cpp:180:12
    #2 0x55d1d3 in TstSaturday::selfTestChild()::$_0::operator()() const /work/repo/tests/soak/tst_saturday.cpp:420:17
    #3 0x7f00a1 in QObject::event(QEvent*) (/opt/Qt/6.12.0/gcc_64/lib/libQt6Core.so.6+0x2ab3c4) (BuildId: 0123abcd)
    #4 0x7f00b2 in g_main_context_dispatch (/lib/x86_64-linux-gnu/libglib-2.0.so.0+0x5c8e4)
    #5 0x7f00c3  (/opt/Qt/6.12.0/gcc_64/lib/libQt6Core.so.6+0x2c11aa)

Indirect leak of 32 byte(s) in 1 object(s) allocated from:
    #0 0x55d0c1 in malloc /src/llvm-project/compiler-rt/lib/asan/asan_malloc_linux.cpp:69:3
    #1 0x7f00d4 in QArrayData::allocate(QArrayData**, long long, long long, long long, QArrayData::AllocationOption) (/opt/Qt/6.12.0/gcc_64/lib/libQt6Core.so.6+0x10a3b2)
    #2 0x7f00e5 in QFontEngine::QFontEngine(QFontEngine::Type) (/opt/Qt/6.12.0/gcc_64/lib/libQt6Gui.so.6+0x3a1b2c)

Direct leak of 64 byte(s) in 2 object(s) allocated from:
    #0 0x55d0c1 in operator new(unsigned long) /src/llvm-project/compiler-rt/lib/asan/asan_new_delete.cpp:95:3
    #1 0x55d2a4  (/work/repo/build/linux-clang-soak/bin/esses_soak_tst_saturday+0x4a2b1)
    #2 0x55d2b5 in main /work/repo/tests/soak/tst_saturday.cpp:431:1

Direct leak of 128 byte(s) in 1 object(s) allocated from:
    #0 0x55d0c1 in malloc /src/llvm-project/compiler-rt/lib/asan/asan_malloc_linux.cpp:69:3
    #1 0x7f0101 in FcPatternCreate (/lib/x86_64-linux-gnu/libfontconfig.so.1+0x2a1b3)
    #2 0x7f0112 in QFontconfigDatabase::populateFontDatabase() (/opt/Qt/6.12.0/gcc_64/plugins/platforms/../../lib/libQt6Gui.so.6+0x5e1234)
    #3 0x55d2b5 in main /work/repo/tests/soak/tst_saturday.cpp:431:1

Direct leak of 16 byte(s) in 1 object(s) allocated from:
    #0 0x55d0c1 in operator new(unsigned long) /src/llvm-project/compiler-rt/lib/asan/asan_new_delete.cpp:95:3
    #1 0x55d3c6 in esses::qt::SourceLoader::open(std::filesystem::path const&) /work/repo/libs/qtmodel/src/source_loader.cpp:120:5

SUMMARY: AddressSanitizer: 4336 byte(s) leaked in 6 allocation(s).
"""

SELF_ERRORS = """\
=================================================================
==4712==ERROR: AddressSanitizer: heap-use-after-free on address 0x602000000010 at pc 0x55d1c2 bp 0x7ffc sp 0x7ffc
READ of size 8 at 0x602000000010 thread T0
    #0 0x55d1c2 in esses::qt::DataSelectionModel::closeSource(esses::Hash128 const&) /work/repo/libs/qtmodel/src/data_selection_model.cpp:1009:14
    #1 0x55d1d3 in esses::qt::SourceLoader::close(esses::Hash128 const&) /work/repo/libs/qtmodel/src/source_loader.cpp:813:12

0x602000000010 is located 24 bytes inside of 1200-byte region [0x601ffffffff8,0x6020000004a8)
freed by thread T0 here:
    #0 0x55d0c1 in operator delete(void*) /src/llvm-project/compiler-rt/lib/asan/asan_new_delete.cpp:152:3
    #1 0x55d1e4 in esses::qt::DataSelectionModel::closeSource(esses::Hash128 const&) /work/repo/libs/qtmodel/src/data_selection_model.cpp:1008:14

SUMMARY: AddressSanitizer: heap-use-after-free /work/repo/libs/base/include/esses/base/hash.hpp:31:39 in esses::operator<=>(esses::Hash128 const&, esses::Hash128 const&)
Shadow bytes around the buggy address:
/work/repo/libs/store/src/column.cpp:120:5: runtime error: signed integer overflow
    #0 0x55d2f5 in esses::store::Column::append(double) /work/repo/libs/store/src/column.cpp:120:5
"""


def self_test(suppressions: str | None) -> int:
    failures = []

    def expect(cond: bool, what: str) -> None:
        if not cond:
            failures.append(what)

    # Frames.
    f = parse_frame("    #1 0x55d1c2 in esses::soak::leakOnPurpose() /work/repo/tests/soak/soak_guard.cpp:180:12")
    expect(f is not None and f.function == "esses::soak::leakOnPurpose()" and f.file.endswith("soak_guard.cpp"),
           "a symbolized frame with a file")
    f = parse_frame("    #3 0x7f in QObject::event(QEvent*) (/opt/Qt/lib/libQt6Core.so.6+0x2ab3c4) (BuildId: ab12)")
    expect(f is not None and f.function == "QObject::event(QEvent*)" and f.module.endswith("libQt6Core.so.6"),
           "a symbolized frame with a module and a build id")
    f = parse_frame("    #5 0x7f00c3  (/opt/Qt/lib/libQt6Core.so.6+0x2c11aa)")
    expect(f is not None and f.function == "" and f.module.endswith("libQt6Core.so.6"), "an unsymbolized frame")
    # Leaks: the deliberate one through Qt and glib frames counts, the unsymbolized esses executable frame counts,
    # the library code counts; Qt's own indirect leak and fontconfig's reached from main do not.
    root = "/work/repo"
    v = judge([("asan.4711", SELF_LOG)], "Totals: 6 passed, 0 failed, 1 skipped, 0 blacklisted, 9ms", 1, root)
    expect(not v.ok, "esses leaks fail the run")
    expect(len(v.counted) == 3, f"three leaks counted, got {len(v.counted)}")
    expect(len(v.foreign) == 2, f"two foreign leaks, got {len(v.foreign)}")
    expect(any(fr.function.startswith("esses::soak::leakOnPurpose") for _, fr in v.counted), "the deliberate leak")
    expect(any("SourceLoader::open" in fr.function for _, fr in v.counted), "a library leak")
    expect(any(fr.module.endswith("esses_soak_tst_saturday") for _, fr in v.counted), "an unsymbolized esses frame")
    v = judge([("asan.4711", SELF_LOG)], None, None, root, expect_leak="esses::soak::leakOnPurpose")
    expect(v.ok, "the pipeline check finds the deliberate leak")
    v = judge([("asan.1", SELF_LOG.split("Direct leak of 64")[0])], None, None, root,
              expect_leak="esses::soak::nothing")
    expect(not v.ok, "the pipeline check fails without the deliberate leak")
    # Foreign leaks alone pass with a warning; a non-zero exit then is LeakSanitizer's.
    foreign_only = "\n".join(SELF_LOG.split("\n\n")[0:1] + [SELF_LOG.split("\n\n")[2], SELF_LOG.split("\n\n")[4]])
    v = judge([("asan.2", foreign_only)], "Totals: 6 passed, 0 failed", 1, root)
    expect(v.ok and not v.counted and len(v.foreign) == 2, "foreign leaks only: pass with a warning")
    expect(any(line.startswith("warning:") for line in v.lines), "the foreign leaks are listed")
    # Sanitizer errors and test results.
    v = judge([("asan.3", SELF_ERRORS)], "Totals: 6 passed, 0 failed", 1, root)
    expect(not v.ok and len(v.errors) == 2, "ASan and UBSan errors fail the run")
    # The first report is printed in full (its rule to its SUMMARY line, paths as logged); the second is not.
    expect(len(v.report) == 13 and v.report[0] == "the first sanitizer report (asan.3, 12 line(s)):",
           f"the first report is printed whole, got {v.report[:1]} and {len(v.report)} line(s)")
    expect(v.report[1] == "  | " + "=" * 65 and v.report[-1].startswith("  | SUMMARY: AddressSanitizer: "),
           "the report runs from its rule to its SUMMARY line")
    expect(any("/work/repo/libs/qtmodel/src/data_selection_model.cpp:1008:14" in line for line in v.report),
           "the report keeps its frames and their paths (the freed-by stack included)")
    expect(not any("Column::append" in line for line in v.report), "only the first report is printed")
    expect(all(line in v.lines for line in v.report), "the verdict prints the report")
    reports = error_reports(SELF_ERRORS)
    expect(len(reports) == 2 and reports[1][-1].endswith("column.cpp:120:5"),
           "a UBSan report without a SUMMARY line runs to the end of the log")
    # A heap-use-after-free report of the soak's first one's shape (129 lines: the use stack of 43 frames, the
    # freed-by stack of 33, the allocated-by stack of 26 and the creation stack of the allocating thread of 15) is
    # printed whole, down to its SUMMARY line.
    def stack(header: str, count: int, where: str) -> list[str]:
        return [header] + [f"    #{k} 0x55 in esses::{where}{k}() /work/repo/libs/{where}.cpp:{k + 1}:1"
                           for k in range(count)] + [""]

    uaf_report = "\n".join(["=" * 65, "==2==ERROR: AddressSanitizer: heap-use-after-free on address 0x5170 at pc 0x55"]
                           + stack("READ of size 8 at 0x5170 thread T0", 43, "use")
                           + ["0x5170 is located 24 bytes inside of 688-byte region [0x5170,0x5172)"]
                           + stack("freed by thread T0 here:", 33, "freed")
                           + stack("previously allocated by thread T5 here:", 26, "allocated")
                           + stack("Thread T5 created by T0 here:", 15, "created")
                           + ["SUMMARY: AddressSanitizer: heap-use-after-free /work/repo/libs/use.cpp:1:1 in f()",
                              "Shadow bytes around the buggy address:", "==2==ABORTING"])
    v = judge([("asan.6", uaf_report)], None, None, root)
    expect(not v.ok and v.report[0] == "the first sanitizer report (asan.6, 129 line(s)):" and len(v.report) == 130,
           f"a heap-use-after-free report is printed whole, got {v.report[:1]} and {len(v.report)} line(s)")
    expect(v.report[-1].startswith("  | SUMMARY: AddressSanitizer: heap-use-after-free ")
           and "  | previously allocated by thread T5 here:" in v.report
           and "  |     #14 0x55 in esses::created14() /work/repo/libs/created.cpp:15:1" in v.report,
           "its allocated-by and thread-creation stacks and its SUMMARY line reach the job log")
    # A longer report is cut, and says so.
    frames = REPORT_LINES + 100
    long_report = "\n".join(["==1==ERROR: AddressSanitizer: stack-overflow on address 0x7ffc"]
                            + [f"    #{k} 0x55 in esses::f{k}() /work/repo/libs/x.cpp:{k}:1" for k in range(frames)]
                            + [f"    #{frames} 0x55 in " + "std::vector<int>::" * 60 + "g() /work/repo/libs/y.cpp:7:3",
                               "SUMMARY: AddressSanitizer: stack-overflow /work/repo/libs/x.cpp:1:1 in esses::f0()"])
    v = judge([("asan.4", long_report)], None, None, root)
    expect(not v.ok and len(v.report) == REPORT_LINES + 2, f"a long report is cut, got {len(v.report)} line(s)")
    expect(v.report[0].endswith(f"{frames + 3} line(s); the first {REPORT_LINES} shown):")
           and v.report[-1] == f"  | ... {frames + 3 - REPORT_LINES} more line(s) in asan.4", "the cut is said")
    shown = shown_report("asan.4", error_reports(long_report)[0][-2:])
    expect(len(shown[1]) < REPORT_LINE_CHARS + 50 and shown[1].endswith("g() /work/repo/libs/y.cpp:7:3"),
           "a long frame keeps its head and its file and line")
    expect(not judge([("asan.5", SELF_LOG)], None, None, root).report, "a leak report is not an error report")
    expect(not judge([], "Totals: 5 passed, 1 failed, 0 skipped", 1, root).ok, "a failed test fails the run")
    expect(not judge([], "PASS   : TstSaturday::initTestCase()", 134, root).ok, "no totals (a crash) fails the run")
    expect(judge([], "Totals: 6 passed, 0 failed, 1 skipped", 0, root).ok, "a clean run passes")
    expect(not judge([], "Totals: 6 passed, 0 failed", 3, root).ok, "an unexplained exit status fails the run")
    # Suppression rules.
    good = ["leak:libfontconfig.so", "leak:FcPatternCreate", "leak:g_type_register_static", "leak:QFontEngineFT::",
            "leak:QFontEngineFT::create", "leak:QGuiApplicationPrivate::createPlatformIntegration"]
    bad = ["leak:*", "leak:", "leak:esses", "leak:tst_saturday", "leak:libQt6Core.so", "leak:libglib-2.0.so",
           "leak:g_main_context_dispatch", "leak:QObject::event", "leak:QObject", "leak:QCoreApplication",
           "leak:QTest::", "leak:main", "leak:QFontDatabase::", "called_from_lib:libQt6Core.so",
           "leak:QWidget", "leak:Fc*", "leak:QFontEngineFT::*"]
    for line in good:
        expect(not check_suppressions(line), f"allowed: {line}")
    for line in bad:
        expect(bool(check_suppressions(line)), f"refused: {line}")
    if suppressions:
        problems = check_suppressions(Path(suppressions).read_text(encoding="utf-8"))
        for p in problems:
            failures.append(f"{suppressions}: {p}")
    if failures:
        for f in failures:
            print(f"self-test FAILED: {f}")
        return 1
    print("lsan_check self-test: ok" + (f" ({suppressions} follows the rules)" if suppressions else ""))
    return 0


# ---- main -------------------------------------------------------------------------------------------------------


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--logs", nargs="*", default=[], help="sanitizer log files or directories")
    ap.add_argument("--suppressions", help="lsan.supp, checked against the families")
    ap.add_argument("--check-suppressions", help="only check this suppression file")
    ap.add_argument("--qtest", help="the Qt Test text log (-o <file>,txt)")
    ap.add_argument("--exit-status", type=int, help="the soak executable's exit status")
    ap.add_argument("--source-root", help="the checkout (frames under its libs/ and apps/ are esses frames)")
    ap.add_argument("--expect-esses-leak", help="pass only when a counted leak has a frame containing this text")
    ap.add_argument("--github", action="store_true", help="also print GitHub Actions annotations")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test(args.suppressions)
    supp = args.check_suppressions or args.suppressions
    if supp:
        problems = check_suppressions(Path(supp).read_text(encoding="utf-8"))
        for p in problems:
            print(f"{supp}: {p}")
            if args.github:
                print(f"::error file={supp}::{p}")
        if problems:
            return 1
        if args.check_suppressions:
            print(f"{supp}: every pattern follows the rules")
            return 0
    qtest = None
    if args.qtest:
        try:
            qtest = Path(args.qtest).read_text(encoding="utf-8", errors="replace")
        except OSError:
            qtest = ""
    v = judge(read_logs(args.logs), qtest, args.exit_status, args.source_root, args.expect_esses_leak)
    for line in v.lines:
        print(line)
        if args.github and line.startswith("FAIL"):
            print(f"::error::{line}")
        elif args.github and line.startswith("warning"):
            print(f"::warning::{line}")
    print("soak run: " + ("PASS" if v.ok else "FAIL"))
    return 0 if v.ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
