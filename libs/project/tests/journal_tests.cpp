// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The write-ahead journal (02 section 6.8, D-019, D-070, D-187; AC-M2-06's Qt-free half): entry files written
// atomically with a monotonic seq, each a session's complete state (its older files deleted); replay takes
// each session's newest file and the newest entry per path across sessions, with baseHash conflicts; damaged
// files and interrupted writes are skipped, never fatal; clear() empties the journal, or one side of it by
// session, never touching a file written after it listed the journal; the recovery hook decides between
// recover, discard and later; the project files are never touched.

#include "persistence/journal_hooks.hpp"
#include "test_support.hpp"

#include <esses/base/hash.hpp>
#include <esses/model/sidecar.hpp>
#include <esses/project/journal.hpp>
#include <esses/project/mappings.hpp>
#include <esses/project/project_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace esses;
using namespace esses::project;
namespace pt = esses::project::test;

namespace {

using std::chrono::system_clock;

const Uuid kSessionA = Uuid::parse("b7e3a1c2-4d5e-4f60-8a1b-2c3d4e5f6a7b");
const Uuid kSessionB = Uuid::parse("0d9e8f7a-6b5c-4d3e-8f1a-0b9c8d7e6f5a");

system_clock::time_point at(std::int64_t seconds) {
    return system_clock::time_point(std::chrono::seconds(1'791'000'000 + seconds));
}

JournalEntry entry(std::string path, std::string_view text, std::optional<Hash128> base = std::nullopt) {
    auto e = JournalEntry::of(std::move(path), text, base);
    REQUIRE(e.ok());
    return std::move(e).value();
}

std::string mappingsText(std::string_view to) {
    Mappings m;
    m.rows.push_back({"rpm", std::string(to), std::nullopt, false});
    return m.toJson();
}

std::filesystem::path journalDir(const pt::TempDir& dir) {
    return dir.path() / ".journal";
}

} // namespace

TEST_CASE("journal: append writes atomic, canonical, schema-valid entry files with a monotonic seq",
          "[unit]") {
    const pt::TempDir dir("journal-append");
    Journal journal(dir.path(), kSessionA);
    CHECK_FALSE(journal.pending());
    REQUIRE(journal.append({}, at(0)).ok()); // nothing dirty: nothing written
    CHECK_FALSE(journal.pending());

    const std::string text = mappingsText("Engine RPM");
    REQUIRE(journal.append({entry("channels/mappings.json", text)}, at(120)).ok());
    CHECK(journal.pending());
    CHECK(journal.nextSeq() == 2);
    const auto first = journalDir(dir) / "1-b7e3a1c2.json"; // <seq>-<session tag>.json (D-190)
    REQUIRE(std::filesystem::exists(first));
    const std::string bytes = pt::readAll(first);
    CHECK(JsonDoc::parse(bytes)->canonical() == bytes);
    CHECK(pt::schemaErrors(bytes, "journal-1").empty());
    std::vector<std::string> log;
    const auto file = JournalFile::parse(bytes, log);
    REQUIRE(file.ok());
    CHECK(file->seq == 1);
    CHECK(file->at == "2026-10-03T04:02:00Z");
    CHECK(file->session == kSessionA);
    REQUIRE(file->entries.size() == 1);
    CHECK(file->entries[0].hash == hash128(JsonDoc::parse(text)->canonical()));
    CHECK_FALSE(file->entries[0].baseHash.has_value());
    CHECK(file->toJson() == bytes); // journal-1 round trip (AC-M2-07)

    // Another session continues after the highest seq on disk; this one after its own.
    Journal other(dir.path(), kSessionB);
    REQUIRE(other.append({entry("channels/mappings.json", mappingsText("RPM 2"))}, at(240)).ok());
    CHECK(std::filesystem::exists(journalDir(dir) / "2-0d9e8f7a.json"));
    REQUIRE(journal.append({entry("channels/mappings.json", mappingsText("RPM 3"))}, at(360)).ok());
    CHECK(std::filesystem::exists(journalDir(dir) / "3-b7e3a1c2.json"));
    CHECK(journal.nextSeq() == 4);
    CHECK_FALSE(std::filesystem::exists(dir.path() / "channels")); // project files are never written
}

TEST_CASE("journal: append refuses unsafe paths, duplicates and mismatching hashes", "[unit]") {
    const pt::TempDir dir("journal-refuse");
    Journal journal(dir.path(), kSessionA);
    const std::string text = mappingsText("A");
    for (const char* bad : {"", "/abs.json", "../up.json", "a/../b.json", "a//b.json", "./a.json",
                            "C:/x.json", "a\\b.json", "a/."}) {
        INFO(bad);
        CHECK_FALSE(JournalEntry::of(bad, text, std::nullopt).ok());
        JournalEntry e = entry("ok.json", text);
        e.path = bad;
        const auto r = journal.append({e}, at(0));
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::Invalid);
    }
    CHECK(JournalEntry::of(".local/settings.json", text, std::nullopt).ok()); // dot files are fine
    const auto twice = journal.append({entry("a.json", text), entry("a.json", text)}, at(0));
    REQUIRE_FALSE(twice.ok());
    JournalEntry wrong = entry("a.json", text);
    wrong.hash = hash128("something else");
    CHECK_FALSE(journal.append({wrong}, at(0)).ok());
    JournalEntry unhashed = entry("a.json", text);
    unhashed.hash = Hash128{}; // computed by append
    CHECK(journal.append({unhashed}, at(0)).ok());
    CHECK_FALSE(JournalEntry::of("a.json", "{not json", std::nullopt).ok());
}

TEST_CASE("journal: replay takes the newest entry per path and reports baseHash conflicts", "[unit]") {
    const pt::TempDir dir("journal-replay");
    // The files on disk the edits started from.
    std::filesystem::create_directories(dir.path() / "channels");
    const std::string mappingsOnDisk = mappingsText("On Disk");
    pt::writeAll(dir.path() / "channels/mappings.json", mappingsOnDisk);
    const std::string projectOnDisk = ProjectFile::makeDefault("P").toJson();
    pt::writeAll(dir.path() / "project.json", projectOnDisk);
    const Hash128 mappingsBase = hash128(mappingsOnDisk);
    const Hash128 projectBase = hash128(projectOnDisk);

    Journal a(dir.path(), kSessionA);
    REQUIRE(a.append({entry("channels/mappings.json", mappingsText("Edit 1"), mappingsBase),
                      entry("project.json", projectOnDisk, projectBase)},
                     at(120))
                .ok());
    REQUIRE(a.append({entry("channels/mappings.json", mappingsText("Edit 2"), mappingsBase),
                      entry("channels/aliases.json",
                            R"({"$schema": "urn:esses:aliases-1", "schemaVersion": 1, "aliases": []})")},
                     at(240))
                .ok());
    // A later file of another session (a second instance that crashed later) wins for the path it holds.
    Journal b(dir.path(), kSessionB);
    REQUIRE(
        b.append({entry("project.json", ProjectFile::makeDefault("Renamed").toJson(), projectBase)}, at(300))
            .ok());

    auto replay = a.replay();
    REQUIRE(replay.ok());
    REQUIRE(replay->entries.size() == 3);
    CHECK(replay->entries[0].path == "channels/aliases.json"); // path order
    CHECK(replay->entries[1].path == "channels/mappings.json");
    CHECK(replay->entries[1].document == *JsonDoc::parse(mappingsText("Edit 2")));
    CHECK(replay->entries[2].path == "project.json");
    CHECK(nlohmann::json::parse(replay->entries[2].document.compact()).at("name") == "Renamed");
    CHECK(replay->conflicts.empty());
    CHECK(replay->log.empty());

    // Another program changed a file after the journal was written; a new file appeared where none was.
    pt::writeAll(dir.path() / "channels/mappings.json", mappingsText("Changed Elsewhere"));
    pt::writeAll(dir.path() / "channels/aliases.json", "{}");
    replay = a.replay();
    REQUIRE(replay.ok());
    CHECK(replay->conflicts == std::vector<std::string>{"channels/aliases.json", "channels/mappings.json"});
    CHECK(replay->log.size() == 1);
    CHECK(pt::readAll(dir.path() / "project.json") == projectOnDisk); // replay never writes
}

// D-187: every append is the session's complete unsaved state, so its older files go and an entry it no
// longer lists (a document back to its disk version, an undone edit) is never replayed.
TEST_CASE("journal: a session's newest file is its state; older files are deleted", "[unit]") {
    const pt::TempDir dir("journal-newest");
    Journal a(dir.path(), kSessionA);
    REQUIRE(a.append({entry("channels/aliases.json",
                            R"({"$schema": "urn:esses:aliases-1", "schemaVersion": 1, "aliases": []})"),
                      entry("channels/mappings.json", mappingsText("Edit 1"))},
                     at(120))
                .ok());
    REQUIRE(a.append({entry("channels/mappings.json", mappingsText("Edit 2"))}, at(240)).ok());
    CHECK_FALSE(std::filesystem::exists(journalDir(dir) / "1-b7e3a1c2.json"));
    CHECK(std::filesystem::exists(journalDir(dir) / "2-b7e3a1c2.json"));
    auto replay = a.replay();
    REQUIRE(replay.ok());
    REQUIRE(replay->entries.size() == 1); // the aliases edit was undone before the second autosave
    CHECK(replay->entries[0].path == "channels/mappings.json");
    CHECK(replay->entries[0].document == *JsonDoc::parse(mappingsText("Edit 2")));

    // An older file of the session left behind (a failed deletion, or a journal written before D-187) is
    // superseded all the same.
    JournalFile stale;
    stale.seq = 1;
    stale.at = "2026-10-03T04:00:00Z";
    stale.session = kSessionA;
    stale.entries.push_back(entry("channels/overrides.json", R"({"$schema": "urn:esses:overrides-1"})"));
    REQUIRE(stale.save(journalDir(dir) / "1.json").ok());
    replay = a.replay();
    REQUIRE(replay.ok());
    CHECK(replay->entries.size() == 1);

    // Two sessions that changed the same document: the newer wins, and the replay says so.
    Journal b(dir.path(), kSessionB);
    REQUIRE(b.append({entry("channels/mappings.json", mappingsText("Other Session"))}, at(300)).ok());
    replay = a.replay();
    REQUIRE(replay.ok());
    CHECK(replay->entries.at(0).document == *JsonDoc::parse(mappingsText("Other Session")));
    REQUIRE(replay->log.size() == 1);
    CHECK(replay->log[0].find("changed in two sessions") != std::string::npos);
}

TEST_CASE("journal: pending, replay and clear by session", "[unit]") {
    const pt::TempDir dir("journal-sessions");
    Journal a(dir.path(), kSessionA);
    Journal b(dir.path(), kSessionB);
    CHECK_FALSE(a.pending(JournalSessions::Others));
    REQUIRE(a.append({entry("channels/mappings.json", mappingsText("A"))}, at(120)).ok());
    CHECK(a.pending(JournalSessions::Own));
    CHECK_FALSE(a.pending(JournalSessions::Others));
    CHECK(b.pending(JournalSessions::Others));
    REQUIRE(b.append({entry("channels/aliases.json",
                            R"({"$schema": "urn:esses:aliases-1", "schemaVersion": 1, "aliases": []})")},
                     at(240))
                .ok());
    auto others = a.replay(JournalSessions::Others);
    REQUIRE(others.ok());
    REQUIRE(others->entries.size() == 1);
    CHECK(others->entries[0].path == "channels/aliases.json");
    // An unreadable file is another session's: offered (and logged), deleted with the others.
    pt::writeAll(journalDir(dir) / "9.json", "{ damaged");
    CHECK(a.pending(JournalSessions::Others));
    REQUIRE(a.clear(JournalSessions::Own).ok());
    CHECK_FALSE(a.pending(JournalSessions::Own));
    CHECK(b.pending(JournalSessions::Own));
    REQUIRE(b.clear(JournalSessions::Others).ok());
    CHECK_FALSE(std::filesystem::exists(journalDir(dir) / "9.json"));
    CHECK(b.pending(JournalSessions::Own));
    REQUIRE(b.clear(JournalSessions::Own).ok());
    CHECK_FALSE(b.pending());
}

TEST_CASE("journal: a kill mid-write leaves a recoverable journal; damaged files are skipped", "[unit]") {
    const pt::TempDir dir("journal-crash");
    Journal journal(dir.path(), kSessionA);
    REQUIRE(
        journal.append({entry("channels/mappings.json", mappingsText("Saved At 2 Minutes"))}, at(120)).ok());
    // The process dies while writing the next entry: AtomicFile leaves only its temporary file behind.
    pt::writeAll(journalDir(dir) / "2.json.tmp-1a2b3c4d",
                 R"({"$schema": "urn:esses:journal-1", "seq": 2, "ent)");
    // A file damaged by something else, and one whose entry no longer matches its hash.
    pt::writeAll(journalDir(dir) / "7.json",
                 R"({"$schema": "urn:esses:journal-1", "schemaVersion": 1, "seq")");
    JournalFile tampered;
    tampered.seq = 8;
    tampered.at = "2026-10-12T08:00:00Z";
    tampered.session = kSessionA;
    JournalEntry e = entry("channels/mappings.json", mappingsText("Tampered"));
    e.document = *JsonDoc::parse(mappingsText("Something Else"));
    tampered.entries.push_back(e);
    REQUIRE(tampered.save(journalDir(dir) / "8.json").ok());

    CHECK(journal.pending());
    const auto replay = journal.replay();
    REQUIRE(replay.ok());
    REQUIRE(replay->entries.size() == 1);
    CHECK(replay->entries[0].document == *JsonDoc::parse(mappingsText("Saved At 2 Minutes")));
    REQUIRE(replay->log.size() == 2);
    CHECK(replay->log[0].starts_with("journal: skipped 7.json: "));
    CHECK(replay->log[1].starts_with("journal: skipped the entry for 'channels/mappings.json' in 8.json"));

    // The next append continues after every seq on disk, damaged ones included.
    REQUIRE(journal.append({entry("a.json", mappingsText("A"))}, at(240)).ok());
    CHECK(std::filesystem::exists(journalDir(dir) / "9-b7e3a1c2.json"));

    REQUIRE(journal.clear().ok());
    CHECK_FALSE(journal.pending());
    CHECK_FALSE(std::filesystem::exists(journalDir(dir))); // the temporary file went too
    REQUIRE(journal.clear().ok());                         // clearing nothing is fine
}

TEST_CASE("journal: the recovery hook recovers, discards or leaves the journal", "[unit]") {
    const pt::TempDir dir("journal-recover");
    Journal journal(dir.path(), kSessionA);
    int prompts = 0;
    const auto answer = [&prompts](JournalRecovery choice) {
        return [&prompts, choice](const JournalReplay& r) {
            ++prompts;
            CHECK(r.entries.size() == 1);
            return choice;
        };
    };
    auto none = journal.recover(answer(JournalRecovery::Recover));
    REQUIRE(none.ok());
    CHECK_FALSE(none->has_value());
    CHECK(prompts == 0); // no journal: no prompt

    REQUIRE(journal.append({entry("channels/mappings.json", mappingsText("Unsaved"))}, at(120)).ok());
    auto later = journal.recover(answer(JournalRecovery::Later));
    REQUIRE(later.ok());
    CHECK_FALSE(later->has_value());
    CHECK(journal.pending());
    auto recovered = journal.recover(answer(JournalRecovery::Recover));
    REQUIRE(recovered.ok());
    REQUIRE(recovered->has_value());
    CHECK((*recovered)->entries.at(0).path == "channels/mappings.json");
    CHECK(journal.pending()); // kept until an explicit Save clears it (02 section 6.8 rule 2)
    auto discarded = journal.recover(answer(JournalRecovery::Discard));
    REQUIRE(discarded.ok());
    CHECK_FALSE(discarded->has_value());
    CHECK_FALSE(journal.pending());
    CHECK(prompts == 3);
}

TEST_CASE("journal: journal-1 files round-trip and refuse what the schema refuses", "[unit]") {
    const std::string full = pt::readAll(pt::repoRoot() / "schemas/examples/journal-1/valid/full.json");
    std::vector<std::string> log;
    const auto file = JournalFile::parse(full, log);
    REQUIRE(file.ok());
    CHECK(file->toJson() == full);
    CHECK(file->entries.at(0).baseHash == model::parseHashText("xxh3:9f3c1b2a0d4e5f60718293a4b5c6d7e8"));
    const auto expect =
        nlohmann::json::parse(pt::readAll(pt::repoRoot() / "schemas/examples/journal-1/expect.json"));
    for (const auto& path : pt::jsonFiles(pt::repoRoot() / "schemas/examples/journal-1/invalid")) {
        INFO(path.filename().string());
        const auto r = JournalFile::parse(pt::readAll(path), log);
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().context.detail.value_or("?").starts_with(
            expect.at(path.filename().string()).at("pointer").get<std::string>()));
    }
}

// D-190: a save journals what it writes first (D-187), so a save that wrote some files and stopped (a
// failure, a kill before the journal was cleared) leaves entries whose document is on disk already. They are
// not replayed and are no conflict; a journal holding nothing else is cleared without a prompt.
TEST_CASE("journal: an entry the disk already holds is not replayed and is no conflict", "[unit]") {
    const pt::TempDir dir("journal-applied");
    std::filesystem::create_directories(dir.path() / "channels");
    const std::string before = mappingsText("Before");
    pt::writeAll(dir.path() / "channels/mappings.json", before);
    const std::string saved = mappingsText("Saved");
    Journal a(dir.path(), kSessionA);
    REQUIRE(a.append({entry("channels/mappings.json", saved, hash128(before)),
                      entry("channels/aliases.json",
                            R"({"$schema": "urn:esses:aliases-1", "schemaVersion": 1, "aliases": []})")},
                     at(120))
                .ok());
    // The save wrote mappings.json, then failed on aliases.json.
    pt::writeAll(dir.path() / "channels/mappings.json", JsonDoc::parse(saved)->canonical());
    Journal next(dir.path(), kSessionB);
    auto replay = next.replay();
    REQUIRE(replay.ok());
    REQUIRE(replay->entries.size() == 1);
    CHECK(replay->entries[0].path == "channels/aliases.json");
    CHECK(replay->applied == std::vector<std::string>{"channels/mappings.json"});
    CHECK(replay->conflicts.empty());
    CHECK(replay->skipped == 0);

    // The save then wrote aliases.json too and was killed before deleting the journal: nothing to offer.
    pt::writeAll(dir.path() / "channels/aliases.json",
                 JsonDoc::parse(R"({"$schema": "urn:esses:aliases-1", "schemaVersion": 1, "aliases": []})")
                     ->canonical());
    int prompts = 0;
    auto none = next.recover([&prompts](const JournalReplay&) {
        ++prompts;
        return JournalRecovery::Recover;
    });
    REQUIRE(none.ok());
    CHECK_FALSE(none->has_value());
    CHECK(prompts == 0);
    CHECK_FALSE(next.pending());
}

// D-190: two sessions that read the same highest seq write files of their own instead of replacing each
// other's.
TEST_CASE("journal: two sessions that pick the same seq keep both files", "[unit]") {
    const pt::TempDir dir("journal-same-seq");
    Journal a(dir.path(), kSessionA);
    Journal b(dir.path(), kSessionB);
    REQUIRE(a.append({entry("channels/mappings.json", mappingsText("A"))}, at(120)).ok());
    // B lists the folder before A's file lands: both take seq 1.
    const auto aFile = journalDir(dir) / "1-b7e3a1c2.json";
    const auto aside = dir.path() / "aside.json";
    std::filesystem::rename(aFile, aside);
    REQUIRE(b.append({entry("channels/aliases.json",
                            R"({"$schema": "urn:esses:aliases-1", "schemaVersion": 1, "aliases": []})")},
                     at(130))
                .ok());
    std::filesystem::rename(aside, aFile);
    CHECK(std::filesystem::exists(journalDir(dir) / "1-0d9e8f7a.json"));
    CHECK(std::filesystem::exists(aFile));
    CHECK(a.pending(JournalSessions::Own));
    CHECK(a.pending(JournalSessions::Others));
    auto replay = a.replay();
    REQUIRE(replay.ok());
    REQUIRE(replay->entries.size() == 2); // both sessions' edits survive
    // Untagged names of journals written before D-190 still count: they are read to tell their session.
    JournalFile legacy;
    legacy.seq = 5;
    legacy.at = "2026-10-03T04:00:00Z";
    legacy.session = kSessionB;
    legacy.entries.push_back(entry("channels/overrides.json", R"({"$schema": "urn:esses:overrides-1"})"));
    REQUIRE(legacy.save(journalDir(dir) / "5.json").ok());
    REQUIRE(a.append({entry("channels/mappings.json", mappingsText("A2"))}, at(140)).ok());
    CHECK(std::filesystem::exists(journalDir(dir) / "6-b7e3a1c2.json"));
    REQUIRE(a.clear(JournalSessions::Others).ok());
    CHECK_FALSE(std::filesystem::exists(journalDir(dir) / "5.json"));
    CHECK_FALSE(std::filesystem::exists(journalDir(dir) / "1-0d9e8f7a.json"));
    CHECK(a.pending(JournalSessions::Own));
}

// D-190: clearing the whole journal is one rename of the folder; a folder an interrupted clear left behind is
// read by nobody and removed by the next clear.
TEST_CASE("journal: clearing the whole journal renames it away first", "[unit]") {
    const pt::TempDir dir("journal-clear-all");
    Journal a(dir.path(), kSessionA);
    REQUIRE(a.append({entry("channels/mappings.json", mappingsText("A"))}, at(120)).ok());
    const auto abandoned = dir.path() / ".journal-clearing-0d9e8f7a-42";
    std::filesystem::create_directories(abandoned);
    pt::writeAll(abandoned / "3-0d9e8f7a.json", "{}");
    Journal b(dir.path(), kSessionB);
    CHECK(b.pending(JournalSessions::Others)); // A's file
    REQUIRE(b.clear().ok());
    CHECK_FALSE(std::filesystem::exists(journalDir(dir)));
    CHECK_FALSE(std::filesystem::exists(abandoned));
    for (const auto& e : std::filesystem::directory_iterator(dir.path())) {
        CHECK_FALSE(e.path().filename().string().starts_with(".journal"));
    }
    CHECK_FALSE(a.pending());
    REQUIRE(a.append({entry("channels/mappings.json", mappingsText("A2"))}, at(240)).ok());
    CHECK(a.pending(JournalSessions::Own));
}

namespace {

// Sets the clear hook of journal_hooks.hpp for one scope and restores the previous one on every exit.
class ClearHook {
public:
    explicit ClearHook(std::function<void()> hook)
        : previous_(project::detail::setJournalClearHook(std::move(hook))) {}
    ~ClearHook() { project::detail::setJournalClearHook(std::move(previous_)); }
    ClearHook(const ClearHook&) = delete;
    ClearHook& operator=(const ClearHook&) = delete;

private:
    std::function<void()> previous_;
};

} // namespace

// D-190, found in its verification: a clear deletes only the files it listed. Another session that writes
// between the listing and the deletions (its newer state, or a write still in progress) keeps its file.
TEST_CASE("journal: a clear never deletes a file written after it listed the journal", "[unit]") {
    const pt::TempDir dir("journal-clear-race");
    Journal a(dir.path(), kSessionA);
    Journal b(dir.path(), kSessionB);
    SECTION("Others: the other session's newer file and its write in progress stay") {
        REQUIRE(b.append({entry("channels/mappings.json", mappingsText("B1"))}, at(120)).ok());
        const auto inFlight = journalDir(dir) / "9-0d9e8f7a.json.tmp-1"; // an AtomicFile temporary file
        int calls = 0;
        {
            const ClearHook hook([&] {
                ++calls;
                // B supersedes the file the clear listed, then starts another write.
                REQUIRE(b.append({entry("channels/mappings.json", mappingsText("B2"))}, at(240)).ok());
                pt::writeAll(inFlight, "{");
            });
            REQUIRE(a.clear(JournalSessions::Others).ok());
        }
        CHECK(calls == 1);
        CHECK_FALSE(std::filesystem::exists(journalDir(dir) / "1-0d9e8f7a.json"));
        CHECK(std::filesystem::exists(journalDir(dir) / "2-0d9e8f7a.json"));
        CHECK(std::filesystem::exists(inFlight));
        CHECK(b.ownFileExists());
        auto replay = a.replay(JournalSessions::Others);
        REQUIRE(replay.ok());
        REQUIRE(replay->entries.size() == 1);
        CHECK(replay->entries[0].document == *JsonDoc::parse(mappingsText("B2")));
        // Without an interleaved write the same clear takes the other session's files and its temporary file.
        REQUIRE(a.clear(JournalSessions::Others).ok());
        CHECK_FALSE(a.pending());
        CHECK_FALSE(std::filesystem::exists(inFlight));
    }
    SECTION("Own: another session's file written meanwhile stays") {
        REQUIRE(a.append({entry("channels/mappings.json", mappingsText("A"))}, at(120)).ok());
        {
            const ClearHook hook([&] {
                REQUIRE(
                    b.append(
                         {entry("channels/aliases.json",
                                R"({"$schema": "urn:esses:aliases-1", "schemaVersion": 1, "aliases": []})")},
                         at(240))
                        .ok());
            });
            REQUIRE(a.clear(JournalSessions::Own).ok());
        }
        CHECK_FALSE(a.pending(JournalSessions::Own));
        CHECK(std::filesystem::exists(journalDir(dir) / "2-0d9e8f7a.json"));
        CHECK(b.ownFileExists());
        CHECK(a.pending(JournalSessions::Others));
    }
}

// D-190 (d), found in its verification: ownFileExists() tells a session that another session's clear
// (a Discard on its recovery prompt) deleted its newest file, so its next autosave writes the file again.
TEST_CASE("journal: ownFileExists follows the session's newest file", "[unit]") {
    const pt::TempDir dir("journal-own-file");
    Journal a(dir.path(), kSessionA);
    Journal b(dir.path(), kSessionB);
    CHECK_FALSE(a.ownFileExists()); // nothing written
    REQUIRE(a.append({entry("channels/mappings.json", mappingsText("A1"))}, at(120)).ok());
    CHECK(a.ownFileExists());
    REQUIRE(a.append({entry("channels/mappings.json", mappingsText("A2"))}, at(240)).ok());
    CHECK(a.ownFileExists()); // the newest, after the older one was deleted
    REQUIRE(b.clear(JournalSessions::Others).ok());
    CHECK_FALSE(a.ownFileExists());
    REQUIRE(a.append({entry("channels/mappings.json", mappingsText("A2"))}, at(360)).ok());
    CHECK(a.ownFileExists());
    REQUIRE(a.clear(JournalSessions::Own).ok());
    CHECK_FALSE(a.ownFileExists());
}
