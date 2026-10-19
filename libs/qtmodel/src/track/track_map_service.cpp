// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapService and TrackMapCommand (04 section 14 item 1, 06 section 13.5, 02 sections 6.9 and 7). Owner:
// the track work (src/track/). M4 (D-210 (a), M3 item 23): the reads, writes and scans of the trackmaps
// folder on the TaskPool when the service has a MathsService and a Dispatcher.

#include "../channel_resolution.hpp"
#include "../range_lookup.hpp"

#include <esses/base/future.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/utf8.hpp>
#include <esses/maths/whole_signal.hpp>
#include <esses/project/project_file.hpp>
#include <esses/project/track_map_file.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/track_map_service.hpp>

#include <QMetaObject>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <mutex>
#include <set>
#include <system_error>
#include <utility>
#include <variant>

namespace esses::qt {

namespace {

constexpr std::string_view kVenueDetail = "Venue";
constexpr std::string_view kSuffix = ".track.json";

QString fromUtf8(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

// <stem>.<yyyymmdd-hhmmss UTC>.removed.json in <folder>/backup/ (a "-2", "-3", ... suffix on a clash).
std::filesystem::path removedBackupPath(const std::filesystem::path& folder,
                                        const std::filesystem::path& file) {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char stamp[32] = {};
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &utc);
    std::string stem = pathToUtf8(file.filename());
    if (stem.size() > 5 && stem.ends_with(".json")) {
        stem.resize(stem.size() - 5);
    }
    const std::filesystem::path dir = folder / "backup";
    std::filesystem::path out = dir / pathFromUtf8(stem + "." + stamp + ".removed.json");
    for (int n = 2; std::filesystem::exists(out); ++n) {
        out = dir / pathFromUtf8(stem + "." + stamp + "-" + std::to_string(n) + ".removed.json");
    }
    return out;
}

// The generation of a venue file's newest write on disk: a write takes the mutex and writes only a newer
// generation, so a write that lands after a newer one (or after the flush of a destroyed service) never puts
// an older map back.
struct WriteSlot {
    std::mutex m;
    std::uint64_t written = 0;
};

struct PendingWrite {
    std::filesystem::path project; // the project folder when setMap queued it
    std::string venue;
    std::shared_ptr<const analysis::TrackMap> map; // nullptr: remove the file
    bool backup = false;                           // copy the previous file to backup/ first
    std::uint64_t generation = 0;                  // increasing per service
    std::uint64_t projectGeneration = 0;           // TrackMapService::Io::projectGeneration then
};

// One write of a venue's file (D-205): its own file (TrackMapFile::claimFor: an earlier build's file renamed
// to the hashed name first, Io when the name holds another venue's map), the previous file backed up when
// asked, a removal moved to <project>/backup/. A worker's job, or the calling thread's without a Dispatcher.
Result<void> writeTrackFile(WriteSlot& slot, const PendingWrite& w) {
    const std::lock_guard lock(slot.m);
    if (w.generation <= slot.written) {
        return ok(); // a newer map is on disk already
    }
    Result<std::filesystem::path> claimed =
        project::TrackMapFile::claimFor(w.project / std::string(project::TrackMapFile::kDirectory), w.venue);
    if (!claimed) {
        return Failure{claimed.error()};
    }
    const std::filesystem::path path = std::move(claimed).value();
    std::error_code ec;
    if (w.map == nullptr) {
        if (std::filesystem::exists(path, ec)) {
            const std::filesystem::path to = removedBackupPath(w.project, path);
            std::filesystem::create_directories(to.parent_path(), ec);
            std::filesystem::rename(path, to, ec);
            if (ec) {
                Error e{ErrorCode::Io, "cannot remove the track map: " + ec.message(), {}};
                e.context.path = path;
                return Failure{std::move(e)};
            }
        }
    } else {
        project::TrackMapFile file;
        file.map = *w.map;
        std::filesystem::create_directories(path.parent_path(), ec);
        const Result<void> saved =
            w.backup ? project::saveWithBackup(w.project, path, file.toJson()) : file.save(path);
        if (!saved) {
            return saved;
        }
    }
    slot.written = w.generation;
    return ok();
}

// The venues the trackmaps folder's files name whose own map TrackMapFile::loadFor finds (D-205), sorted.
std::vector<std::string> venuesInFolder(const std::filesystem::path& dir) {
    std::set<std::string> named; // the venues the folder's track files name
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec)) {
        for (const std::filesystem::directory_entry& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::string name = pathToUtf8(e.path().filename());
            if (!e.is_regular_file(ec) || !name.ends_with(kSuffix)) {
                continue;
            }
            std::vector<std::string> log;
            if (Result<project::TrackMapFile> f = project::TrackMapFile::load(e.path(), log)) {
                named.insert(f->map.venue);
            }
        }
    }
    std::vector<std::string> out;
    for (const std::string& venue : named) {
        std::vector<std::string> log;
        if (project::TrackMapFile::loadFor(dir, venue, log)) {
            out.push_back(venue);
        }
    }
    return out;
}

} // namespace

// ---- the pool side (M4, D-210 (a)) ----------------------------------------------------------------------

struct TrackMapCommand::Watch {
    TrackMapCommand* command = nullptr; // the command owning this watch (the watch dies with it)
};

struct TrackMapService::Loaded {
    std::shared_ptr<const analysis::TrackMap> map; // nullptr: absent or unreadable
    std::vector<std::string> log;                  // repair notes of TrackMapFile::loadFor
    std::optional<Error> error;                    // unreadable (NotFound is "absent")
};

struct TrackMapService::Io {
    TaskPool* pool = nullptr; // MathsService::pool(); null: synchronous
    std::uint64_t projectGeneration = 1;
    std::uint64_t nextTicket = 0;
    std::uint64_t nextGeneration = 0;
    std::uint64_t nextStamp = 0;
    std::map<std::string, std::uint64_t, std::less<>> stamps;  // venue -> mapStamp
    std::map<std::string, std::uint64_t, std::less<>> loading; // venue -> ticket of the read in flight
    std::map<std::string, std::vector<std::function<void()>>, std::less<>> waiting; // venue -> hand-overs
    struct Watched {
        std::uint64_t generation = 0;
        std::weak_ptr<TrackMapCommand::Watch> watch;
    };
    struct VenueWrites {
        std::shared_ptr<WriteSlot> slot = std::make_shared<WriteSlot>();
        std::optional<PendingWrite> inFlight;
        std::optional<PendingWrite> next; // merged edits held until inFlight lands
        std::vector<Watched> watched;     // the commands whose edits have not landed
    };
    std::map<std::string, VenueWrites, std::less<>> writes; // by "<project folder>\n<venue>"
    // The write the last setMap queued (key, generation): TrackMapCommand watches it.
    std::optional<std::pair<std::string, std::uint64_t>> lastQueued;
    bool scanning = false;
    std::uint64_t scanTicket = 0;
    std::optional<std::vector<std::string>> scanned; // the folder's venues as of the last scan
};

// ---- TrackMapService ------------------------------------------------------------------------------------

TrackMapService::TrackMapService(ProjectSession* session, MathsService* maths, Dispatcher* dispatcher,
                                 QObject* parent)
    : QObject(parent), session_(session), maths_(maths), dispatcher_(dispatcher),
      io_(std::make_unique<Io>()) {
    if (maths != nullptr && dispatcher != nullptr) {
        io_->pool = &maths->pool();
    }
    if (session != nullptr) {
        connect(session, &ProjectSession::projectOpened, this, &TrackMapService::reload);
        connect(session, &ProjectSession::projectClosed, this, &TrackMapService::reload);
    }
}

TrackMapService::~TrackMapService() {
    // The writes not on disk yet are made now, on this thread (exit: a queued write may be cancelled by the
    // pool's shutdown); one still running on a worker finishes first (the slot's mutex).
    for (auto& [key, v] : io_->writes) {
        std::optional<PendingWrite> newest = v.next ? v.next : v.inFlight;
        if (!newest) {
            continue;
        }
        if (v.next && v.inFlight) {
            newest->backup = newest->backup || v.inFlight->backup; // the in-flight one may not have run
        }
        static_cast<void>(writeTrackFile(*v.slot, *newest));
    }
}

bool TrackMapService::asynchronous() const noexcept {
    return io_->pool != nullptr && dispatcher_ != nullptr;
}

bool TrackMapService::isLoading(const std::string& venue) const {
    return io_->loading.contains(venue);
}

std::uint64_t TrackMapService::mapStamp(const std::string& venue) const {
    const auto it = io_->stamps.find(venue);
    return it != io_->stamps.end() ? it->second : 0;
}

int TrackMapService::pendingIo() const {
    std::size_t n = io_->loading.size() + (io_->scanning ? 1 : 0);
    for (const auto& [key, v] : io_->writes) {
        n += (v.inFlight ? 1 : 0) + (v.next ? 1 : 0);
    }
    return static_cast<int>(n);
}

std::string TrackMapService::venueOf(const model::DataSource& source) {
    for (const model::Detail& d : source.details) {
        if (d.id == kVenueDetail) {
            if (const auto* text = std::get_if<std::string>(&d.value)) {
                return *text;
            }
        }
    }
    return {};
}

std::optional<std::filesystem::path> TrackMapService::folder() const {
    if (session_.isNull() || !session_->isOpen()) {
        return std::nullopt;
    }
    return session_->folder();
}

std::filesystem::path TrackMapService::pathFor(const std::string& venue) const {
    const std::optional<std::filesystem::path> project = folder();
    if (!project) {
        return {};
    }
    return *project / std::string(project::TrackMapFile::kDirectory) /
           pathFromUtf8(project::TrackMapFile::fileNameFor(venue));
}

void TrackMapService::cache(const std::string& venue, std::shared_ptr<const analysis::TrackMap> map) {
    cache_[venue] = std::move(map);
    io_->stamps[venue] = ++io_->nextStamp;
}

std::shared_ptr<const analysis::TrackMap> TrackMapService::map(const std::string& venue) {
    if (const auto it = cache_.find(venue); it != cache_.end()) {
        return it->second;
    }
    const std::optional<std::filesystem::path> project = folder();
    if (project && asynchronous()) {
        startLoad(venue, *project); // nothing cached until it lands (mapChanged)
        return nullptr;
    }
    std::shared_ptr<const analysis::TrackMap> out;
    if (project) {
        // The venue's own file only (D-205): its hashed name, else the name of an earlier build, each when
        // the file's "venue" is exactly this venue, so a venue never shows another venue's map.
        std::vector<std::string> log;
        Result<project::TrackMapFile> file = project::TrackMapFile::loadFor(
            *project / std::string(project::TrackMapFile::kDirectory), venue, log);
        for (const std::string& line : log) {
            Q_EMIT logMessage(fromUtf8(line));
        }
        if (file) {
            ++fileReads_;
            out = std::make_shared<const analysis::TrackMap>(std::move(file->map));
        } else if (file.error().code != ErrorCode::NotFound) {
            ++fileReads_;
            const std::filesystem::path where = file.error().context.path.value_or(pathFor(venue));
            Q_EMIT logMessage(
                tr("The track map %1 could not be read: %2")
                    .arg(QString::fromStdWString(where.wstring()), fromUtf8(file.error().toString())));
        }
    }
    cache(venue, out); // an absent map is cached too (setMap or reload changes it)
    return out;
}

void TrackMapService::startLoad(const std::string& venue, const std::filesystem::path& project) {
    if (io_->loading.contains(venue)) {
        return;
    }
    const std::uint64_t ticket = ++io_->nextTicket;
    io_->loading.emplace(venue, ticket);
    const std::filesystem::path dir = project / std::string(project::TrackMapFile::kDirectory);
    Future<Loaded> future =
        io_->pool->submit(Priority::Interactive, TaskKey{}, CancelToken{}, [dir, venue](const CancelToken&) {
            Loaded out;
            Result<project::TrackMapFile> file = project::TrackMapFile::loadFor(dir, venue, out.log);
            if (file) {
                out.map = std::make_shared<const analysis::TrackMap>(std::move(file->map));
            } else if (file.error().code != ErrorCode::NotFound) {
                out.error = file.error();
            }
            return Result<Loaded>(std::move(out));
        });
    Dispatcher* dispatcher = dispatcher_; // the posting context (libs/qtmodel/README.md "Threading")
    const QPointer<TrackMapService> guard(this);
    future.onReady([dispatcher, guard, venue, ticket](const Result<Loaded>& result) {
        std::optional<Loaded> found;
        if (result) {
            found = *result; // a failure is the pool's shutdown: nothing was read
        }
        dispatcher->post(dispatcher, [guard, venue, ticket, found] {
            if (TrackMapService* self = guard.data()) {
                self->loaded(venue, ticket, found);
            }
        });
    });
}

void TrackMapService::loaded(const std::string& venue, std::uint64_t ticket,
                             const std::optional<Loaded>& found) {
    const auto it = io_->loading.find(venue);
    if (it == io_->loading.end() || it->second != ticket) {
        return; // superseded by a setMap or a project change: the cache holds something newer
    }
    io_->loading.erase(it);
    if (!found) {
        return; // not read (the pool shut down); a later map() reads it again
    }
    const Loaded& f = *found;
    for (const std::string& line : f.log) {
        Q_EMIT logMessage(fromUtf8(line));
    }
    if (f.map != nullptr || f.error) {
        ++fileReads_;
    }
    if (f.error) {
        const std::filesystem::path where = f.error->context.path.value_or(pathFor(venue));
        Q_EMIT logMessage(tr("The track map %1 could not be read: %2")
                              .arg(QString::fromStdWString(where.wstring()), fromUtf8(f.error->toString())));
    }
    cache(venue, f.map);
    Q_EMIT mapChanged(fromUtf8(venue)); // found or not: what was waiting for the read goes on
    venueKnown(venue);
}

std::shared_ptr<const analysis::TrackMap> TrackMapService::mapFor(const model::DataSource& source) {
    return map(venueOf(source));
}

Result<void> TrackMapService::setMap(const std::string& venue, std::shared_ptr<const analysis::TrackMap> map,
                                     bool backup) {
    io_->lastQueued.reset();
    if (map != nullptr) {
        if (Result<void> valid = map->validate(); !valid) {
            return valid;
        }
        if (map->venue != venue) {
            // The file binds the venue it is stored under; loadFor() reads it for that venue only (D-205).
            auto bound = std::make_shared<analysis::TrackMap>(*map);
            bound->venue = venue;
            map = std::move(bound);
        }
    }
    if (const std::optional<std::filesystem::path> project = folder()) {
        if (asynchronous()) {
            queueWrite(*project, venue, map, backup); // on the pool; a failure is reported when it lands
        } else {
            WriteSlot slot;
            const PendingWrite w{*project, venue, map, backup, 1, io_->projectGeneration};
            if (Result<void> r = writeTrackFile(slot, w); !r) {
                return r;
            }
        }
    }
    io_->loading.erase(venue); // a read still in flight is older than this map: its result is dropped
    cache(venue, std::move(map));
    Q_EMIT mapChanged(fromUtf8(venue));
    venueKnown(venue);
    return {};
}

void TrackMapService::queueWrite(const std::filesystem::path& project, const std::string& venue,
                                 std::shared_ptr<const analysis::TrackMap> map, bool backup) {
    const std::string key = pathToUtf8(project) + "\n" + venue;
    Io::VenueWrites& v = io_->writes[key];
    PendingWrite w{project, venue, std::move(map), backup, ++io_->nextGeneration, io_->projectGeneration};
    if (v.next) {
        w.backup = w.backup || v.next->backup; // the merged edits back up the file they all replace
    }
    io_->lastQueued = std::make_pair(key, w.generation);
    v.next = std::move(w);
    if (!v.inFlight) {
        submitWrite(key);
    }
}

void TrackMapService::submitWrite(const std::string& key) {
    Io::VenueWrites& v = io_->writes[key];
    if (!v.next) {
        return;
    }
    const PendingWrite w = std::move(*v.next);
    v.next.reset();
    v.inFlight = w;
    // Unkeyed: one write per file is in flight at a time and the slot orders them.
    Future<void> done = io_->pool->submit(
        Priority::Interactive, TaskKey{}, CancelToken{},
        [slot = v.slot, w](const CancelToken&) -> Result<void> { return writeTrackFile(*slot, w); });
    Dispatcher* dispatcher = dispatcher_;
    const QPointer<TrackMapService> guard(this);
    done.onReady([dispatcher, guard, key, generation = w.generation](const Result<void>& result) {
        dispatcher->post(dispatcher, [guard, key, generation, result = Result<void>(result)] {
            if (TrackMapService* self = guard.data()) {
                self->written(key, generation, result);
            }
        });
    });
}

void TrackMapService::written(const std::string& key, std::uint64_t generation, const Result<void>& result) {
    const auto it = io_->writes.find(key);
    if (it == io_->writes.end()) {
        return;
    }
    Io::VenueWrites& v = it->second;
    if (!v.inFlight || v.inFlight->generation != generation) {
        return;
    }
    const PendingWrite w = std::move(*v.inFlight);
    v.inFlight.reset();
    if (!result && result.error().code == ErrorCode::Cancelled) {
        // The pool shut down before it ran: held for the destructor's flush (nothing newer is lost).
        if (!v.next) {
            v.next = w;
        }
        return;
    }
    // The commands whose edits this write carried (every watched generation up to it).
    std::vector<std::shared_ptr<TrackMapCommand::Watch>> carried;
    std::erase_if(v.watched, [&](const Io::Watched& watched) {
        if (watched.generation > generation) {
            return false;
        }
        if (std::shared_ptr<TrackMapCommand::Watch> alive = watched.watch.lock()) {
            carried.push_back(std::move(alive));
        }
        return true;
    });
    if (!result) {
        Q_EMIT logMessage(tr("The track map of %1 could not be written: %2")
                              .arg(fromUtf8(w.venue), fromUtf8(result.error().toString())));
        for (const std::shared_ptr<TrackMapCommand::Watch>& watch : carried) {
            if (watch->command != nullptr) {
                // D-203 (n), asynchronously: no step that did not happen stays (QUndoStack drops an obsolete
                // command at its next undo or redo without running it).
                watch->command->lastError_ = result.error();
                watch->command->setObsolete(true);
            }
        }
        if (!v.next && w.projectGeneration == io_->projectGeneration) {
            // The cache shows what the file holds: the venue's map is read back.
            cache_.erase(w.venue);
            io_->stamps.erase(w.venue);
            if (const std::optional<std::filesystem::path> project = folder()) {
                startLoad(w.venue, *project);
            }
        }
    }
    if (v.next) {
        submitWrite(key);
    } else if (v.watched.empty()) {
        io_->writes.erase(it);
    }
}

QStringList TrackMapService::venues() {
    std::set<std::string> names;
    for (const auto& [venue, map] : cache_) {
        if (map != nullptr) {
            names.insert(venue);
        }
    }
    if (const std::optional<std::filesystem::path> project = folder()) {
        std::vector<std::string> inFolder;
        if (asynchronous()) {
            if (!io_->scanned && !io_->scanning) {
                startScan(*project);
            }
            if (io_->scanned) {
                inFolder = *io_->scanned;
            }
        } else {
            inFolder = venuesInFolder(*project / std::string(project::TrackMapFile::kDirectory));
        }
        // A venue counts when map() would find its map: the cache decides for a cached venue (above), the
        // rule of TrackMapFile::loadFor for the others (a file under another venue's name is not this venue's
        // map, D-205).
        for (const std::string& venue : inFolder) {
            if (!cache_.contains(venue)) {
                names.insert(venue);
            }
        }
    }
    QStringList out;
    for (const std::string& n : names) {
        out << fromUtf8(n);
    }
    return out;
}

void TrackMapService::startScan(const std::filesystem::path& project) {
    io_->scanning = true;
    const std::uint64_t ticket = ++io_->nextTicket;
    io_->scanTicket = ticket;
    const std::filesystem::path dir = project / std::string(project::TrackMapFile::kDirectory);
    Future<std::vector<std::string>> future =
        io_->pool->submit(Priority::Interactive, TaskKey{}, CancelToken{}, [dir](const CancelToken&) {
            return Result<std::vector<std::string>>(venuesInFolder(dir));
        });
    Dispatcher* dispatcher = dispatcher_;
    const QPointer<TrackMapService> guard(this);
    future.onReady([dispatcher, guard, ticket](const Result<std::vector<std::string>>& result) {
        std::vector<std::string> found;
        if (result) {
            found = *result;
        }
        dispatcher->post(dispatcher, [guard, ticket, found] {
            if (TrackMapService* self = guard.data()) {
                self->scanned(ticket, found);
            }
        });
    });
}

void TrackMapService::scanned(std::uint64_t ticket, const std::vector<std::string>& found) {
    if (!io_->scanning || io_->scanTicket != ticket) {
        return; // a project change dropped it
    }
    io_->scanning = false;
    io_->scanned = found;
    Q_EMIT venuesChanged();
}

void TrackMapService::venueKnown(const std::string& venue) {
    const auto it = io_->waiting.find(venue);
    if (it == io_->waiting.end()) {
        return;
    }
    std::vector<std::function<void()>> handOvers = std::move(it->second);
    io_->waiting.erase(it);
    for (std::function<void()>& h : handOvers) {
        QMetaObject::invokeMethod(this, std::move(h), Qt::QueuedConnection); // never inside the caller
    }
}

void TrackMapService::reload() {
    ++io_->projectGeneration; // reads and scans in flight belong to the previous project: dropped
    // Announced: the venues cached and those being read (whoever waits for a dropped read asks again).
    std::set<std::string> held;
    for (const auto& [venue, map] : cache_) {
        held.insert(venue);
    }
    for (const auto& [venue, ticket] : io_->loading) {
        held.insert(venue);
    }
    io_->loading.clear();
    io_->scanning = false;
    io_->scanned.reset();
    cache_.clear();
    io_->stamps.clear();
    for (const std::string& v : held) {
        Q_EMIT mapChanged(fromUtf8(v));
    }
    // What waits for a venue's map waits for it in this project now.
    std::vector<std::string> waitingFor;
    for (const auto& [venue, handOvers] : io_->waiting) {
        waitingFor.push_back(venue);
    }
    for (const std::string& venue : waitingFor) {
        static_cast<void>(map(venue));
        if (!isLoading(venue)) {
            venueKnown(venue);
        }
    }
}

// ---- generation (06 section 13.5 item 1) ------------------------------------------------------------------

namespace {

constexpr double kStandardGravity = 9.80665; // m/s/s per G

struct GenerationInput {
    store::Series series;
    units::Converter convert; // stored (base) unit -> what TrackGenerator reads
};

analysis::LapWindow windowOf(const model::Range& r) {
    return analysis::LapWindow{r.start, r.end};
}

} // namespace

TrackGenerationStatus TrackMapService::generate(const TrackGenerationRequest& request, QObject* context,
                                                OnGenerated onGui) {
    TrackGenerationStatus status;
    if (maths_.isNull() || dispatcher_ == nullptr) {
        status.reason = tr("Track generation needs the maths service");
        return status;
    }
    const std::shared_ptr<const model::DataSource> src = maths_->source(request.lap.src);
    const model::Range* lap = src == nullptr ? nullptr : detail::findRange(*src, request.lap);
    if (lap == nullptr) {
        status.reason = tr("The lap is not open");
        return status;
    }
    const bool gps = request.method == analysis::TrackMethod::Gps;
    if (!gps && request.method != analysis::TrackMethod::SpeedLatG) {
        status.reason = tr("No generation method");
        return status;
    }
    const std::string layer(model::canonicalLayerName(request.lap.layer));
    struct Need {
        std::string name;
        std::string unit; // the unit TrackGenerator reads
        bool lateral = false;
    };
    const std::vector<Need> needs =
        gps ? std::vector<Need>{{request.latitude, "deg", false}, {request.longitude, "deg", false}}
            : std::vector<Need>{{request.speed, "m/s", false}, {request.lateral, "m/s/s", true}};
    std::vector<GenerationInput> inputs;
    QStringList missing;
    bool pending = false;
    for (const Need& need : needs) {
        const plot::ResolvedChannel rc = detail::resolveChannel(
            *maths_, *src, layer, ChannelRequest{need.name, {}, std::nullopt}, Priority::Background);
        if (!rc.descriptor || rc.status == model::ChannelStatus::Error) {
            missing << QString::fromStdString(need.name);
            continue;
        }
        if (!rc.series || rc.status == model::ChannelStatus::Pending) {
            pending = true;
            continue;
        }
        // Stored unit -> the unit TrackGenerator reads, through the quantity's base unit (a unitless lateral
        // channel is in G; any other unitless channel is taken as it is).
        units::Converter convert;
        const units::UnitRegistry* registry = maths_->units().get();
        const units::UnitId stored = rc.descriptor->unit;
        if (registry != nullptr && stored != units::kNoUnit && registry->contains(stored)) {
            if (const auto target = registry->find(need.unit, registry->quantityOf(stored))) {
                convert = registry->toBase(stored).then(registry->fromBase(*target));
            }
        } else if (need.lateral) {
            convert = units::Converter{kStandardGravity, 0.0};
        }
        inputs.push_back(GenerationInput{*rc.series, convert});
    }
    if (!missing.isEmpty()) {
        status.reason = tr("Missing channels: %1").arg(missing.join(QStringLiteral(", ")));
        return status;
    }
    if (pending || inputs.size() != 2) {
        status.state = TrackGenerationState::Pending;
        status.reason = tr("Waiting for the channels to be calculated");
        return status;
    }
    analysis::TrackGeneratorOptions options;
    options.smoothingM = request.smoothingM;
    options.invertLatAccel = request.invertLatAccel;
    std::optional<analysis::LapWindow> inner;
    std::optional<analysis::LapWindow> outer;
    if (gps) {
        const std::optional<model::RangeRef>* refs[] = {&request.innerLap, &request.outerLap};
        std::optional<analysis::LapWindow>* windows[] = {&inner, &outer};
        for (std::size_t i = 0; i < 2; ++i) {
            const std::optional<model::RangeRef>& ref = *refs[i];
            if (!ref.has_value()) {
                continue;
            }
            const model::RangeRef& r = ref.value();
            const model::Range* range = r.src == request.lap.src ? detail::findRange(*src, r) : nullptr;
            if (range == nullptr) {
                status.reason = tr("The inner and outer laps must be laps of the same source");
                return status;
            }
            *windows[i] = windowOf(*range);
        }
    }
    const analysis::LapWindow window = windowOf(*lap);
    analysis::TrackGeneratedFrom from;
    from.method = request.method;
    from.source = "xxh3:" + detail::sourceHex(src->id);
    from.lap = lap->id.str();
    const std::string venue = venueOf(*src);
    const std::string key = "geom/track/" + detail::sourceHex(src->id) + "/" + lap->id.str();
    Future<analysis::GeneratedTrack> future = maths_->pool().submit(
        Priority::Background, TaskKey{key}, CancelToken{},
        [gps, inputs, options, window, inner,
         outer](const CancelToken& cancel) -> Result<analysis::GeneratedTrack> {
            Result<analysis::UniformSignal> a =
                maths::readWholeSignal(inputs[0].series, cancel, inputs[0].convert);
            if (!a) {
                return Failure{a.error()};
            }
            Result<analysis::UniformSignal> b =
                maths::readWholeSignal(inputs[1].series, cancel, inputs[1].convert);
            if (!b) {
                return Failure{b.error()};
            }
            analysis::TrackGeneratorOptions o = options;
            if (!gps) {
                return analysis::TrackGenerator::fromSpeedLatG(window, *a, *b, o, cancel);
            }
            if (inner.has_value()) {
                o.innerLap = analysis::GpsLap{inner.value(), *a, *b};
            }
            if (outer.has_value()) {
                o.outerLap = analysis::GpsLap{outer.value(), *a, *b};
            }
            return analysis::TrackGenerator::fromGps(analysis::GpsLap{window, *a, *b}, o, cancel);
        });
    if (asynchronous()) {
        static_cast<void>(map(venue)); // M4: the venue's file is read while the track is generated
    }
    Dispatcher* dispatcher = dispatcher_; // the posting context (libs/qtmodel/README.md "Threading")
    const QPointer<QObject> guard(context);
    const QPointer<TrackMapService> service(this);
    future.onReady([dispatcher, guard, service, onGui = std::move(onGui), venue,
                    from](const Result<analysis::GeneratedTrack>& result) {
        dispatcher->post(dispatcher, [guard, service, onGui, venue, from,
                                      result = Result<analysis::GeneratedTrack>(result)]() mutable {
            if (guard.isNull()) {
                return;
            }
            if (result) {
                result->map.name = venue.empty() ? std::string("Track") : venue;
                result->map.venue = venue;
                result->map.generatedFrom = from;
            }
            // M4 (D-210 (a)): handed over once the venue's own map is known, so a caller that generates
            // because mapFor() was null (the shell's auto-generation) never writes over a file still being
            // read.
            if (TrackMapService* self = service.data(); self != nullptr && self->asynchronous()) {
                static_cast<void>(self->map(venue));
                if (self->isLoading(venue)) {
                    self->io_->waiting[venue].push_back([guard, onGui, result]() mutable {
                        if (!guard.isNull()) {
                            onGui(std::move(result));
                        }
                    });
                    return;
                }
            }
            onGui(std::move(result));
        });
    });
    status.state = TrackGenerationState::Started;
    return status;
}

// ---- TrackMapCommand ------------------------------------------------------------------------------------

TrackMapCommand::TrackMapCommand(TrackMapService& service, std::string venue,
                                 std::shared_ptr<const analysis::TrackMap> before,
                                 std::shared_ptr<const analysis::TrackMap> after, const QString& text,
                                 bool backupFirst, QUndoCommand* parent)
    : QUndoCommand(text, parent), service_(&service), venue_(std::move(venue)), before_(std::move(before)),
      after_(std::move(after)), backupFirst_(backupFirst) {}

void TrackMapCommand::apply(const std::shared_ptr<const analysis::TrackMap>& map, bool backup) {
    lastError_.reset();
    if (service_.isNull()) {
        return;
    }
    if (Result<void> r = service_->setMap(venue_, map, backup); !r) {
        lastError_ = r.error();
        return;
    }
    // M4 (D-210 (a)): a write queued on the pool is watched, so its failure marks this command when it lands.
    TrackMapService::Io& io = *service_->io_;
    if (io.lastQueued) {
        if (watch_ == nullptr) {
            watch_ = std::make_shared<Watch>();
            watch_->command = this;
        }
        io.writes[io.lastQueued->first].watched.push_back(
            TrackMapService::Io::Watched{io.lastQueued->second, watch_});
    }
}

void TrackMapCommand::redo() {
    apply(after_, backupFirst_ && !redone_);
    redone_ = true;
    if (failed()) {
        setObsolete(
            true); // the stack drops a command whose write failed: nothing to undo or redo (D-203 (n))
    }
}

void TrackMapCommand::undo() {
    apply(before_, false);
    if (failed()) {
        setObsolete(true);
    }
}

} // namespace esses::qt
