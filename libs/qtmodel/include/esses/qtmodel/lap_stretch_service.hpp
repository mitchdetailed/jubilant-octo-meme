// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LapStretchService (D-016; 06 sections 2.1, 13.3 item 1 and 13.6 item 4; 07 section 5.1 "LapStretch feeding
// the Lap Editor stretch report and the toolbar toggle"): the lap-distance stretching state of a workspace.
// Each lap of a source's "Outings:Laps" group whose measured length (its unstretched DistanceIndex) lies
// within the tolerance of the track length gets the factor trackLength / length (analysis::LapStretch); the
// factors are published as Range::stretch through SourceLoader::setRangeStretch (derived, never persisted),
// which the distance chains, the domain mappers and the maths selection already multiply by. Corr Dist itself
// is never stretched. A lap whose index is still pending keeps its factor until the index arrives; nothing
// waits.
//
// The track length of a source is setTrackLength's, else its "Venue Length" detail (D-172: the track map's
// length replaces it when track maps arrive, M3). With a session the toggle and the tolerance are
// project.json options.lapStretch: the toolbar toggle (data.lap_stretch_toggle) is view state written without
// the undo stack, the tolerance an options edit (SetProjectOptionsCommand, undoable); without a session
// setSettings sets both. report() is the per-lap table of the Lap Editor's Stretch column and the Corrected
// Speed and Distance dialog. GUI thread only. Added to the outline in M2.
#pragma once

#include <esses/analysis/lap_stretch.hpp>
#include <esses/model/ids.hpp>
#include <esses/units/unit_registry.hpp>

#include <QObject>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class QTimer;

namespace esses::qt {

class DistanceService;
class ProjectSession;
class SourceLoader;

class LapStretchService final : public QObject {
    Q_OBJECT
public:
    LapStretchService(SourceLoader& loader, DistanceService& distance,
                      std::shared_ptr<const units::UnitRegistry> units, ProjectSession* session = nullptr,
                      QObject* parent = nullptr);
    ~LapStretchService() override;

    // The toolbar toggle: with a session, options.lapStretch.enabled (written without the undo stack).
    bool enabled() const noexcept { return settings_.enabled; }
    void setEnabled(bool on);
    void toggle() { setEnabled(!enabled()); }
    // Toggle and tolerance (a fraction). With a session they follow project.json and this sets only the
    // toggle there (the tolerance is an options edit).
    const analysis::LapStretchSettings& settings() const noexcept { return settings_; }
    void setSettings(analysis::LapStretchSettings settings);

    // The track length a source's laps are compared with: an explicit value (metres; nullopt hands it back to
    // the "Venue Length" detail); 0 or absent: no stretching for the source.
    void setTrackLength(const model::SourceId& source, std::optional<double> metres);
    std::optional<double> trackLength(const model::SourceId& source) const;

    // The per-lap report of a source's "Outings:Laps" in time order (a lap without an index reports length 0
    // and stretch 1).
    std::vector<analysis::LapStretchRow> report(const model::SourceId& source) const;
    // Recomputes and publishes the factors of a source now (otherwise done after every new snapshot, index
    // arrival and settings change, coalesced).
    void update(const model::SourceId& source);

Q_SIGNALS:
    void enabledChanged(bool enabled);
    void reportChanged(const esses::model::SourceId& source);

private:
    void scheduleUpdate(const model::SourceId& source);
    void scheduleAll();
    void readSession();

    SourceLoader& loader_;
    DistanceService& distance_;
    std::shared_ptr<const units::UnitRegistry> units_;
    ProjectSession* session_ = nullptr;
    analysis::LapStretchSettings settings_;
    std::map<model::SourceId, double> trackLengths_;
    std::set<model::SourceId> dirty_;
    QTimer* timer_ = nullptr;
};

} // namespace esses::qt
