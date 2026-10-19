// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MessagesModel (06 section 11, 01 section 8.3, P-7): the append-only Messages list with the session cap.

#include <esses/qtmodel/messages_model.hpp>

#include <utility>

namespace esses::qt {

// ---- MessagesModel --------------------------------------------------------------------------------------

MessagesModel::MessagesModel(QObject* parent) : QAbstractListModel(parent) {}

MessagesModel::~MessagesModel() = default;

int MessagesModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant MessagesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || static_cast<std::size_t>(index.row()) >= rows_.size()) {
        return {};
    }
    const Message& m = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
        return m.text;
    case SeverityRole:
        return static_cast<int>(m.severity);
    case OriginRole:
        return static_cast<int>(m.origin);
    case SourceRole:
        return m.source;
    case CodeRole:
        return m.code;
    case ActionRole:
        return m.action;
    case TimestampRole:
        return m.timestamp;
    case AltActionRole: // M4 (D-206)
        return m.altAction;
    default:
        return {};
    }
}

QHash<int, QByteArray> MessagesModel::roleNames() const {
    QHash<int, QByteArray> names = QAbstractListModel::roleNames();
    names.insert(SeverityRole, "severity");
    names.insert(OriginRole, "origin");
    names.insert(SourceRole, "source");
    names.insert(CodeRole, "code");
    names.insert(ActionRole, "action");
    names.insert(TimestampRole, "timestamp");
    names.insert(AltActionRole, "altAction");
    return names;
}

void MessagesModel::append(Message message) {
    if (!message.timestamp.isValid()) {
        message.timestamp = QDateTime::currentDateTime();
    }
    if (rows_.size() >= kSessionCap) {
        beginRemoveRows(QModelIndex(), 0, 0);
        rows_.pop_front();
        endRemoveRows();
    }
    const bool isError = message.severity == Message::Severity::Error;
    const int row = static_cast<int>(rows_.size());
    beginInsertRows(QModelIndex(), row, row);
    rows_.push_back(std::move(message));
    endInsertRows();
    if (isError && !sawError_) {
        sawError_ = true;
        Q_EMIT firstError();
    }
}

void MessagesModel::appendImportLog(const QString& source,
                                    const std::vector<formats::ImportLogEntry>& entries) {
    for (const formats::ImportLogEntry& e : entries) {
        Message m;
        m.origin = Message::Origin::Import;
        m.severity = e.level == formats::ImportLogEntry::Level::Error     ? Message::Severity::Error
                     : e.level == formats::ImportLogEntry::Level::Warning ? Message::Severity::Warning
                                                                          : Message::Severity::Info;
        m.source = source;
        m.code = QString::fromStdString(e.code);
        m.text = QString::fromStdString(e.message);
        append(std::move(m));
    }
}

void MessagesModel::clear() {
    beginResetModel();
    rows_.clear();
    endResetModel();
}

const Message& MessagesModel::at(int row) const {
    return rows_.at(static_cast<std::size_t>(row));
}

int MessagesModel::count(Message::Severity severity) const {
    int n = 0;
    for (const Message& m : rows_) {
        n += m.severity == severity ? 1 : 0;
    }
    return n;
}

} // namespace esses::qt
