// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelsWindow search (06 section 7 item 3, D-175), offscreen through the search box: every query word is a
// case-insensitive prefix of a distinct name word, in order, with names and queries split on white space,
// '_', '-', '/', brackets and parentheses ("W S FL", "temp (ecu)", "pres front" over "Oil_Pres_Front" and
// "Brake Pres [Front]"); a query out of order or inside a word matches nothing and says so under the box;
// loose matching (a substring per word, still in order) re-filters at once, and follows the project's
// options.general.looseChannelSearch (an options edit, undone too); the first match stays highlighted.

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/base/json_doc.hpp>
#include <esses/base/paths.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/channels_model.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/widgets/docks/channels_window.hpp>

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QTest>
#include <QTreeView>
#include <QUndoStack>
#include <memory>

// The qtmodel synthetic-source helpers (in memory, circuit laps), compiled into this executable only.
#include "../../../qtmodel/tests/qtmodel_test_support.cpp" // NOLINT(bugprone-suspicious-include)

using namespace esses;
using namespace esses::widgets;

namespace {

Paths pathsFor(const std::filesystem::path& config) {
    PathOverrides o;
    o.configDir = config;
    o.dataDir = qt::test::repoRoot() / "data";
    o.cacheDir = config / "cache";
    o.logDir = config / "logs";
    auto p = Paths::resolve(o);
    if (!p) {
        qFatal("Paths::resolve failed: %s", p.error().toString().c_str());
    }
    return std::move(p).value();
}

// The synthetic log with three channels renamed to the spellings of 06 section 7 item 3.
std::shared_ptr<const model::DataSource> namedSource(store::ChunkCache& cache) {
    qt::test::SyntheticOptions o;
    o.path = "search.ld";
    o.gps = true;
    auto src = std::make_shared<model::DataSource>(*qt::test::makeSource(o, cache));
    model::DataLayer& layer = src->layers.front();
    for (model::Channel& c : layer.channels) {
        if (c.desc.name == "Throttle") {
            c.desc.name = c.desc.loggedName = "Water Temp (ECU)";
        } else if (c.desc.name == "GPS Latitude") {
            c.desc.name = c.desc.loggedName = "Oil_Pres_Front";
        } else if (c.desc.name == "GPS Longitude") {
            c.desc.name = c.desc.loggedName = "Brake Pres [Front]";
        }
    }
    layer.rebuildIndex();
    return src;
}

void search(ChannelsWindow& win, const QString& text) {
    win.search().clear();
    QTest::keyClicks(&win.search(), text);
}

} // namespace

class TstChannelsSearch : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void wordPrefixSearchAndLooseMatching() {
        qt::AppRuntimeOptions o;
        o.workers = 2;
        o.cacheBudget = std::size_t{64} << 20;
        o.markGuiThreadNoBlock = true; // as esses (AC-M1-12)
        qt::AppRuntime rt(o);
        {
            const auto dir = qt::test::scratchDir("de-channels-search");
            qt::ProjectSession session(pathsFor(dir / "config"));
            QVERIFY(session.loadDefaults().ok());
            qt::MathsService maths(rt.pool(), rt.dispatcher(), qt::test::shippedUnits());
            qt::DataSelectionModel selection;
            qt::CommandRegistry commands;
            const auto src = namedSource(rt.cache());
            maths.addSource(src);
            selection.sourcePublished(src);
            qt::ChannelsModel model(selection, maths);
            model.rebuild();
            ChannelsWindow win(model, commands);
            win.resize(320, 480);
            win.show();
            QVERIFY(QTest::qWaitForWindowExposed(&win));
            QVERIFY(win.search().toolTip().contains(QStringLiteral("W S FL")));
            QVERIFY(!win.looseMatch());

            search(win, QStringLiteral("W S FL"));
            QCOMPARE(win.visibleChannels(), QStringList{QStringLiteral("Wheel Speed FL")});
            QCOMPARE(win.currentChannel(), QStringLiteral("Wheel Speed FL"));
            QVERIFY(win.noMatch().isHidden());
            search(win, QStringLiteral("temp (ecu)"));
            QCOMPARE(win.visibleChannels(), QStringList{QStringLiteral("Water Temp (ECU)")});
            search(win, QStringLiteral("PRES front"));
            QStringList pres = win.visibleChannels();
            pres.sort();
            QCOMPARE(pres,
                     (QStringList{QStringLiteral("Brake Pres [Front]"), QStringLiteral("Oil_Pres_Front")}));
            search(win, QStringLiteral("oil-pres"));
            QCOMPARE(win.visibleChannels(), QStringList{QStringLiteral("Oil_Pres_Front")});
            // Each query word takes a distinct name word, in order.
            search(win, QStringLiteral("speed wheel"));
            QVERIFY(win.visibleChannels().isEmpty());
            QVERIFY(!win.noMatch().isHidden());
            QVERIFY(win.noMatch().text().contains(QStringLiteral("speed wheel")));
            search(win, QStringLiteral("wheel wheel"));
            QVERIFY(win.visibleChannels().isEmpty());
            // A prefix, not a substring, unless loose matching is on (re-filtered at once).
            search(win, QStringLiteral("eed"));
            QVERIFY(win.visibleChannels().isEmpty());
            win.setLooseMatch(true);
            QVERIFY(win.visibleChannels().contains(QStringLiteral("Wheel Speed FL")));
            QVERIFY(win.visibleChannels().contains(QStringLiteral("Corr Speed")));
            QVERIFY(!win.visibleChannels().contains(QStringLiteral("Water Temp (ECU)")));
            QVERIFY(win.noMatch().isHidden());
            search(win, QStringLiteral("eed heel"));
            QVERIFY(win.visibleChannels().isEmpty()); // loose keeps the order
            win.setLooseMatch(false);

            // The project option drives it: an options edit, and its undo.
            win.followSession(session);
            QVERIFY(!win.looseMatch());
            project::ProjectFile edited = session.projectFile();
            QJsonObject options =
                QJsonDocument::fromJson(QByteArray::fromStdString(edited.options.compact())).object();
            QJsonObject general = options.value(QStringLiteral("general")).toObject();
            general.insert(QStringLiteral("looseChannelSearch"), true);
            options.insert(QStringLiteral("general"), general);
            auto doc = JsonDoc::parse(QJsonDocument(options).toJson(QJsonDocument::Compact).toStdString());
            QVERIFY(doc.ok());
            edited.options = *doc;
            session.undoStack().push(
                new qt::SetProjectOptionsCommand(session, edited, QStringLiteral("Options")));
            QVERIFY(win.looseMatch());
            search(win, QStringLiteral("eed"));
            QVERIFY(win.visibleChannels().contains(QStringLiteral("Wheel Speed FR")));
            session.undoStack().undo();
            QVERIFY(!win.looseMatch());
            QVERIFY(win.visibleChannels().isEmpty());
            QCOMPARE(win.search().text(), QStringLiteral("eed")); // the text is kept
            search(win, QString());
            QVERIFY(win.noMatch().isHidden());
            QVERIFY(win.visibleChannels().size() >= 6);
        }
        rt.shutdown();
    }
};

QTEST_MAIN(TstChannelsSearch)
#include "tst_channels_search.moc"
