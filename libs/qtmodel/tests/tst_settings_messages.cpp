// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SettingsStore (06 section 18, 02 section 6.9 settings-1; D-055) and MessagesModel (06 section 11, 01
// section 8.3): defaults without a file, dotted keys, the coalesced atomic write, unknown members preserved,
// the MRU caps, load errors; the 5,000-row cap, the first-error signal, Import Log rows and the Log sink.

#include "qtmodel_test_support.hpp"

#include <esses/base/log.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/messages_model.hpp>
#include <esses/qtmodel/settings_store.hpp>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <fstream>
#include <thread>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

namespace {

QJsonObject readJsonFile(const std::filesystem::path& p) {
    QFile f(qs(p.string()));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(f.readAll()).object();
}

void writeFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

} // namespace

class TstSettingsMessages : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void settingsDefaultsAndDottedKeys() {
        const auto file = scratchDir("settings-a") / "settings.json";
        SettingsStore s(file);
        QVERIFY(s.load().ok()); // a missing file is not an error
        QCOMPARE(s.value(QStringLiteral("scheme.active"), QStringLiteral("light")).toString(),
                 QStringLiteral("light"));
        QSignalSpy changed(&s, &SettingsStore::changed);
        s.setSaveDelay(20);
        s.setValue(QStringLiteral("scheme.active"), QStringLiteral("Esses Dark"));
        s.setValue(QStringLiteral("memory.chunkCacheBudgetBytes"), qint64{4294967296});
        s.setValue(QStringLiteral("scheme.active"), QStringLiteral("Esses Dark")); // unchanged: no signal
        QCOMPARE(changed.count(), 2);
        QVERIFY(s.dirty());
        QCOMPARE(s.value(QStringLiteral("scheme.active")).toString(), QStringLiteral("Esses Dark"));
        QTRY_VERIFY(std::filesystem::exists(file)); // the coalesced write
        QVERIFY(!s.dirty());
        const QJsonObject root = readJsonFile(file);
        QCOMPARE(root.value(QStringLiteral("$schema")).toString(), QStringLiteral("urn:esses:settings-1"));
        QCOMPARE(root.value(QStringLiteral("schemaVersion")).toInt(), 1);
        QCOMPARE(root.value(QStringLiteral("scheme")).toObject().value(QStringLiteral("active")).toString(),
                 QStringLiteral("Esses Dark"));
        QCOMPARE(root.value(QStringLiteral("memory"))
                     .toObject()
                     .value(QStringLiteral("chunkCacheBudgetBytes"))
                     .toDouble(),
                 4294967296.0);
        s.setValue(QStringLiteral("scheme.active"), QVariant()); // removal
        QVERIFY(!s.value(QStringLiteral("scheme.active")).isValid());
    }

    void settingsPreserveUnknownMembersAndFlushOnDestruction() {
        const auto file = scratchDir("settings-b") / "settings.json";
        writeFile(file, R"({"$schema": "urn:esses:settings-1", "schemaVersion": 1,
                           "plugins": {"disabled": ["x"]}, "window": {"geometry": "abc"}})");
        {
            SettingsStore s(file);
            QVERIFY(s.load().ok());
            QCOMPARE(s.value(QStringLiteral("window.geometry")).toString(), QStringLiteral("abc"));
            s.setValue(QStringLiteral("language"), QStringLiteral("en"));
            // destroyed before the coalescing timer fires: the destructor writes
        }
        const QJsonObject root = readJsonFile(file);
        QCOMPARE(root.value(QStringLiteral("language")).toString(), QStringLiteral("en"));
        QVERIFY(root.value(QStringLiteral("plugins")).isObject());
        QCOMPARE(root.value(QStringLiteral("window")).toObject().value(QStringLiteral("geometry")).toString(),
                 QStringLiteral("abc"));
    }

    void settingsMruCaps() {
        SettingsStore s(scratchDir("settings-c") / "settings.json");
        for (int i = 0; i < 25; ++i) {
            s.addRecentLog(QStringLiteral("D:/logs/%1.ld").arg(i));
        }
        QCOMPARE(s.recentLogs().size(), SettingsStore::kRecentLogs);
        QCOMPARE(s.recentLogs().front(), QStringLiteral("D:/logs/24.ld"));
        s.addRecentLog(QStringLiteral("D:/logs/10.ld"));
        QCOMPARE(s.recentLogs().front(), QStringLiteral("D:/logs/10.ld"));
        QCOMPARE(s.recentLogs().count(QStringLiteral("D:/logs/10.ld")), 1);
        for (int i = 0; i < 12; ++i) {
            s.addRecentProject(QStringLiteral("C:/p/%1").arg(i));
        }
        QCOMPARE(s.recentProjects().size(), SettingsStore::kRecentProjects);
        QVERIFY(s.saveNow().ok());
    }

    void settingsLoadErrors() {
        const auto dir = scratchDir("settings-d");
        writeFile(dir / "bad.json", "{ not json");
        SettingsStore bad(dir / "bad.json");
        QCOMPARE(bad.load().error().code, ErrorCode::Format);
        writeFile(dir / "other.json", R"({"$schema": "urn:esses:keymap-1"})");
        SettingsStore other(dir / "other.json");
        QCOMPARE(other.load().error().code, ErrorCode::Invalid);
        writeFile(dir / "array.json", "[1, 2]");
        SettingsStore array(dir / "array.json");
        QCOMPARE(array.load().error().code, ErrorCode::Format);
    }

    void messagesCapAndFirstError() {
        MessagesModel m;
        QSignalSpy firstError(&m, &MessagesModel::firstError);
        for (std::size_t i = 0; i < MessagesModel::kSessionCap + 10; ++i) {
            Message msg;
            msg.text = QString::number(i);
            msg.severity = i % 1000 == 7 ? Message::Severity::Error : Message::Severity::Info;
            m.append(std::move(msg));
        }
        QCOMPARE(static_cast<std::size_t>(m.rowCount()), MessagesModel::kSessionCap);
        QCOMPARE(m.at(0).text, QStringLiteral("10")); // the oldest rows dropped
        QCOMPARE(firstError.count(), 1);              // only the first Error of a session
        QVERIFY(m.at(0).timestamp.isValid());
        QCOMPARE(m.data(m.index(0), MessagesModel::SeverityRole).toInt(),
                 static_cast<int>(Message::Severity::Info));
        QVERIFY(m.roleNames().contains(MessagesModel::CodeRole));
        m.clear();
        QCOMPARE(m.rowCount(), 0);
    }

    void messagesImportLog() {
        MessagesModel m;
        formats::ImportLogEntry info;
        info.code = "ld.dialect";
        info.message = "dialect";
        formats::ImportLogEntry warning;
        warning.level = formats::ImportLogEntry::Level::Warning;
        warning.code = "ld.count_mismatch";
        warning.message = "count mismatch";
        formats::ImportLogEntry error;
        error.level = formats::ImportLogEntry::Level::Error;
        error.code = "ld.truncated";
        error.message = "truncated";
        m.appendImportLog(QStringLiteral("a.ld"), {info, warning, error});
        QCOMPARE(m.rowCount(), 3);
        QCOMPARE(m.count(Message::Severity::Info), 1);
        QCOMPARE(m.count(Message::Severity::Warning), 1);
        QCOMPARE(m.count(Message::Severity::Error), 1);
        QCOMPARE(m.at(1).code, QStringLiteral("ld.count_mismatch"));
        QCOMPARE(m.at(1).origin, Message::Origin::Import);
        QCOMPARE(m.data(m.index(2), MessagesModel::SourceRole).toString(), QStringLiteral("a.ld"));
        QCOMPARE(m.data(m.index(2)).toString(), QStringLiteral("truncated"));
    }

    // The developer log reaches the dock through the Dispatcher, from any thread; shutdown removes the sink.
    void logSinkPostsThroughTheDispatcher() {
        MessagesModel m;
        {
            AppRuntimeOptions o;
            o.workers = 2;
            o.cacheBudget = std::size_t{16} << 20;
            AppRuntime rt(o);
            rt.addSink(makeMessagesLogSink(m, rt.dispatcher()));
            std::thread worker([] { Log::write(LogLevel::Warning, "test.sink", "from a worker"); });
            worker.join();
            Log::write(LogLevel::Info, "test.sink", "below the minimum");
            Log::write(LogLevel::Error, "test.sink", "an error");
            QTRY_COMPARE(m.rowCount(), 2);
            QCOMPARE(m.at(0).text, QStringLiteral("from a worker"));
            QCOMPARE(m.at(0).source, QStringLiteral("test.sink"));
            QCOMPARE(m.at(0).severity, Message::Severity::Warning);
            QCOMPARE(m.at(1).severity, Message::Severity::Error);
            QCOMPARE(m.at(1).origin, Message::Origin::System);
            rt.shutdown();
        }
        Log::write(LogLevel::Error, "test.sink", "after shutdown");
        QCoreApplication::processEvents();
        QCOMPARE(m.rowCount(), 2);
    }
};

QTEST_GUILESS_MAIN(TstSettingsMessages)
#include "tst_settings_messages.moc"
