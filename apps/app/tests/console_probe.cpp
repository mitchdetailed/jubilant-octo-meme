// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses_console_probe (ctest app.version_console, Windows only): `esses --version` started from a terminal
// must print into that terminal, although esses is a GUI-subsystem executable (08 section 3.10). ctest gives
// a test pipes for its output, and esses writes to those anyway (app.version); a shell gives a GUI program no
// standard handles at all. The probe recreates that without opening a window: it creates a pseudo console
// (ConPTY), runs a second copy of itself inside it (--inside) as the shell, and that copy starts esses as a
// shell does (no inherited handles, no new console). esses has to attach to the pseudo console and print
// there; the probe reads the pseudo console's output back and looks for "esses <version>".
//   esses_console_probe <esses.exe> [arguments...]
// Exit code 0 when esses exited with 0 and its version line reached the pseudo console, 1 otherwise.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

namespace {

constexpr DWORD kTimeoutMs = 60000;

// One argument, quoted for CommandLineToArgvW / the C runtime's parser.
std::wstring quoted(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
        } else {
            out.append(slashes, L'\\');
        }
        out += c;
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}

std::wstring commandLine(const std::vector<std::wstring>& args) {
    std::wstring line;
    for (const std::wstring& a : args) {
        if (!line.empty()) {
            line += L' ';
        }
        line += quoted(a);
    }
    return line;
}

// Starts the command line, waits for it and returns its exit code; -1 when it could not run or timed out.
int runAndWait(std::wstring line, DWORD flags, STARTUPINFOW* startup) {
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, startup,
                       &pi) == 0) {
        std::fprintf(stderr, "console_probe: CreateProcess failed (Win32 error %lu)\n", GetLastError());
        return -1;
    }
    CloseHandle(pi.hThread);
    DWORD code = 0;
    const bool done = WaitForSingleObject(pi.hProcess, kTimeoutMs) == WAIT_OBJECT_0 &&
                      GetExitCodeProcess(pi.hProcess, &code) != 0;
    if (!done) {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hProcess);
    return done ? static_cast<int>(code) : -1;
}

// Inside the pseudo console: start esses the way a shell starts a GUI program and pass on its exit code.
int inside(const std::vector<std::wstring>& esses) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    return runAndWait(commandLine(esses), 0, &startup);
}

void drain(HANDLE pipe, std::string& into) {
    DWORD available = 0;
    while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) != 0 && available > 0) {
        char buffer[4096];
        DWORD read = 0;
        const DWORD want = available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));
        if (ReadFile(pipe, buffer, want, &read, nullptr) == 0 || read == 0) {
            return;
        }
        into.append(buffer, read);
    }
}

// The pseudo console's output is a VT stream: drop the escape sequences and carriage returns.
std::string plainText(const std::string& vt) {
    std::string out;
    for (std::size_t i = 0; i < vt.size(); ++i) {
        const char c = vt[i];
        if (c == '\x1b' && i + 1 < vt.size()) {
            const char kind = vt[i + 1];
            i += 2;
            if (kind == '[') { // CSI: parameter bytes up to a final byte in 0x40..0x7e
                while (i < vt.size() && (vt[i] < 0x40 || vt[i] > 0x7e)) {
                    ++i;
                }
            } else if (kind == ']') { // OSC: up to BEL or ESC backslash
                while (i < vt.size() && vt[i] != '\x07' && vt[i] != '\x1b') {
                    ++i;
                }
                if (i < vt.size() && vt[i] == '\x1b') {
                    ++i;
                }
            } else {
                --i; // a two-byte sequence
            }
            continue;
        }
        if (c != '\r') {
            out += c;
        }
    }
    return out;
}

// "esses" then the version's first digit; the space between may arrive as a cursor movement.
bool hasVersionLine(const std::string& text) {
    for (std::size_t at = text.find("esses"); at != std::string::npos; at = text.find("esses", at + 1)) {
        std::size_t i = at + 5;
        while (i < text.size() && text[i] == ' ') {
            ++i;
        }
        if (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
            return true;
        }
    }
    return false;
}

int outside(const std::wstring& self, const std::vector<std::wstring>& esses) {
    HANDLE inRead = nullptr;
    HANDLE inWrite = nullptr;
    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    if (CreatePipe(&inRead, &inWrite, nullptr, 0) == 0 ||
        CreatePipe(&outRead, &outWrite, nullptr, 1 << 20) == 0) {
        std::fprintf(stderr, "console_probe: CreatePipe failed (Win32 error %lu)\n", GetLastError());
        return 1;
    }
    HPCON console = nullptr;
    const HRESULT created = CreatePseudoConsole(COORD{200, 50}, inRead, outWrite, 0, &console);
    CloseHandle(inRead); // the pseudo console holds its own references
    CloseHandle(outWrite);
    if (created != S_OK) {
        std::fprintf(stderr, "console_probe: CreatePseudoConsole failed (HRESULT 0x%08lx)\n",
                     static_cast<unsigned long>(created));
        return 1;
    }

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (InitializeProcThreadAttributeList(attributes, 1, 0, &size) == 0 ||
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console,
                                  sizeof(console), nullptr, nullptr) == 0) {
        std::fprintf(stderr, "console_probe: attribute list failed (Win32 error %lu)\n", GetLastError());
        ClosePseudoConsole(console);
        return 1;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;

    std::vector<std::wstring> args{self, L"--inside"};
    args.insert(args.end(), esses.begin(), esses.end());
    std::wstring line = commandLine(args);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr,
                       nullptr, &startup.StartupInfo, &pi) == 0) {
        std::fprintf(stderr, "console_probe: CreateProcess failed (Win32 error %lu)\n", GetLastError());
        DeleteProcThreadAttributeList(attributes);
        ClosePseudoConsole(console);
        return 1;
    }
    CloseHandle(pi.hThread);

    // Read while the copy inside runs (a full pipe would stall the pseudo console), then a little longer: the
    // pseudo console renders asynchronously.
    std::string vt;
    const ULONGLONG start = GetTickCount64();
    bool exited = false;
    while (!exited && GetTickCount64() - start < kTimeoutMs) {
        drain(outRead, vt);
        exited = WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0;
    }
    DWORD code = 1;
    if (!exited) {
        TerminateProcess(pi.hProcess, 1);
        std::fprintf(stderr, "console_probe: timed out\n");
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hProcess);
    const ULONGLONG settle = GetTickCount64();
    while (!hasVersionLine(plainText(vt)) && GetTickCount64() - settle < 5000) {
        drain(outRead, vt);
        Sleep(20);
    }
    // Closing our end first: ClosePseudoConsole may otherwise wait for its last frame to be read.
    CloseHandle(outRead);
    CloseHandle(inWrite);
    ClosePseudoConsole(console);
    DeleteProcThreadAttributeList(attributes);

    const std::string text = plainText(vt);
    std::printf("esses exit code: %lu\npseudo console output:\n%s\n", code, text.c_str());
    if (code != 0 || !hasVersionLine(text)) {
        std::fprintf(stderr, "console_probe: no version line from esses in the console that started it\n");
        return 1;
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::vector<std::wstring> args(argv, argv + argc);
    if (args.size() >= 3 && args[1] == L"--inside") {
        return inside(std::vector<std::wstring>(args.begin() + 2, args.end()));
    }
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: esses_console_probe <esses.exe> [arguments...]\n");
        return 2;
    }
    std::wstring self(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
        if (n == 0) {
            return 2;
        }
        if (n < self.size()) {
            self.resize(n);
            break;
        }
        self.resize(self.size() * 2);
    }
    return outside(self, std::vector<std::wstring>(args.begin() + 1, args.end()));
}
