// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The project wiring of a workspace and steps 5-6 of the startup sequence (06 section 18; AC-M2-06).
//
// connectProjectSession: the project's documents reach the services on every documentsChanged() (and at
// once): the maths modules in project order, options.maths.compat, the mappings and aliases (resolver tables)
// and the built-in corrected speed settings reach MathsService; the mappings, aliases and unit maps reach
// SourceLoader's step 4. The session shares the maths unit table. Either service may be null.
//
// SessionRestore: after a project opens, start() opens the given logs (the -l arguments) or, with none and
// options.general.loadPreviousData on, the project's recentSources (paths relative to the project folder),
// each through the SourceLoader pipeline (the Mapping panel when needed); once every open has published or
// failed and neither a Mapping panel nor a memory budget warning (M4, D-207: Continue or Cancel) is pending,
// the selection is restored from the lastSelection captured at
// start() (06 section 5.5: by (hash, layer, groupPath, startMarker), then (path, layer, groupPath, name);
// unresolved keys are dropped with an info row). The Data-window filters and the single-colour state are
// restored by DataSelectionModel itself. GUI thread only. Added to the outline in M2.
#pragma once

#include <esses/project/project_file.hpp>

#include <QObject>
#include <filesystem>
#include <optional>
#include <vector>

namespace esses::qt {

class DataSelectionModel;
class MathsService;
class ProjectSession;
class SourceLoader;

void connectProjectSession(ProjectSession& session, MathsService* maths, SourceLoader* loader);

class SessionRestore final : public QObject {
    Q_OBJECT
public:
    SessionRestore(ProjectSession& session, SourceLoader& loader, DataSelectionModel& selection,
                   QObject* parent = nullptr);
    ~SessionRestore() override;

    // Starts steps 5-6; returns the number of opens started (0: the selection is restored at once).
    int start(std::vector<std::filesystem::path> logs = {});
    bool running() const noexcept { return keys_.has_value(); }
    // The data of the project's recentSources that exist (absolute paths, newest first).
    std::vector<std::filesystem::path> previousData() const;

Q_SIGNALS:
    void finished();

private:
    void check();

    ProjectSession& session_;
    SourceLoader& loader_;
    DataSelectionModel& selection_;
    std::optional<project::SelectionKeys> keys_;
};

} // namespace esses::qt
