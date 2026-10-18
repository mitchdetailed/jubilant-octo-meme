// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// connectProjectSession and SessionRestore (06 section 18 steps 5-6, 06 section 5.5; AC-M2-06).

#include <esses/base/utf8.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/session_restore.hpp>
#include <esses/qtmodel/source_loader.hpp>

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <utility>

namespace esses::qt {

void connectProjectSession(ProjectSession& session, MathsService* maths, SourceLoader* loader) {
    if (maths != nullptr && maths->units() != nullptr) {
        session.setUnits(maths->units());
    }
    const auto apply = [&session, m = QPointer<MathsService>(maths), l = QPointer<SourceLoader>(loader)] {
        if (MathsService* service = m.data()) {
            service->setCorrectedSpeedSettings(session.correctedSpeedSettings());
            service->setCorrectedSpeedEnabled(session.correctedSpeedEnabled());
            service->setCompat(session.mathsCompat());
            service->setResolverTables(session.mappingTable(), session.aliasTable());
            service->setModules(session.mathsModules());
        }
        if (SourceLoader* sourceLoader = l.data()) {
            sourceLoader->setChannelDocuments(session.mappingTable(), session.aliasTable(),
                                              session.unitMaps());
        }
    };
    QObject::connect(&session, &ProjectSession::documentsChanged, &session, apply);
    // An open started for the project being left neither publishes nor writes its mapping into the next one
    // (D-190): the opens in flight and those waiting for the Mapping panel are cancelled.
    const auto leave = [l = QPointer<SourceLoader>(loader)] {
        if (SourceLoader* sourceLoader = l.data()) {
            sourceLoader->cancelOpens();
        }
    };
    QObject::connect(&session, &ProjectSession::projectOpened, &session, leave);
    QObject::connect(&session, &ProjectSession::projectClosed, &session, leave);
    apply();
}

SessionRestore::SessionRestore(ProjectSession& session, SourceLoader& loader, DataSelectionModel& selection,
                               QObject* parent)
    : QObject(parent), session_(session), loader_(loader), selection_(selection) {
    // Each check runs after every receiver of the loader's signal (the selection first of all) has run.
    const auto later = [this] {
        if (running()) {
            QTimer::singleShot(0, this, &SessionRestore::check);
        }
    };
    connect(&loader_, &SourceLoader::sourcePublished, this, later);
    connect(&loader_, &SourceLoader::openFailed, this, later);
    connect(&loader_, &SourceLoader::importLog, this, later); // a second open of an open file
    connect(&loader_, &SourceLoader::mappingRequired, this, later);
}

SessionRestore::~SessionRestore() = default;

std::vector<std::filesystem::path> SessionRestore::previousData() const {
    std::vector<std::filesystem::path> out;
    const std::optional<std::filesystem::path>& folder = session_.folder();
    for (const project::RecentSource& r : session_.projectFile().recentSources) {
        std::filesystem::path p = pathFromUtf8(r.path);
        if (p.is_relative() && folder) {
            p = *folder / p;
        }
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) {
            out.push_back(p.lexically_normal());
        }
    }
    return out;
}

int SessionRestore::start(std::vector<std::filesystem::path> logs) {
    keys_ = session_.projectFile().lastSelection; // captured: every selection change rewrites it
    if (logs.empty()) {
        const JsonDoc& options = session_.projectFile().options;
        const QJsonObject general =
            options.isObject() ? QJsonDocument::fromJson(QByteArray::fromStdString(options.compact()))
                                     .object()
                                     .value(QStringLiteral("general"))
                                     .toObject()
                               : QJsonObject();
        if (general.value(QStringLiteral("loadPreviousData")).toBool(true)) {
            logs = previousData();
        }
    }
    for (const std::filesystem::path& p : logs) {
        loader_.open(p);
    }
    QTimer::singleShot(0, this, &SessionRestore::check);
    return static_cast<int>(logs.size());
}

void SessionRestore::check() {
    if (!keys_ || loader_.pendingOpens() > 0 || loader_.pendingMappings() > 0) {
        return;
    }
    const project::SelectionKeys keys = std::move(*keys_);
    keys_.reset();
    selection_.restore(keys);
    Q_EMIT finished();
}

} // namespace esses::qt
