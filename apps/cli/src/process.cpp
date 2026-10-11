// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "process.hpp"

#include "cli_common.hpp"

#include <esses/base/config.hpp>
#include <esses/base/paths.hpp>
#include <esses/base/utf8.hpp>

#if defined(ESSES_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <spawn.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace esses::cli {

std::string quoteWindowsArgument(std::string_view arg) {
    const bool needsQuotes = arg.empty() || arg.find_first_of(" \t\n\v\"") != std::string_view::npos;
    if (!needsQuotes) {
        return std::string(arg);
    }
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\'); // escape the backslashes and the quote
        } else {
            out.append(backslashes, '\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\'); // before the closing quote
    out.push_back('"');
    return out;
}

std::filesystem::path executableDirectory() {
    return Paths::executableDir();
}

#if defined(ESSES_OS_WINDOWS)

namespace {

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE && h != nullptr) {
            CloseHandle(h);
        }
    }
};

Result<void> openRedirect(const std::filesystem::path& path, Handle& out) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    out.h = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out.h == INVALID_HANDLE_VALUE) {
        ErrorContext ctx;
        ctx.path = path;
        return fail(ErrorCode::Io, "cannot create " + u8(path), std::move(ctx));
    }
    return ok();
}

} // namespace

Result<int> runProcess(const ProcessRequest& request) {
    std::string line = quoteWindowsArgument(request.program);
    for (const std::string& a : request.args) {
        line.push_back(' ');
        line += quoteWindowsArgument(a);
    }
    std::wstring wline = utf8::toWide(line);
    Handle out, err;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    const bool redirect = request.stdoutFile || request.stderrFile;
    if (redirect) {
        if (request.stdoutFile) {
            if (auto r = openRedirect(*request.stdoutFile, out); !r) {
                return Failure{r.error()};
            }
        }
        if (request.stderrFile) {
            if (auto r = openRedirect(*request.stderrFile, err); !r) {
                return Failure{r.error()};
            }
        }
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = request.stdoutFile ? out.h : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = request.stderrFile ? err.h : GetStdHandle(STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, wline.data(), nullptr, nullptr, redirect ? TRUE : FALSE, 0, nullptr, nullptr,
                        &si, &pi)) {
        const DWORD code = GetLastError();
        return fail(ErrorCode::Io, "cannot start '" + request.program + "' (Windows error " +
                                       std::to_string(static_cast<unsigned long>(code)) + ")");
    }
    Handle process(pi.hProcess);
    Handle thread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exitCode)) {
        return fail(ErrorCode::Io, "cannot read the exit code of '" + request.program + "'");
    }
    return static_cast<int>(exitCode);
}

#else

Result<int> runProcess(const ProcessRequest& request) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    std::string outPath, errPath;
    if (request.stdoutFile) {
        std::error_code ec;
        if (request.stdoutFile->has_parent_path()) {
            std::filesystem::create_directories(request.stdoutFile->parent_path(), ec);
        }
        outPath = request.stdoutFile->string();
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, outPath.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (request.stderrFile) {
        std::error_code ec;
        if (request.stderrFile->has_parent_path()) {
            std::filesystem::create_directories(request.stderrFile->parent_path(), ec);
        }
        errPath = request.stderrFile->string();
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, errPath.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    std::vector<std::string> storage;
    storage.reserve(request.args.size() + 1);
    storage.push_back(request.program);
    for (const std::string& a : request.args) {
        storage.push_back(a);
    }
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& s : storage) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, request.program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        return fail(ErrorCode::Io,
                    "cannot start '" + request.program + "' (errno " + std::to_string(rc) + ")");
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return fail(ErrorCode::Io, "waitpid failed for '" + request.program + "'");
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

#endif

} // namespace esses::cli
