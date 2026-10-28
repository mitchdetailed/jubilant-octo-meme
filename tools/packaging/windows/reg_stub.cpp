// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses_reg_stub (ctest packaging.association_scripts, Windows only; never installed): a stand-in for
// reg.exe, so that the test of the portable zip's associate-ld.cmd and unassociate-ld.cmd never touches the
// registry. `smoke.py --script-test` copies it to <a temporary folder>\System32\reg.exe, the one place the
// scripts take reg from, and runs them with SystemRoot naming that folder (D-218 (b)). Each run appends one
// line to the file ESSES_REG_STUB_LOG names: a JSON array of the stub's own path and of its arguments as
// reg.exe receives them, in UTF-8. Then:
//   query ...  prints the file ESSES_REG_STUB_QUERY names, byte for byte, and exits 0; without that file it
//              prints an error to stderr and exits 1, as reg does for a key or value that is not there;
//   <verb> ... exits 1 with an error when ESSES_REG_STUB_FAIL names that verb (add, delete), else 0.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdio>
#include <string>

#include <windows.h>

namespace {

std::wstring environment(const wchar_t* name) {
    std::wstring value(64, L'\0');
    for (;;) {
        const DWORD n = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        if (n == 0) {
            return {};
        }
        if (n < value.size()) {
            value.resize(n);
            return value;
        }
        value.resize(n); // n counts the terminating null when the buffer was too small
    }
}

std::wstring modulePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) {
            return {};
        }
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = static_cast<int>(text.size());
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), size, out.data(), n, nullptr, nullptr);
    }
    return out;
}

std::string jsonString(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (byte < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(byte));
            out += escaped;
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

bool appendTo(const std::wstring& path, const std::string& line) {
    const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
    return ok != 0 && written == line.size();
}

bool readAll(const std::wstring& path, std::string& into) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    char buffer[4096];
    bool ok = true;
    for (;;) {
        DWORD read = 0;
        if (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) == 0) {
            ok = false;
            break;
        }
        if (read == 0) {
            break;
        }
        into.append(buffer, read);
    }
    CloseHandle(file);
    return ok;
}

// Byte for byte, without the C runtime's text-mode line-end translation.
void writeAll(DWORD which, const std::string& bytes) {
    const HANDLE out = GetStdHandle(which);
    std::size_t done = 0;
    while (done < bytes.size()) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(bytes.size() - done);
        if (WriteFile(out, bytes.data() + done, chunk, &written, nullptr) == 0 || written == 0) {
            return;
        }
        done += written;
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::string line = "[" + jsonString(utf8(modulePath()));
    for (int i = 1; i < argc; ++i) {
        line += "," + jsonString(utf8(argv[i]));
    }
    line += "]\n";
    const std::wstring log = environment(L"ESSES_REG_STUB_LOG");
    if (!log.empty() && !appendTo(log, line)) {
        writeAll(STD_ERROR_HANDLE, "reg stub: cannot write ESSES_REG_STUB_LOG\r\n");
        return 3;
    }
    const std::wstring verb = argc > 1 ? std::wstring(argv[1]) : std::wstring();
    const std::wstring failing = environment(L"ESSES_REG_STUB_FAIL");
    if (!verb.empty() && !failing.empty() && lstrcmpiW(verb.c_str(), failing.c_str()) == 0) {
        writeAll(STD_ERROR_HANDLE, "reg stub: " + utf8(verb) + " fails (ESSES_REG_STUB_FAIL)\r\n");
        return 1;
    }
    if (lstrcmpiW(verb.c_str(), L"query") == 0) {
        std::string text;
        const std::wstring source = environment(L"ESSES_REG_STUB_QUERY");
        if (source.empty() || !readAll(source, text)) {
            writeAll(STD_ERROR_HANDLE, "reg stub: no such key or value (no ESSES_REG_STUB_QUERY file)\r\n");
            return 1;
        }
        writeAll(STD_OUTPUT_HANDLE, text);
    }
    return 0;
}
