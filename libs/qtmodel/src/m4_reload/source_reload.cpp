// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// connectSourceReload and the stale source's Messages row (source_reload.hpp; M4 reload package, D-208).

#include <esses/base/utf8.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/source_reload.hpp>

#include <QDateTime>
#include <QPointer>
#include <QVariantList>

namespace esses::qt {

namespace {

QString toQString(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

bool isLowerHex(QChar c) {
    return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
           (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
}

} // namespace

QString sourceIdArgument(const model::SourceId& id) {
    return QString::fromStdString(id.hex());
}

std::optional<model::SourceId> parseSourceIdArgument(const QString& text) {
    if (text.size() != 32) {
        return std::nullopt;
    }
    for (const QChar c : text) {
        if (!isLowerHex(c)) {
            return std::nullopt;
        }
    }
    bool hiOk = false;
    bool loOk = false;
    model::SourceId id;
    id.hi = text.left(16).toULongLong(&hiOk, 16); // hi first (Hash128::hex)
    id.lo = text.right(16).toULongLong(&loOk, 16);
    if (!hiOk || !loOk) {
        return std::nullopt;
    }
    return id;
}

Message staleSourceMessage(const model::DataSource& source, formats::SourceFileState state) {
    Message m;
    m.timestamp = QDateTime::currentDateTime();
    m.severity = Message::Severity::Warning;
    m.origin = Message::Origin::Import;
    m.source = QString::fromStdString(pathToUtf8(source.prov.path));
    m.code = toQString(kFileChangedCode);
    m.text =
        state == formats::SourceFileState::Missing
            ? QObject::tr("file changed on disk: it can no longer be read (deleted, moved, or its share "
                          "disconnected); nothing more is decoded from it, and Reload opens it again")
            : QObject::tr("file changed on disk after it was opened: nothing more is decoded from it, and "
                          "Reload opens it again with its edits and selection");
    m.action = toQString(kReloadSourceCommand);
    m.actionArgs = QVariantList{sourceIdArgument(source.id)};
    return m;
}

void connectSourceReload(SourceLoader& loader, DataSelectionModel& selection, MessagesModel* messages) {
    if (messages != nullptr) {
        QObject::connect(
            &loader, &SourceLoader::staleChanged, messages,
            [guard = QPointer<SourceLoader>(&loader), messages](const model::SourceId& id, bool stale) {
                SourceLoader* l = guard.data();
                const std::shared_ptr<const model::DataSource> src = l == nullptr ? nullptr : l->source(id);
                if (!stale || src == nullptr) {
                    return;
                }
                // What the check found (no second stat on the GUI thread, D-217).
                formats::SourceFileState state = l->staleState(id);
                if (state == formats::SourceFileState::Unchanged) {
                    state = formats::SourceFileState::Changed;
                }
                messages->append(staleSourceMessage(*src, state));
            });
    }
    QObject::connect(
        &loader, &SourceLoader::sourceReloaded, &selection,
        [sel = &selection](const model::SourceId& previous, std::shared_ptr<const model::DataSource> source) {
            sel->sourceReloaded(previous, std::move(source));
        });
}

} // namespace esses::qt
