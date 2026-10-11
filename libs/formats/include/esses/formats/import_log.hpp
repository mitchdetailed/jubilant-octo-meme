// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ImportLogEntry, ImportLog and DecodeLog (05 section 1.1): the one diagnostic record of esses_formats. Every
// heuristic a reader applies produces an entry with a stable code (05 section 1.3 rule 9). Entries raised
// during sniff, probe and open go to the reader's ImportLog (deterministic, single thread, file order);
// entries raised while chunks decode on workers go to the source's DecodeLog.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace esses::formats {

struct ImportLogEntry {
    enum class Level : std::uint8_t { Info, Warning, Error };
    struct Context {
        std::optional<std::string> layer, channel, detail; // detail: the value chosen, e.g. "afterMul"
        std::optional<std::uint64_t> line, offset;
        std::uint32_t repeats = 1;
        friend bool operator==(const Context&, const Context&) = default;
    };

    Level level = Level::Info;
    std::string code;    // stable id, e.g. "ld.count_mismatch"
    std::string message; // UTF-8, human readable
    Context ctx;

    friend bool operator==(const ImportLogEntry&, const ImportLogEntry&) = default;
};

std::string_view importLogLevelName(ImportLogEntry::Level level) noexcept; // "Info", "Warning", "Error"

// Thread-safe. add() coalesces identical (code, layer, channel) keys: the first three occurrences are kept as
// entries, every later one bumps ctx.repeats of the third.
class ImportLog {
public:
    ImportLog() = default;
    ImportLog(const ImportLog& other);
    ImportLog& operator=(const ImportLog& other);
    virtual ~ImportLog() = default;

    void add(ImportLogEntry e);
    void add(ImportLogEntry::Level level, std::string code, std::string message,
             ImportLogEntry::Context ctx = {});
    // Copy under the lock in canonical order: (code, layer, channel, offset), stable.
    std::vector<ImportLogEntry> snapshot() const;
    // Copy under the lock in the order the entries were added.
    std::vector<ImportLogEntry> entries() const;
    std::uint64_t version() const;
    bool hasErrors() const;
    bool contains(std::string_view code) const;
    // Entries with this code (repeats not expanded).
    std::size_t count(std::string_view code) const;
    std::size_t size() const;

private:
    using Key = std::tuple<std::string, std::string, std::string>; // code, layer, channel
    mutable std::mutex m_;
    std::vector<ImportLogEntry> items_;
    std::map<Key, std::pair<std::uint32_t, std::size_t>> seen_; // occurrences, index of the latest entry
    std::uint64_t version_ = 0;
};

// Per source: entries raised by chunk decodes on workers (05 section 1.1).
class DecodeLog : public ImportLog {};

} // namespace esses::formats
