// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Journal (02 section 6.8, D-019, D-070, D-187, D-190; journal-1): the write-ahead autosave of a project
// folder. Every entry file <Project>/.journal/<seq>-<session tag>.json is written atomically (AtomicFile:
// temporary file, flush, rename), so a kill leaves the previous set of files plus at most an orphaned
// temporary file, which replay() ignores and clear() removes; the session tag keeps two sessions that pick
// the same seq from replacing each other's file. clear() of the whole journal renames the folder away first
// and deletes the files of a session oldest first, so an interrupted clear leaves either the whole journal or
// each session's newest file; a clear deletes only the files it listed, so a file another session writes
// meanwhile stays. The project files themselves are never written here.

#include "../doc_io.hpp"
#include "journal_hooks.hpp"

#include <esses/base/atomic_file.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/sidecar.hpp>
#include <esses/project/journal.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace esses::project {

namespace {

namespace fs = std::filesystem;
using detail::json;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;

constexpr detail::DocKind kJournalKind{"journal", 1};
constexpr std::string_view kJsonSuffix = ".json";
constexpr std::string_view kTempMarker = ".json.tmp-";             // AtomicFile's temporary files
constexpr std::string_view kClearingPrefix = ".journal-clearing-"; // a whole journal on its way out
constexpr std::size_t kTagLength = 8;

// The test seam of journal_hooks.hpp: runs between clear()'s listing and its deletions.
std::function<void()>& clearHook() {
    static std::function<void()> hook;
    return hook;
}

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

fs::path fromUtf8(std::string_view s) {
    return {std::u8string(s.begin(), s.end())};
}

// A project-relative path with '/' separators that stays inside the project folder.
bool isSafeRelativePath(std::string_view path) noexcept {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos || path.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string_view segment = path.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

// A session's file-name tag: the first eight hex digits of its id.
std::string sessionTag(const Uuid& session) {
    return session.str().substr(0, kTagLength);
}

struct EntryName {
    std::uint64_t seq = 0;
    std::string_view tag; // empty for the untagged "<seq>.json" of journals written before D-190
};

// An entry file name "<seq>-<8 lower-case hex>.json" or "<seq>.json"; nullopt for every other name.
std::optional<EntryName> parseEntryName(std::string_view name) noexcept {
    if (name.size() <= kJsonSuffix.size() || !name.ends_with(kJsonSuffix)) {
        return std::nullopt;
    }
    std::string_view stem = name.substr(0, name.size() - kJsonSuffix.size());
    EntryName out;
    if (const std::size_t dash = stem.find('-'); dash != std::string_view::npos) {
        out.tag = stem.substr(dash + 1);
        stem = stem.substr(0, dash);
        if (out.tag.size() != kTagLength || !std::all_of(out.tag.begin(), out.tag.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            })) {
            return std::nullopt;
        }
    }
    if (stem.empty() || stem.size() > 19) {
        return std::nullopt;
    }
    for (const char c : stem) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        out.seq = out.seq * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return out;
}

// The seq a file sorts by when the journal is cleared: its name's, else 0 (damaged or foreign names go
// first).
std::uint64_t seqForOrder(std::string_view name) noexcept {
    const auto parsed = parseEntryName(name);
    return parsed ? parsed->seq : 0;
}

// Removes the folders an interrupted clear(All) renamed away (nothing reads them any more).
void removeAbandonedClears(const fs::path& projectFolder) {
    std::error_code ec;
    for (fs::directory_iterator it(projectFolder, ec), end; !ec && it != end; it.increment(ec)) {
        if (utf8(it->path().filename()).starts_with(kClearingPrefix)) {
            std::error_code re;
            fs::remove_all(it->path(), re);
        }
    }
}

struct EntryFile {
    fs::path path;
    std::string name;
};

// The *.json files of the journal directory (an absent directory is empty). Io when it cannot be listed.
Result<std::vector<EntryFile>> listEntryFiles(const fs::path& dir) {
    std::vector<EntryFile> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return out;
    }
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        const std::string name = utf8(it->path().filename());
        if (it->is_regular_file(fe) && name.size() > kJsonSuffix.size() && name.ends_with(kJsonSuffix)) {
            out.push_back({it->path(), name});
        }
    }
    if (ec) {
        ErrorContext ctx;
        ctx.path = dir;
        return fail(ErrorCode::Io, "cannot list the journal: " + ec.message(), std::move(ctx));
    }
    std::sort(out.begin(), out.end(), [](const EntryFile& a, const EntryFile& b) { return a.name < b.name; });
    return out;
}

Hash128 readHash(const json& j, const std::string& pointer) {
    const std::string& text = asString(j, pointer);
    const auto h = model::parseHashText(text);
    if (!h) {
        violate(pointer, "expected \"xxh3:\" and 32 lower-case hex digits, found '" + text + "'");
    }
    return *h;
}

json journalToJson(const JournalFile& f) {
    json root = detail::newDocument("journal", std::max(f.schemaVersion, kJournalKind.current));
    root["seq"] = f.seq;
    root["at"] = f.at;
    root["session"] = f.session.str();
    json list = json::array();
    for (const JournalEntry& e : f.entries) {
        json j = json::object();
        j["path"] = e.path;
        j["baseHash"] = e.baseHash ? json(model::hashText(*e.baseHash)) : json(nullptr);
        j["hash"] = model::hashText(e.hash);
        j["document"] = jsonio::fromDoc(e.document);
        list.push_back(std::move(j));
    }
    root["entries"] = std::move(list);
    detail::putUnknown(root, f.unknown);
    return root;
}

// A journal file as read: its document when it parses (nullopt otherwise), its name and time.
struct ReadFile {
    std::filesystem::path path;
    std::string name;
    std::optional<JournalFile> file;
    std::string error;
    std::uint64_t seq = 0; // of the parsed file
    std::int64_t atUs = 0;
};

// Every *.json file of the journal directory, parsed. Io when the directory or a file cannot be read.
Result<std::vector<ReadFile>> readFiles(const fs::path& dir) {
    auto files = listEntryFiles(dir);
    if (!files) {
        return Failure{files.error()};
    }
    std::vector<ReadFile> out;
    for (const EntryFile& f : *files) {
        auto text = jsonio::readFile(f.path);
        if (!text) {
            return Failure{text.error()};
        }
        ReadFile r;
        r.path = f.path;
        r.name = f.name;
        std::vector<std::string> log;
        auto parsed = JournalFile::parse(*text, log);
        if (parsed) {
            r.seq = parsed->seq;
            r.atUs = model::DateTime::parseIso(parsed->at).value_or(model::DateTime{}).unixUs;
            r.file = std::move(parsed).value();
        } else {
            r.error = parsed.error().message;
        }
        out.push_back(std::move(r));
    }
    return out;
}

// Whether a read file belongs to `which` sessions relative to `own` (an unreadable file is another's).
bool selected(const ReadFile& f, JournalSessions which, const Uuid& own) {
    switch (which) {
    case JournalSessions::All:
        return true;
    case JournalSessions::Own:
        return f.file.has_value() && f.file->session == own;
    case JournalSessions::Others:
        return !f.file.has_value() || f.file->session != own;
    }
    return true;
}

bool newerFirst(const ReadFile& a, const ReadFile& b) {
    if (a.seq != b.seq) {
        return a.seq > b.seq;
    }
    if (a.atUs != b.atUs) {
        return a.atUs > b.atUs;
    }
    return a.name > b.name;
}

std::string pathsList(const std::set<std::string>& paths) {
    std::string out;
    for (const std::string& p : paths) {
        out += out.empty() ? "" : ", ";
        out += p;
    }
    return out;
}

} // namespace

Result<JournalEntry> JournalEntry::of(std::string entryPath, std::string_view canonicalText,
                                      std::optional<Hash128> base) {
    if (!isSafeRelativePath(entryPath)) {
        ErrorContext ctx;
        ctx.detail = entryPath;
        return fail(ErrorCode::Invalid,
                    "journal: '" + entryPath + "' is not a path inside the project folder", std::move(ctx));
    }
    auto doc = JsonDoc::parse(canonicalText);
    if (!doc) {
        return Failure{doc.error()};
    }
    JournalEntry e;
    e.path = std::move(entryPath);
    e.baseHash = base;
    e.hash = hash128(doc->canonical());
    e.document = std::move(doc).value();
    return e;
}

Result<JournalFile> JournalFile::parse(std::string_view json, std::vector<std::string>& log) {
    return detail::readDocument<JournalFile>(
        json, kJournalKind, log,
        [&](const detail::json& root, const detail::DocVersion& version) -> Result<JournalFile> {
            ObjectReader r(root, "");
            detail::markSchemaHeader(r);
            JournalFile f;
            f.schemaVersion = version.write;
            const std::int64_t seqValue = jsonio::asInt(r.required("seq"), r.child("seq"));
            if (seqValue < 1) {
                violate(r.child("seq"), "seq is at least 1");
            }
            f.seq = static_cast<std::uint64_t>(seqValue);
            f.at = asString(r.required("at"), r.child("at"));
            if (!model::DateTime::parseIso(f.at)) {
                violate(r.child("at"), "at is an ISO 8601 date-time with an offset, found '" + f.at + "'");
            }
            f.session = jsonio::asUuid(r.required("session"), r.child("session"));
            const detail::json& entriesJson = r.required("entries");
            const std::string ep = r.child("entries");
            if (!entriesJson.is_array()) {
                violate(ep, "entries is an array");
            }
            for (std::size_t i = 0; i < entriesJson.size(); ++i) {
                ObjectReader er(entriesJson[i], jsonio::pointerIndex(ep, i));
                JournalEntry e;
                e.path = asString(er.required("path"), er.child("path"));
                if (!isSafeRelativePath(e.path)) {
                    violate(er.child("path"), "'" + e.path + "' is not a path inside the project folder");
                }
                // baseHash is required and may be null (a new file), which ObjectReader reads as absent.
                const auto base = er.value().find("baseHash");
                if (base == er.value().end()) {
                    violate(er.pointer(), "required member 'baseHash' is missing");
                }
                er.skip("baseHash");
                if (!base->is_null()) {
                    e.baseHash = readHash(*base, er.child("baseHash"));
                }
                e.hash = readHash(er.required("hash"), er.child("hash"));
                e.document = detail::objectDoc(er.required("document"), er.child("document"));
                er.closed();
                f.entries.push_back(std::move(e));
            }
            f.unknown = detail::unknownMembers(r);
            return f;
        },
        journalToJson);
}

Result<JournalFile> JournalFile::load(const std::filesystem::path& path, std::vector<std::string>& log) {
    return detail::loadDocument<JournalFile>(
        path, [&](std::string_view text) { return JournalFile::parse(text, log); });
}

std::string JournalFile::toJson() const {
    return detail::canonicalOrEmpty(journalToJson(*this));
}

Result<void> JournalFile::save(const std::filesystem::path& path) const {
    return detail::saveCanonical(journalToJson(*this), path);
}

namespace {

// Whether a journal file is `session`'s: a name tagged for another session is not (nothing is read);
// otherwise its content tells, and an unreadable file is another session's (as in replay()).
bool isOwnFile(const fs::path& path, std::string_view name, const Uuid& session) {
    if (const auto parsed = parseEntryName(name);
        parsed && !parsed->tag.empty() && parsed->tag != sessionTag(session)) {
        return false;
    }
    auto text = jsonio::readFile(path);
    if (!text) {
        return false;
    }
    std::vector<std::string> log;
    const auto file = JournalFile::parse(*text, log);
    return file && file->session == session;
}

} // namespace

namespace detail {

std::function<void()> setJournalClearHook(std::function<void()> hook) {
    return std::exchange(clearHook(), std::move(hook));
}

} // namespace detail

Journal::Journal(std::filesystem::path projectFolder, Uuid session)
    : folder_(std::move(projectFolder)), session_(session) {}

Result<void> Journal::append(const std::vector<JournalEntry>& entries,
                             std::chrono::system_clock::time_point now) {
    if (entries.empty()) {
        return ok();
    }
    JournalFile file;
    file.session = session_;
    std::set<std::string> seen;
    for (const JournalEntry& e : entries) {
        ErrorContext ctx;
        ctx.detail = e.path;
        if (!isSafeRelativePath(e.path)) {
            return fail(ErrorCode::Invalid,
                        "journal: '" + e.path + "' is not a path inside the project folder", std::move(ctx));
        }
        if (!seen.insert(e.path).second) {
            return fail(ErrorCode::Invalid, "journal: '" + e.path + "' is listed twice", std::move(ctx));
        }
        if (!e.document.isObject()) {
            return fail(ErrorCode::Invalid, "journal: the entry for '" + e.path + "' holds no document",
                        std::move(ctx));
        }
        const Hash128 hash = hash128(e.document.canonical());
        if (e.hash != Hash128{} && e.hash != hash) {
            return fail(ErrorCode::Invalid,
                        "journal: the hash of '" + e.path + "' does not match its document", std::move(ctx));
        }
        JournalEntry stamped = e;
        stamped.hash = hash;
        file.entries.push_back(std::move(stamped));
    }
    const fs::path dir = folder_ / fromUtf8(kDirName);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        ErrorContext ctx;
        ctx.path = dir;
        return fail(ErrorCode::Io, "cannot create the journal directory: " + ec.message(), std::move(ctx));
    }
    auto files = listEntryFiles(dir);
    if (!files) {
        return Failure{files.error()};
    }
    std::uint64_t seq = seq_;
    for (const EntryFile& f : *files) {
        if (const auto parsed = parseEntryName(f.name)) {
            seq = std::max(seq, parsed->seq + 1);
        }
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    file.seq = seq;
    file.at = model::DateTime{static_cast<std::int64_t>(us), 0}.iso();
    // The session tag: another session that read the same highest seq writes a name of its own (D-190).
    const std::string name = std::to_string(seq) + "-" + sessionTag(session_) + std::string(kJsonSuffix);
    if (auto saved = file.save(dir / fromUtf8(name)); !saved) {
        return saved;
    }
    seq_ = seq + 1;
    // The new file is this session's complete unsaved state: the older ones are superseded.
    for (const std::string& old : written_) {
        std::error_code re;
        fs::remove(dir / fromUtf8(old), re);
    }
    written_ = {name};
    return ok();
}

bool Journal::pending(JournalSessions which) const {
    auto files = listEntryFiles(folder_ / fromUtf8(kDirName));
    if (!files || files->empty()) {
        return false;
    }
    if (which == JournalSessions::All) {
        return true;
    }
    // A file with another session's tag is another session's without reading it; this session's tag and
    // untagged (pre-D-190) names are read to tell (an unreadable file is another session's).
    const bool wantOwn = which == JournalSessions::Own;
    return std::any_of(files->begin(), files->end(),
                       [&](const EntryFile& f) { return isOwnFile(f.path, f.name, session_) == wantOwn; });
}

bool Journal::ownFileExists() const {
    if (written_.empty()) {
        return false;
    }
    std::error_code ec;
    return fs::is_regular_file(folder_ / fromUtf8(kDirName) / fromUtf8(written_.back()), ec);
}

Result<JournalReplay> Journal::replay(JournalSessions which) const {
    JournalReplay out;
    auto files = readFiles(folder_ / fromUtf8(kDirName));
    if (!files) {
        return Failure{files.error()};
    }
    // Each session's files, newest first.
    struct Parsed {
        const ReadFile* read = nullptr;
        const JournalFile* file = nullptr;
    };
    std::map<Uuid, std::vector<Parsed>> sessions;
    for (const ReadFile& f : *files) {
        if (!selected(f, which, session_)) {
            continue;
        }
        if (!f.file) {
            out.log.push_back("journal: skipped " + f.name + ": " + f.error);
            ++out.skipped;
            continue;
        }
        sessions[f.file->session].push_back(Parsed{&f, &*f.file});
    }
    struct State {
        const ReadFile* newest = nullptr;
        std::map<std::string, JournalEntry> entries;
    };
    std::vector<State> states;
    for (auto& [session, list] : sessions) {
        std::sort(list.begin(), list.end(),
                  [](const Parsed& a, const Parsed& b) { return newerFirst(*a.read, *b.read); });
        State s;
        s.newest = list.front().read;
        // The newest file lists the session's unsaved documents; a damaged entry falls back to the same path
        // in an older file of the session.
        for (const JournalEntry& e : list.front().file->entries) {
            for (const Parsed& f : list) {
                const auto it = std::find_if(f.file->entries.begin(), f.file->entries.end(),
                                             [&](const JournalEntry& c) { return c.path == e.path; });
                if (it == f.file->entries.end()) {
                    continue;
                }
                if (hash128(it->document.canonical()) != it->hash) {
                    out.log.push_back("journal: skipped the entry for '" + it->path + "' in " + f.read->name +
                                      ": its hash does not match its document");
                    ++out.skipped;
                    continue;
                }
                s.entries.insert_or_assign(it->path, *it);
                break;
            }
        }
        states.push_back(std::move(s));
    }
    std::sort(states.begin(), states.end(),
              [](const State& a, const State& b) { return newerFirst(*b.newest, *a.newest); });
    std::map<std::string, JournalEntry> newest;
    for (State& s : states) {
        for (auto& [path, e] : s.entries) {
            if (newest.count(path) != 0) {
                out.log.push_back("journal: '" + path + "' was changed in two sessions; the newer, " +
                                  s.newest->name + ", is used");
            }
            newest.insert_or_assign(path, std::move(e));
        }
    }
    std::set<std::string> conflicts;
    std::set<std::string> applied;
    for (auto& [path, entry] : newest) {
        const fs::path onDisk = folder_ / fromUtf8(path);
        std::error_code ec;
        const bool exists = fs::is_regular_file(onDisk, ec);
        std::optional<Hash128> diskHash;
        if (exists) {
            auto bytes = jsonio::readFile(onDisk);
            if (!bytes) {
                return Failure{bytes.error()};
            }
            diskHash = hash128(*bytes);
        }
        if (diskHash == entry.hash) {
            // The file already holds this document: a save wrote it after journalling it (write-ahead) and
            // stopped before clearing the journal. Nothing to replay, and no conflict (D-190).
            applied.insert(path);
            continue;
        }
        if (diskHash != entry.baseHash) {
            conflicts.insert(path);
        }
        out.entries.push_back(std::move(entry));
    }
    out.conflicts.assign(conflicts.begin(), conflicts.end());
    out.applied.assign(applied.begin(), applied.end());
    if (!conflicts.empty()) {
        out.log.push_back("journal: changed on disk since the journal was written: " + pathsList(conflicts));
    }
    if (!applied.empty()) {
        out.log.push_back("journal: already saved, not replayed: " + pathsList(applied));
    }
    return out;
}

Result<void> Journal::clear(JournalSessions which) {
    const fs::path dir = folder_ / fromUtf8(kDirName);
    removeAbandonedClears(folder_);
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        written_.clear();
        return ok();
    }
    if (which == JournalSessions::All) {
        // Atomic: the journal leaves .journal/ in one rename and is deleted from there; a crash in between
        // leaves a folder nothing reads (removed by the next clear). Should the rename fail (a file in use),
        // the files are deleted one by one below.
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path away = folder_ / fromUtf8(std::string(kClearingPrefix) + sessionTag(session_) + "-" +
                                                 std::to_string(stamp));
        std::error_code re;
        fs::rename(dir, away, re);
        if (!re) {
            written_.clear();
            std::error_code rm;
            fs::remove_all(away, rm);
            return ok();
        }
    }
    // One listing decides everything deleted below: a file another session writes from here on (its newer
    // state, or a write in progress) is not in it and stays.
    struct Doomed {
        fs::path path;
        std::string name;
        bool temporary = false;
    };
    std::vector<Doomed> listed;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        const std::string name = utf8(it->path().filename());
        const bool entry = name.size() > kJsonSuffix.size() && name.ends_with(kJsonSuffix);
        const bool interrupted = name.find(kTempMarker) != std::string::npos;
        if ((entry || interrupted) && it->is_regular_file(fe)) {
            listed.push_back({it->path(), name, interrupted});
        }
    }
    if (ec) {
        ErrorContext ctx;
        ctx.path = dir;
        return fail(ErrorCode::Io, "cannot list the journal: " + ec.message(), std::move(ctx));
    }
    std::vector<Doomed> doomed;
    for (Doomed& f : listed) {
        bool gone = true; // All: every listed file
        if (f.temporary) {
            gone = which != JournalSessions::Own; // interrupted writes go with All and Others
        } else if (which != JournalSessions::All) {
            const bool own = std::find(written_.begin(), written_.end(), f.name) != written_.end() ||
                             isOwnFile(f.path, f.name, session_);
            gone = own == (which == JournalSessions::Own);
        }
        if (gone) {
            doomed.push_back(std::move(f));
        }
    }
    if (const std::function<void()>& hook = clearHook(); hook) {
        hook();
    }
    // Temporary files first, then oldest first: an interrupted clear leaves each session's newest file, its
    // complete state (D-187), never an older one that would replay over it (D-190).
    std::sort(doomed.begin(), doomed.end(), [](const Doomed& a, const Doomed& b) {
        if (a.temporary != b.temporary) {
            return a.temporary;
        }
        const std::uint64_t sa = seqForOrder(a.name);
        const std::uint64_t sb = seqForOrder(b.name);
        return sa != sb ? sa < sb : a.name < b.name;
    });
    for (const Doomed& d : doomed) {
        const fs::path& p = d.path;
        std::error_code re;
        fs::remove(p, re);
        if (re) {
            ErrorContext ctx;
            ctx.path = p;
            return fail(ErrorCode::Io, "cannot delete a journal file: " + re.message(), std::move(ctx));
        }
    }
    if (which != JournalSessions::Others) {
        written_.clear();
    }
    std::error_code rd;
    fs::remove(dir, rd); // only when empty; another file there is left alone
    return ok();
}

Result<std::optional<JournalReplay>>
Journal::recover(const std::function<JournalRecovery(const JournalReplay&)>& prompt) {
    if (!pending()) {
        return std::optional<JournalReplay>{};
    }
    auto replayed = replay();
    if (!replayed) {
        return Failure{replayed.error()};
    }
    if (replayed->entries.empty() && replayed->skipped == 0) {
        // Everything it holds is on disk already (a save stopped before clearing it): no prompt (D-190).
        if (auto cleared = clear(); !cleared) {
            return Failure{cleared.error()};
        }
        return std::optional<JournalReplay>{};
    }
    const JournalRecovery answer = prompt ? prompt(*replayed) : JournalRecovery::Later;
    switch (answer) {
    case JournalRecovery::Recover:
        return std::optional<JournalReplay>{std::move(replayed).value()};
    case JournalRecovery::Discard:
        if (auto cleared = clear(); !cleared) {
            return Failure{cleared.error()};
        }
        return std::optional<JournalReplay>{};
    case JournalRecovery::Later:
        break;
    }
    return std::optional<JournalReplay>{};
}

} // namespace esses::project
