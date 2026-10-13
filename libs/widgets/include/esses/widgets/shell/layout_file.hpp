// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LayoutFile: the per-user layout document `layout-1` (02 section 6.7, 06 section 3.4, D-019, D-071):
// <project>/.local/layout.json for an open project. Holds the window geometry, the active workbook and
// worksheet, one dock perspective per workbook id plus "_default", the dock states and per-dock options
// (docks.values.allChannels, 06 section 8) and dialog geometries. Never shared, never archived, never
// undoable; written atomically (QSaveFile: temp + rename) on workbook switch, Reset Dock Windows, project
// close and exit, never by autosave; a missing or invalid file is not an error (the docks reset to the
// default perspective). Unknown members are preserved on save.
//
// Deviations: lives in esses_widgets (01/ARCHITECTURE_OUTLINE place layout.json under esses_qtmodel), as only
// the shell reads and writes it and the perspective bytes come from the widget-level DockHost. Without an
// open project (`esses -l <log>`) the same document is kept at <config>/layout.json, because settings-1 has
// only `window.geometry` for the no-project state. "dockBackend" records which DockHost backend wrote the
// perspectives ("ads" or "qdockwidget", D-153), and "adsVersion" the ADS release when it was the ADS backend
// (06 section 3.4; removed when another backend writes the file).
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <filesystem>
#include <optional>

namespace esses::widgets {

class LayoutFile {
public:
    static constexpr const char* kSchemaId = "urn:esses:layout-1";
    static constexpr const char* kDefaultPerspective = "_default";

    LayoutFile();

    // A missing file gives an empty document (true); an unreadable or invalid one gives an empty document
    // and false with `error` set (the caller reports it as an info row and resets the docks).
    static bool load(const std::filesystem::path& file, LayoutFile& out, QString* error = nullptr);
    // Atomic write; the parent directory (.local/) is created. False with `error` on failure.
    bool save(const std::filesystem::path& file, QString* error = nullptr) const;

    // Perspective bytes of a workbook id or kDefaultPerspective; empty when absent.
    QByteArray perspective(const QString& key) const;
    void setPerspective(const QString& key, const QByteArray& perspective);
    void removePerspective(const QString& key);

    QByteArray windowGeometry() const;
    bool windowMaximised() const;
    void setWindow(const QByteArray& geometry, bool maximised, const QString& screenName, double dpr);

    std::optional<QString> activeWorkbook() const;
    std::optional<QString> activeWorksheet() const;
    void setActive(const QString& workbookId, const QString& worksheetName);

    // docks.<id>.state (informational; the perspective is authoritative) and docks.<id>.<option>.
    void setDockState(const QString& id, const QString& state);
    QJsonObject dock(const QString& id) const;
    void setDockOption(const QString& id, const QString& option, const QJsonValue& value);

    // `adsVersion` empty: the member is removed (the perspectives were not written by ADS).
    void setVersions(const QString& appVersion, const QString& qtVersion, const QString& dockBackend,
                     const QString& adsVersion = {});
    QString appVersion() const;
    QString dockBackend() const;
    QString adsVersion() const;

    const QJsonObject& json() const noexcept { return root_; }

private:
    QJsonObject root_;
};

} // namespace esses::widgets
