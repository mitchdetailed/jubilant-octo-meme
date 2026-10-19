// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapService (04 section 14 item 1, 06 sections 9 and 13.5, 02 sections 6.9 and 7; D-027, D-061, D-199;
// the track work of M3): the workspace's track maps, one per venue, as the project documents
// trackmaps/<venue>.track.json (project::TrackMapFile; the file of a venue is TrackMapFile::fileNameFor, a
// slug and a hash of the exact venue, and holds that venue in its "venue" member, D-205). map(venue) loads a
// venue's file on first use (TrackMapFile::loadFor: never a file that holds another venue) and keeps it (an
// absent or unreadable file gives nullptr, the second with a Messages row through logMessage);
// mapFor(source) binds a source by its Venue detail (04 section 14 item 1). setMap() is the one write path:
// it writes the file at once (the previous file copied to <project>/backup/ by project::saveWithBackup when
// asked, the Track Editor's Accept, 06 section 13.5 item 2), replaces the cached map and emits mapChanged;
// nullptr deletes the file (moved to backup/); the file is TrackMapFile::claimFor's (an earlier build's file
// of the venue renamed to the hashed name first; Io when that name holds another venue's map, which is never
// overwritten). Without a project (no ProjectSession, or none open) the maps live in memory only. A project
// change (opened, closed) drops the cache and announces every venue it held.
// Consumers: RenderService::prepareInputs fills ComponentBuildInputs::trackMap from mapFor(Main source)
// (RenderService::setTrackMaps), SourceLoader builds the Track Sections groups from it (setTrackMaps), the
// Track Editor and the Track Map dock edit and show it.
// generate() is the track generation path of 06 section 13.5 item 1 (the Track Editor's Generate and the
// shell's auto-generation on open): on the GUI thread it resolves the lap's inputs through MathsService (GPS
// latitude/longitude as the esses.gps_corrections chain heads, so (0, 0) fixes are invalid; Corr Speed and
// the lateral-G channel for Speed+LatG; logged channels through the source's ChannelResolver), then a worker
// reads them in base units (Angle -> degrees, a unitless lateral channel read as G) and runs
// analysis::TrackGenerator (Background, replace key geom/track/<source>/<lap>); the result reaches `onGui`
// through the Dispatcher with the map's name and venue set from the source's Venue detail and generatedFrom
// filled (no sections: the caller runs SectionDetector::defaults when it accepts the map).
// M4 (D-210 (a), M3 item 23): with a MathsService and a Dispatcher (the application's service) every file
// operation runs on the TaskPool (MathsService::pool(), Interactive) and never on the GUI thread:
//   * map(venue) on a cache miss returns nullptr at once and reads the file on a worker (isLoading(venue)
//     meanwhile); the result is cached and mapChanged(venue) fires, found or not. A project change drops the
//     reads in flight.
//   * setMap(venue, map) validates and replaces the cached map at once (mapChanged, as before) and queues the
//     write: one write per venue file in flight, the next one held and merged (the newest map, backed up when
//     any merged edit asked for it), so the file always ends as the last setMap left it. A failed write is a
//     Messages row through logMessage, the venue's map is read back from its file (mapChanged), and the
//     TrackMapCommands whose edit it carried are marked failed and obsolete, so QUndoStack drops them at the
//     next undo or redo (D-203 (n), asynchronously). Writes not yet done when the service is destroyed are
//     made then, on the destroying thread (as SourceLoader's sidecars, D-190).
//   * venues() answers from the cache and the last scan of the trackmaps folder, which runs on a worker on
//     first use after a project change; venuesChanged() announces the scan.
//   * a generated map reaches `onGui` only once the venue's own map is known (read, or set), so a caller that
//     generates when mapFor() is null (the shell's auto-generation) never writes over a file that was still
//     being read.
// mapStamp(venue) identifies what map(venue) returns now (SourceLoader's phase-2 key, D-210 (d)). Without a
// Dispatcher (headless tools, tests) every read and write is synchronous on the calling thread, as in M3.
//
// TrackMapCommand: one undoable edit of a venue's map on ProjectSession::undoStack() (02 section 7: section
// add/edit/delete, rename, category edits and "regenerate" are each one command): redo() writes `after`,
// undo() `before` (nullptr: no map, the file removed), each through setMap. Only the first redo of an Accept
// copies the previous file to backup/. A refused edit (an invalid map, or any failed write without a
// Dispatcher) leaves the cache unchanged and failed() reports it at once; with a Dispatcher a failed write
// reports it when it fails (above).
// GUI thread only.
#pragma once

#include <esses/analysis/track_generator.hpp>
#include <esses/analysis/track_map.hpp>
#include <esses/base/result.hpp>
#include <esses/model/range.hpp>
#include <esses/model/source.hpp>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUndoCommand>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esses::qt {

class Dispatcher;
class MathsService;
class ProjectSession;

// One generation (06 section 13.5 item 1). The channel names are the defaults of the shipped settings
// (esses.gps_corrections latitude/longitude, the built-in Corr Speed) and the usual lateral-G name.
struct TrackGenerationRequest {
    model::RangeRef lap; // the source lap (any group; its source's Venue detail names the map)
    analysis::TrackMethod method = analysis::TrackMethod::Gps;
    std::optional<model::RangeRef> innerLap; // GPS only, the same source
    std::optional<model::RangeRef> outerLap;
    double smoothingM = 10.0;
    bool invertLatAccel = false;
    std::string latitude = "GPS Latitude";
    std::string longitude = "GPS Longitude";
    std::string speed = "Corr Speed";
    std::string lateral = "G Force Lat";
};

enum class TrackGenerationState : std::uint8_t {
    Started, // onGui follows
    Pending, // a maths input is being evaluated (requested now): try again on MathsService::channelReady
    Failed   // an input is missing, or the request is malformed; `reason` says which
};
struct TrackGenerationStatus {
    TrackGenerationState state = TrackGenerationState::Failed;
    QString reason;
};

class TrackMapService final : public QObject {
    Q_OBJECT
public:
    // `session` may be null (no project: maps in memory only); `maths` and `dispatcher` are needed by
    // generate(), and with both the files are read and written on MathsService::pool() (M4, D-210 (a)).
    explicit TrackMapService(ProjectSession* session, MathsService* maths = nullptr,
                             Dispatcher* dispatcher = nullptr, QObject* parent = nullptr);
    ~TrackMapService() override;

    using OnGenerated = std::function<void(Result<analysis::GeneratedTrack> track)>;
    // See the header comment; `onGui` runs on the GUI thread while `context` lives.
    TrackGenerationStatus generate(const TrackGenerationRequest& request, QObject* context,
                                   OnGenerated onGui);

    // The Venue detail of a source ("" when it has none; a source without a venue binds the "" map).
    static std::string venueOf(const model::DataSource& source);
    // The map of `venue`: cached, else read from the project's trackmaps folder (nullptr when absent or
    // unreadable). Never blocks on a worker; on the pool (asynchronous(): nullptr until mapChanged), else a
    // small synchronous file read the first time.
    std::shared_ptr<const analysis::TrackMap> map(const std::string& venue);
    std::shared_ptr<const analysis::TrackMap> mapFor(const model::DataSource& source);
    // <project>/trackmaps/<TrackMapFile::fileNameFor(venue)>, where setMap writes; empty without an open
    // project.
    std::filesystem::path pathFor(const std::string& venue) const;
    // Replaces the venue's map (nullptr removes it): the file is written (or moved to backup/) first, then
    // the cache changes and mapChanged fires. Io on a failed write or when the venue's file name holds
    // another venue's map, Invalid for a map that fails TrackMap::validate. `backup`: copy the previous file
    // to backup/ before overwriting it. A map with another `venue` is stored as a copy bound to `venue`.
    // asynchronous(): the cache changes at once and the write is queued on the pool (Invalid only; a failed
    // write is reported when it fails, see the header comment).
    Result<void> setMap(const std::string& venue, std::shared_ptr<const analysis::TrackMap> map,
                        bool backup = false);
    // The venues with a map, sorted: the cached ones, and those the project's trackmaps/*.track.json files
    // name (their "venue" member) whose map map() would find (asynchronous(): as of the last scan, which
    // the first call after a project change starts; venuesChanged() when it lands).
    QStringList venues();
    // Drops the cache (the next map() reads the files again) and announces the venues it held.
    void reload();
    // How many file reads map() did (tests: the cache works).
    int fileReads() const noexcept { return fileReads_; }

    // ---- M4 (D-210 (a), M3 item 23): file operations on the pool ----------------------------------------
    // Whether the files are read and written on the TaskPool (a MathsService and a Dispatcher were given).
    bool asynchronous() const noexcept;
    // Whether the venue's file is being read (map(venue) gives nullptr until mapChanged(venue)).
    bool isLoading(const std::string& venue) const;
    // The identity of what map(venue) returns now: unique within this service, it changes whenever the
    // venue's cached map does (read, set, removed, read back, dropped by a project change); 0 while nothing
    // is cached for the venue.
    std::uint64_t mapStamp(const std::string& venue) const;
    // Reads, writes and scans not landed yet (tests settle on 0).
    int pendingIo() const;

Q_SIGNALS:
    void mapChanged(const QString& venue);
    void logMessage(const QString& text); // read problems and repair notes (Messages, origin Project)
    void venuesChanged();                 // M4: a scan of the trackmaps folder landed (venues() changed)

private:
    friend class TrackMapCommand;
    struct Io; // the pool side (track_map_service.cpp)

    std::optional<std::filesystem::path> folder() const;
    void cache(const std::string& venue, std::shared_ptr<const analysis::TrackMap> map);
    void startLoad(const std::string& venue, const std::filesystem::path& project);
    struct Loaded; // what a read on the pool found
    void loaded(const std::string& venue, std::uint64_t ticket, const std::optional<Loaded>& found);
    void queueWrite(const std::filesystem::path& project, const std::string& venue,
                    std::shared_ptr<const analysis::TrackMap> map, bool backup);
    void submitWrite(const std::string& key);
    void written(const std::string& key, std::uint64_t generation, const Result<void>& result);
    void startScan(const std::filesystem::path& project);
    void scanned(std::uint64_t ticket, const std::vector<std::string>& found);
    // Runs (queued) what waits for the venue's map to be known (generate's hand-over).
    void venueKnown(const std::string& venue);

    QPointer<ProjectSession> session_;
    QPointer<MathsService> maths_;
    Dispatcher* dispatcher_ = nullptr;                                                    // runtime-owned
    std::map<std::string, std::shared_ptr<const analysis::TrackMap>, std::less<>> cache_; // venue -> map
    int fileReads_ = 0;
    std::unique_ptr<Io> io_;
};

class TrackMapCommand final : public QUndoCommand {
public:
    TrackMapCommand(TrackMapService& service, std::string venue,
                    std::shared_ptr<const analysis::TrackMap> before,
                    std::shared_ptr<const analysis::TrackMap> after, const QString& text,
                    bool backupFirst = false, QUndoCommand* parent = nullptr);

    void redo() override;
    void undo() override;

    const std::string& venue() const noexcept { return venue_; }
    const std::shared_ptr<const analysis::TrackMap>& before() const noexcept { return before_; }
    const std::shared_ptr<const analysis::TrackMap>& after() const noexcept { return after_; }
    // The last redo()/undo() could not write the file (the error is in lastError()); the command then marks
    // itself obsolete, so QUndoStack drops it (pushed: not added; undone or redone: removed) and no step that
    // did not happen stays on the stack (D-203 (n)). Read failed() only before pushing or inside a parent.
    bool failed() const noexcept { return lastError_.has_value(); }
    const std::optional<Error>& lastError() const noexcept { return lastError_; }

    // M4 (D-210 (a)): what the service's write queue marks when this command's write fails on the pool.
    struct Watch;

private:
    friend class TrackMapService; // marks lastError_ when a queued write fails
    void apply(const std::shared_ptr<const analysis::TrackMap>& map, bool backup);

    QPointer<TrackMapService> service_;
    std::string venue_;
    std::shared_ptr<const analysis::TrackMap> before_;
    std::shared_ptr<const analysis::TrackMap> after_;
    bool backupFirst_ = false;
    bool redone_ = false;
    std::optional<Error> lastError_;
    std::shared_ptr<Watch> watch_;
};

} // namespace esses::qt
