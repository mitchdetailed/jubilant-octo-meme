// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Runs a child process without a shell (CreateProcessW on Windows, posix_spawnp elsewhere): the oracle
// (python -I tools/ld_ref_decode.py) for `fixture add --oracle ref-decode`, and esses-cli for
// esses-gen-fixtures. Arguments are passed verbatim; no quoting by the caller.
#pragma once

#include <esses/base/result.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::cli {

struct ProcessRequest {
    std::string program;                             // a path, or a bare name searched on PATH ("python")
    std::vector<std::string> args;                   // UTF-8, without the program
    std::optional<std::filesystem::path> stdoutFile; // redirected (truncated) when set, else inherited
    std::optional<std::filesystem::path> stderrFile; // redirected when set, else inherited
};

// The exit code of the child. Io when it cannot be started.
Result<int> runProcess(const ProcessRequest& request);

// One argument quoted for the Windows command line so CommandLineToArgvW and the C runtime give it back
// unchanged (exposed for tests; identity-safe on every platform).
std::string quoteWindowsArgument(std::string_view arg);

// The directory of the running executable (the sibling tools live there).
std::filesystem::path executableDirectory();

} // namespace esses::cli
