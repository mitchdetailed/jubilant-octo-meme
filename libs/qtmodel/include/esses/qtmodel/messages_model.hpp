// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MessagesModel (06 section 11, 01 section 8.3, P-7; D-030): the append-only list behind the Messages dock:
// Message{timestamp, severity {Info, Warning, Error}, origin {Import, Maths, Budget, Project, System},
// source, text, action?}. The Import Log of every open (one group per file), maths diagnostics not tied to an
// item, chunk-cache budget warnings (D-011, including the open-time estimate), layout restore failures,
// scheme and keymap validation problems, migrations, selection-restore drops. Session cap 5,000 rows (oldest
// dropped, not persisted); no modal diagnostics. An `action` is a command id with arguments (rendered as a
// link). GUI thread only; the Log sink that feeds it posts through the Dispatcher (makeMessagesLogSink).
#pragma once

#include <esses/base/log.hpp>
#include <esses/formats/import_log.hpp>

#include <QAbstractListModel>
#include <QDateTime>
#include <QString>
#include <QVariant>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace esses::qt {

class Dispatcher;

struct Message {
    enum class Severity : std::uint8_t { Info, Warning, Error };
    enum class Origin : std::uint8_t { Import, Maths, Budget, Project, System };
    QDateTime timestamp;
    Severity severity = Severity::Info;
    Origin origin = Origin::System;
    QString source; // file or document
    QString text;
    QString code;   // stable code (Import Log code, maths diagnostic code), may be empty
    QString action; // command id, may be empty
    QVariantList actionArgs;
};

class MessagesModel final : public QAbstractListModel {
    Q_OBJECT
public:
    static constexpr std::size_t kSessionCap = 5000;
    enum Role {
        SeverityRole = Qt::UserRole + 1,
        OriginRole,
        SourceRole,
        CodeRole,
        ActionRole,
        TimestampRole
    };

    explicit MessagesModel(QObject* parent = nullptr);
    ~MessagesModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void append(Message message);
    // One row per Import Log entry of an open (origin Import, source = the file).
    void appendImportLog(const QString& source, const std::vector<formats::ImportLogEntry>& entries);
    void clear();
    const Message& at(int row) const;
    int count(Message::Severity severity) const;

Q_SIGNALS:
    // The first Error of a session (the dock auto-shows unless Options say otherwise).
    void firstError();

private:
    std::deque<Message> rows_;
    bool sawError_ = false;
};

// The developer log's records at or above `minimum` as Messages rows (origin System, source = the log
// category; 01 section 8.3), except the "store.cache" records (the chunk-cache budget warning, which the
// shell's ChunkCache budget hook posts as one Budget row): a thread-safe ILogSink that posts each record
// through the Dispatcher. Made on
// the GUI thread; register it with AppRuntime::addSink so that shutdown removes it before the Dispatcher
// closes.
std::shared_ptr<ILogSink> makeMessagesLogSink(MessagesModel& messages, Dispatcher& dispatcher,
                                              LogLevel minimum = LogLevel::Warning);

} // namespace esses::qt
