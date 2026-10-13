// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SettingsStore (06 section 18, 02 section 6.9 settings-1, 01 section 9; D-055): per-user settings in
// <config>/settings.json, written atomically on change (coalesced 1 s): chunk-cache budget, default time
// format, active scheme, keymap name, MRU projects (10) and logs (20), importer overrides keyed by
// (extension, preamble fingerprint), UI flags, command-palette history. Unknown members are preserved. Dock
// perspectives of an open project live in <project>/.local/layout.json (layout-1), not here. GUI thread only.
// M1 details: "$schema" urn:esses:settings-1 and schemaVersion 1 are written on save; a file with another
// schema is Invalid and leaves the defaults; QJsonDocument writes members sorted. Pending changes are written
// by the destructor. A failed coalesced write is reported through saveFailed().
#pragma once

#include <esses/base/result.hpp>

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <filesystem>

class QTimer;

namespace esses::qt {

class SettingsStore final : public QObject {
    Q_OBJECT
public:
    // `file`: <config>/settings.json; a missing file is not an error (defaults).
    explicit SettingsStore(std::filesystem::path file, QObject* parent = nullptr);
    ~SettingsStore() override;

    Result<void> load();
    Result<void> saveNow();
    bool dirty() const noexcept { return dirty_; }
    // The coalescing delay of setValue writes (default 1000 ms; tests shorten it).
    void setSaveDelay(int ms);
    static constexpr int kRecentLogs = 20;
    static constexpr int kRecentProjects = 10;

    // Dotted keys into the settings-1 object ("scheme.active", "memory.chunkCacheBudgetBytes").
    QVariant value(const QString& key, const QVariant& fallback = {}) const;
    void setValue(const QString& key, const QVariant& value); // schedules the coalesced write

    QStringList recentLogs() const;
    void addRecentLog(const QString& path);
    QStringList recentProjects() const;
    void addRecentProject(const QString& path);
    const std::filesystem::path& file() const noexcept { return file_; }

Q_SIGNALS:
    void changed(const QString& key);
    void saveFailed(const QString& message);

private:
    QStringList recent(const QString& key) const;
    void addRecent(const QString& key, const QString& path, int cap);

    std::filesystem::path file_;
    QJsonObject root_;
    bool dirty_ = false;
    QTimer* saveTimer_ = nullptr;
};

} // namespace esses::qt
