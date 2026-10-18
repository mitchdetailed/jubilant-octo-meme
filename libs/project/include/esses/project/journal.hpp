// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Journal (02 section 6.8, D-019, D-070, D-187, D-190; schema journal-1,
// <Project>/.journal/<seq>-<tag>.json, the tag being the first eight hex digits of the session id): the
// write-ahead autosave. Every 2 minutes (project option options.general.autosaveMinutes, 0 = off), on request
// and before an explicit Save writes anything, the session appends one journal file holding the full
// canonical document of every project file that differs from disk: a complete snapshot of that session's
// unsaved state, so once it is written the session's older files are deleted (project files are not
// rewritten by autosave). An explicit Save then writes every dirty document atomically, copies the previous
// version of each to backup/ and deletes the session's own journal files, in that order, so a crash or a
// failure mid-save leaves a journal that holds what was being saved. On open a journal offers recovery: the
// newest file of each session is that session's state (a damaged entry in it falls back to the same path in
// the session's next older file), the sessions merge newest entry per path in seq-then-time order, and each
// entry is checked against the file on disk: a file that already holds the entry's document is not replayed
// (a save wrote it and stopped before clearing the journal), else a baseHash mismatch is reported as a
// conflict. Journal files are written atomically (temp + rename), so a kill during autosave leaves either the
// previous set or a complete new entry (AC-M2-06); the session tag keeps two sessions that pick the same seq
// apart. Clearing the whole journal renames the folder away first, and a partial clear deletes oldest first,
// so an interrupted clear never leaves an older state behind a newer one; a clear deletes only the files it
// listed, never one another session wrote after it began (D-190).
#pragma once

#include <esses/base/hash.hpp>
#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/base/uuid.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::project {

struct JournalEntry {
    std::string path;                // project-relative, '/'-separated ("workbooks/General.workbook.json")
    std::optional<Hash128> baseHash; // xxh3 of the on-disk canonical bytes the edit started from; null = new
    Hash128 hash;                    // xxh3 of `document`'s canonical bytes (JsonDoc::canonical())
    JsonDoc document;                // the full canonical document
    friend bool operator==(const JournalEntry&, const JournalEntry&) = default;

    // An entry for `canonicalText` (a document's toJson()), hash computed. Errors: Invalid for a path that is
    // empty, absolute, not '/'-separated or leaves the project folder ("..", "."), Format for text that is
    // not JSON.
    static Result<JournalEntry> of(std::string entryPath, std::string_view canonicalText,
                                   std::optional<Hash128> base);
};

// One <Project>/.journal/<seq>.json file (journal-1): the document Journal writes and replays. `at` keeps the
// ISO 8601 text as written; unknown top-level members are preserved, entries are closed.
struct JournalFile {
    static constexpr std::string_view kSchemaId = "urn:esses:journal-1";

    std::uint64_t seq = 1;
    std::string at; // ISO 8601 with an offset ("2026-10-12T08:00:00Z")
    Uuid session;
    std::vector<JournalEntry> entries;
    int schemaVersion = 1;
    JsonDoc unknown; // unrecognised top-level members, preserved

    // Errors: Format (syntax), Invalid (a journal-1 rule; context.detail = JSON pointer), Unsupported
    // (another major).
    static Result<JournalFile> parse(std::string_view json, std::vector<std::string>& log);
    static Result<JournalFile> load(const std::filesystem::path& path, std::vector<std::string>& log);
    std::string toJson() const; // canonical text
    Result<void> save(const std::filesystem::path& path) const;

    friend bool operator==(const JournalFile&, const JournalFile&) = default;
};

struct JournalReplay {
    std::vector<JournalEntry> entries;  // the newest entry per path the disk lacks, in path order
    std::vector<std::string> conflicts; // paths whose baseHash differs from the file on disk
    std::vector<std::string> applied;   // paths whose file already holds the entry's document (not replayed)
    std::vector<std::string> log;       // skipped or unreadable journal files and entries, applied paths
    std::size_t skipped = 0;            // the unreadable files and damaged entries among them
};

// Whose files a call looks at: every file, the files of this Journal's session, or those of the other
// sessions (an unreadable file counts as another session's).
enum class JournalSessions : std::uint8_t { All, Own, Others };

// The answer of the recovery prompt (06 section 18, AC-M2-06: "Recover unsaved changes?").
enum class JournalRecovery : std::uint8_t {
    Recover, // apply the replayed entries in memory (the project becomes dirty); the journal stays until Save
    Discard, // delete the journal
    Later    // keep the journal untouched (the prompt was dismissed)
};

class Journal {
public:
    static constexpr std::string_view kSchemaId = "urn:esses:journal-1";
    static constexpr std::string_view kDirName = ".journal";

    // `projectFolder` holds .journal/; `session` identifies this process's entries.
    Journal(std::filesystem::path projectFolder, Uuid session);

    // Appends <seq>-<tag>.json with `entries`, the session's complete set of unsaved documents (atomic;
    // nothing is written for an empty list), then deletes the files this object wrote before (superseded; a
    // failed deletion is harmless, replay reads a session's newest file). seq is one above the highest seq in
    // .journal/ and above every seq this object wrote. An entry whose hash is zero gets the hash of its
    // document; a non-zero hash must match it. Errors: Invalid (an unsafe path, a hash mismatch, a path
    // listed twice), Io.
    Result<void> append(const std::vector<JournalEntry>& entries,
                        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
    // True when .journal/ holds at least one entry file of `which` sessions.
    bool pending(JournalSessions which = JournalSessions::All) const;
    // True when the newest file this object wrote is still in .journal/ (another session's clear, a Discard
    // on its recovery prompt, may have deleted it); false when this object holds no file (none written, or
    // cleared). One file-system query, no read.
    bool ownFileExists() const;
    // The journal's state over `which` sessions: per session its newest file (an entry whose hash does not
    // match its document falls back to the same path in that session's next older file), the sessions merged
    // newest entry per path (by seq, then time, then file name), without the entries the files on disk
    // already hold (`applied`), with baseHash conflicts against the files on disk. A file that is not a
    // readable journal-1 document and a damaged entry are skipped, counted and logged. Errors: Io.
    Result<JournalReplay> replay(JournalSessions which = JournalSessions::All) const;
    // Deletes the journal files of `which` sessions: All (and interrupted temporary files: "discard" on the
    // recovery prompt at open; the folder is renamed away in one step, then deleted), Own (an explicit Save,
    // or leaving the project without saving), Others (a recovery that was adopted or discarded later;
    // temporary files too). Files are deleted oldest first, and only those the folder listed when the clear
    // began: a file another session writes meanwhile stays. Errors: Io.
    Result<void> clear(JournalSessions which = JournalSessions::All);
    // Recovery on open (02 section 6.8 rule 3): when pending(), replays the journal and asks `prompt`;
    // Recover returns the replay for the caller to apply, Discard clears the journal, Later leaves it. A
    // journal whose every entry is on disk already (nothing skipped) is cleared without asking. nullopt when
    // nothing is pending or the answer was not Recover. Errors: replay() and clear() errors.
    Result<std::optional<JournalReplay>>
    recover(const std::function<JournalRecovery(const JournalReplay&)>& prompt);

    const std::filesystem::path& folder() const noexcept { return folder_; }
    const Uuid& session() const noexcept { return session_; }
    std::uint64_t nextSeq() const noexcept { return seq_; }

private:
    std::filesystem::path folder_;
    Uuid session_;
    std::uint64_t seq_ = 1;
    std::vector<std::string> written_; // the file names this object wrote and has not deleted yet
};

} // namespace esses::project
