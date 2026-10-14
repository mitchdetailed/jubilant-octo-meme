// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapService (04 section 14 item 1, 06 sections 9 and 13.5, 02 sections 6.9 and 7; D-027, D-061, D-199;
// the track work of M3): the workspace's track maps, one per venue, as the project documents
// trackmaps/<venue>.track.json (project::TrackMapFile). map(venue) loads a venue's file on first use and
// keeps it (an absent or unreadable file gives nullptr, the second with a Messages row through logMessage);
// mapFor(source) binds a source by its Venue detail (04 section 14 item 1). setMap() is the one write path:
// it writes the file at once (the previous file copied to <project>/backup/ by project::saveWithBackup when
// asked, the Track Editor's Accept, 06 section 13.5 item 2), replaces the cached map and emits mapChanged;
// nullptr deletes the file (moved to backup/). Without a project (no ProjectSession, or none open) the maps
// live in memory only. A project change (opened, closed) drops the cache and announces every venue it held.
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
//
// TrackMapCommand: one undoable edit of a venue's map on ProjectSession::undoStack() (02 section 7: section
// add/edit/delete, rename, category edits and "regenerate" are each one command): redo() writes `after`,
// undo() `before` (nullptr: no map, the file removed), each through setMap. Only the first redo of an Accept
// copies the previous file to backup/. A failed write leaves the cache unchanged; failed() reports it.
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
    // generate() only.
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
    // unreadable). Never blocks on a worker; a small synchronous file read the first time.
    std::shared_ptr<const analysis::TrackMap> map(const std::string& venue);
    std::shared_ptr<const analysis::TrackMap> mapFor(const model::DataSource& source);
    // <project>/trackmaps/<slug of venue>.track.json; empty without an open project.
    std::filesystem::path pathFor(const std::string& venue) const;
    // Replaces the venue's map (nullptr removes it): the file is written (or moved to backup/) first, then
    // the cache changes and mapChanged fires. Io on a failed write, Invalid for a map that fails
    // TrackMap::validate. `backup`: copy the previous file to backup/ before overwriting it.
    Result<void> setMap(const std::string& venue, std::shared_ptr<const analysis::TrackMap> map,
                        bool backup = false);
    // The venues with a map: the project's trackmaps/*.track.json files (by their "venue" member) and the
    // cached ones, sorted.
    QStringList venues();
    // Drops the cache (the next map() reads the files again) and announces the venues it held.
    void reload();
    // How many file reads map() did (tests: the cache works).
    int fileReads() const noexcept { return fileReads_; }

Q_SIGNALS:
    void mapChanged(const QString& venue);
    void logMessage(const QString& text); // read problems and repair notes (Messages, origin Project)

private:
    std::optional<std::filesystem::path> folder() const;

    QPointer<ProjectSession> session_;
    QPointer<MathsService> maths_;
    Dispatcher* dispatcher_ = nullptr;                                                    // runtime-owned
    std::map<std::string, std::shared_ptr<const analysis::TrackMap>, std::less<>> cache_; // venue -> map
    int fileReads_ = 0;
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
    // The last redo()/undo() could not write the file (the error is in lastError()).
    bool failed() const noexcept { return lastError_.has_value(); }
    const std::optional<Error>& lastError() const noexcept { return lastError_; }

private:
    void apply(const std::shared_ptr<const analysis::TrackMap>& map, bool backup);

    QPointer<TrackMapService> service_;
    std::string venue_;
    std::shared_ptr<const analysis::TrackMap> before_;
    std::shared_ptr<const analysis::TrackMap> after_;
    bool backupFirst_ = false;
    bool redone_ = false;
    std::optional<Error> lastError_;
};

} // namespace esses::qt
