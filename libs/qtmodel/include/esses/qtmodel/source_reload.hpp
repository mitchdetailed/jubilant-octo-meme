// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The wiring of changed source files (M4 reload package, D-208; 06 section 6 "Changed files", 02 section 1
// behaviour 5, AC-M4-10), beside connectSourceLoader (source_loader.hpp), whose file another package owns:
//   * SourceLoader::staleChanged(id, true) appends the source's Messages row (staleSourceMessage: Import,
//     Warning, code open.file_changed, "file changed on disk ...", action file.reload_source with the source
//     id as 32 hex digits), when a MessagesModel is given;
//   * SourceLoader::sourceReloaded(previous, source) restores the selection of the reloaded file
//     (DataSelectionModel::sourceReloaded: slots by RangeId, its filter and its Data-window position).
// The shell calls it once for its workspace (ReloadShell); tests call it beside connectSourceLoader. GUI
// thread only.
#pragma once

#include <esses/formats/source_file_check.hpp>
#include <esses/model/source.hpp>
#include <esses/qtmodel/messages_model.hpp>

#include <QString>
#include <optional>
#include <string_view>

namespace esses::qt {

class DataSelectionModel;
class SourceLoader;

// The command of the row's action and of the Data window's Reload File (06 section 14.4).
inline constexpr std::string_view kReloadSourceCommand = "file.reload_source";
// The code of the row (an Import Log code of the loader, 06 section 6).
inline constexpr std::string_view kFileChangedCode = "open.file_changed";

// The Messages row of a source whose file changed on disk (Changed) or can no longer be read (Missing):
// origin Import, Warning, source = the file's path, code open.file_changed, the action file.reload_source
// with [source id hex].
Message staleSourceMessage(const model::DataSource& source, formats::SourceFileState state);

// The stale source's id as the action's argument (32 lower-case hex digits) and back; nullopt for anything
// else.
QString sourceIdArgument(const model::SourceId& id);
std::optional<model::SourceId> parseSourceIdArgument(const QString& text);

// Wires `loader` to `selection` and (when not null) `messages` as described above.
void connectSourceReload(SourceLoader& loader, DataSelectionModel& selection, MessagesModel* messages);

} // namespace esses::qt
