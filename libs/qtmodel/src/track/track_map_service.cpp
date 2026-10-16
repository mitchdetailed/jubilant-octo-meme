// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapService and TrackMapCommand (04 section 14 item 1, 06 section 13.5, 02 sections 6.9 and 7). Owner:
// the track work (src/track/).

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

#include <algorithm>
#include <chrono>
#include <ctime>
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

} // namespace

// ---- TrackMapService ------------------------------------------------------------------------------------

TrackMapService::TrackMapService(ProjectSession* session, MathsService* maths, Dispatcher* dispatcher,
                                 QObject* parent)
    : QObject(parent), session_(session), maths_(maths), dispatcher_(dispatcher) {
    if (session != nullptr) {
        connect(session, &ProjectSession::projectOpened, this, &TrackMapService::reload);
        connect(session, &ProjectSession::projectClosed, this, &TrackMapService::reload);
    }
}

TrackMapService::~TrackMapService() = default;

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

std::shared_ptr<const analysis::TrackMap> TrackMapService::map(const std::string& venue) {
    if (const auto it = cache_.find(venue); it != cache_.end()) {
        return it->second;
    }
    std::shared_ptr<const analysis::TrackMap> out;
    const std::filesystem::path path = pathFor(venue);
    std::error_code ec;
    if (!path.empty() && std::filesystem::exists(path, ec)) {
        ++fileReads_;
        std::vector<std::string> log;
        Result<project::TrackMapFile> file = project::TrackMapFile::load(path, log);
        for (const std::string& line : log) {
            Q_EMIT logMessage(fromUtf8(line));
        }
        if (file) {
            out = std::make_shared<const analysis::TrackMap>(std::move(file->map));
        } else {
            Q_EMIT logMessage(
                tr("The track map %1 could not be read: %2")
                    .arg(QString::fromStdWString(path.wstring()), fromUtf8(file.error().toString())));
        }
    }
    cache_.emplace(venue, out); // an absent map is cached too (setMap or reload changes it)
    return out;
}

std::shared_ptr<const analysis::TrackMap> TrackMapService::mapFor(const model::DataSource& source) {
    return map(venueOf(source));
}

Result<void> TrackMapService::setMap(const std::string& venue, std::shared_ptr<const analysis::TrackMap> map,
                                     bool backup) {
    if (map != nullptr) {
        if (Result<void> valid = map->validate(); !valid) {
            return valid;
        }
    }
    if (const std::optional<std::filesystem::path> project = folder()) {
        const std::filesystem::path path = pathFor(venue);
        std::error_code ec;
        if (map == nullptr) {
            if (std::filesystem::exists(path, ec)) {
                const std::filesystem::path to = removedBackupPath(*project, path);
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
            file.map = *map;
            std::filesystem::create_directories(path.parent_path(), ec);
            const Result<void> written =
                backup ? project::saveWithBackup(*project, path, file.toJson()) : file.save(path);
            if (!written) {
                return written;
            }
        }
    }
    cache_[venue] = std::move(map);
    Q_EMIT mapChanged(fromUtf8(venue));
    return {};
}

QStringList TrackMapService::venues() {
    std::set<std::string> names;
    for (const auto& [venue, map] : cache_) {
        if (map != nullptr) {
            names.insert(venue);
        }
    }
    if (const std::optional<std::filesystem::path> project = folder()) {
        const std::filesystem::path dir = *project / std::string(project::TrackMapFile::kDirectory);
        std::error_code ec;
        if (std::filesystem::is_directory(dir, ec)) {
            for (const std::filesystem::directory_entry& e : std::filesystem::directory_iterator(dir, ec)) {
                const std::string name = pathToUtf8(e.path().filename());
                if (!e.is_regular_file(ec) || !name.ends_with(kSuffix)) {
                    continue;
                }
                std::vector<std::string> log;
                if (Result<project::TrackMapFile> f = project::TrackMapFile::load(e.path(), log)) {
                    // A file whose venue does not slug to its own name is not this venue's map.
                    if (project::TrackMapFile::fileNameFor(f->map.venue) == name) {
                        const auto it = cache_.find(f->map.venue);
                        if (it == cache_.end() || it->second != nullptr) {
                            names.insert(f->map.venue);
                        }
                    }
                }
            }
        }
    }
    QStringList out;
    for (const std::string& n : names) {
        out << fromUtf8(n);
    }
    return out;
}

void TrackMapService::reload() {
    std::vector<std::string> held;
    held.reserve(cache_.size());
    for (const auto& [venue, map] : cache_) {
        held.push_back(venue);
    }
    cache_.clear();
    for (const std::string& v : held) {
        Q_EMIT mapChanged(fromUtf8(v));
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
    Dispatcher* dispatcher = dispatcher_; // the posting context (libs/qtmodel/README.md "Threading")
    const QPointer<QObject> guard(context);
    future.onReady([dispatcher, guard, onGui = std::move(onGui), venue,
                    from](const Result<analysis::GeneratedTrack>& result) {
        dispatcher->post(dispatcher, [guard, onGui, venue, from,
                                      result = Result<analysis::GeneratedTrack>(result)]() mutable {
            if (guard.isNull()) {
                return;
            }
            if (result) {
                result->map.name = venue.empty() ? std::string("Track") : venue;
                result->map.venue = venue;
                result->map.generatedFrom = from;
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
