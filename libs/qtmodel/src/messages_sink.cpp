// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Log sink behind the Messages dock (01 sections 8.1 and 8.3): records from any thread reach the
// MessagesModel through the Dispatcher.

#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/messages_model.hpp>

#include <QDateTime>
#include <QPointer>
#include <chrono>
#include <string_view>
#include <utility>

namespace esses::qt {

namespace {

class MessagesLogSink final : public ILogSink {
public:
    MessagesLogSink(MessagesModel& messages, Dispatcher& dispatcher, LogLevel minimum)
        : messages_(&messages), dispatcher_(&dispatcher), minimum_(minimum) {}

    void log(const LogRecord& record) override {
        if (record.level < minimum_ || record.level == LogLevel::Off) {
            return;
        }
        if (record.category == kCacheCategory) {
            return; // the chunk-cache budget warning: the shell's budget hook posts it as a Budget row
        }
        Message m;
        m.severity = record.level >= LogLevel::Error     ? Message::Severity::Error
                     : record.level >= LogLevel::Warning ? Message::Severity::Warning
                                                         : Message::Severity::Info;
        m.origin = Message::Origin::System;
        m.source = QString::fromUtf8(record.category.data(), static_cast<qsizetype>(record.category.size()));
        m.text = QString::fromStdString(record.message);
        m.timestamp = QDateTime::fromMSecsSinceEpoch(
            std::chrono::duration_cast<std::chrono::milliseconds>(record.time.time_since_epoch()).count());
        // The guard was made on the GUI thread (constructor); copies of it are thread-safe.
        const QPointer<MessagesModel> guard = messages_;
        dispatcher_->post(dispatcher_, [guard, m = std::move(m)]() mutable {
            if (MessagesModel* model = guard.data()) {
                model->append(std::move(m));
            }
        });
    }

private:
    static constexpr std::string_view kCacheCategory = "store.cache";
    QPointer<MessagesModel> messages_;
    Dispatcher* dispatcher_;
    LogLevel minimum_;
};

} // namespace

std::shared_ptr<ILogSink> makeMessagesLogSink(MessagesModel& messages, Dispatcher& dispatcher,
                                              LogLevel minimum) {
    return std::make_shared<MessagesLogSink>(messages, dispatcher, minimum);
}

} // namespace esses::qt
