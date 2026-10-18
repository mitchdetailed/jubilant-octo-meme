// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The Data-window filter model and the Channels-window search (06 sections 5.4 and 7; D-017, D-162; moved
// from M1 by D-044): the lap-number syntax with its editor messages (a parse error keeps the previous
// filter), the filters persisted in project.json dataWindow (view state, never on the undo stack) by source
// hash with a default and the apply-to-all flag, and the word-prefix search (prefixes in order, words split
// on whitespace and the separators of 06 section 7 item 3; loose = substrings) with the Channels tree's Alias
// and Project/Constants categories.

#include "qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/channels_model.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/session_restore.hpp>
#include <esses/qtmodel/undo_commands.hpp>

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

using namespace esses;
using namespace esses::qt;
using namespace esses::qt::test;

namespace {

Paths pathsFor(const std::filesystem::path& config) {
    PathOverrides o;
    o.configDir = config;
    o.dataDir = repoRoot() / "data";
    o.cacheDir = config / "cache";
    o.logDir = config / "logs";
    auto p = Paths::resolve(o);
    if (!p) {
        qFatal("Paths::resolve failed: %s", p.error().toString().c_str());
    }
    return std::move(p).value();
}

QJsonObject dataWindowOf(const ProjectSession& s) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(s.projectFile().dataWindow.compact())).object();
}

QStringList childTexts(const QAbstractItemModel& m, const QModelIndex& parent) {
    QStringList out;
    for (int r = 0; r < m.rowCount(parent); ++r) {
        out << m.data(m.index(r, 0, parent)).toString();
    }
    return out;
}

QModelIndex childNamed(const QAbstractItemModel& m, const QModelIndex& parent, const QString& text) {
    for (int r = 0; r < m.rowCount(parent); ++r) {
        const QModelIndex i = m.index(r, 0, parent);
        if (m.data(i).toString() == text) {
            return i;
        }
    }
    return {};
}

} // namespace

class TstFiltersSearch : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void lapNumberSyntax() {
        for (const char* ok : {"3", "3-7", " 3 , 5 - 7 ", "7-3", "1,2,3", "10-12,1"}) {
            QVERIFY2(DataSelectionModel::lapNumbersError(ok).empty(), ok);
            QVERIFY2(DataSelectionModel::parseLapNumbers(ok).has_value(), ok);
        }
        QCOMPARE(DataSelectionModel::lapNumbersError(""), std::string("expected a lap number at 1"));
        QCOMPARE(DataSelectionModel::lapNumbersError("3-"), std::string("expected a lap number at 3"));
        QCOMPARE(DataSelectionModel::lapNumbersError("3,,4"), std::string("expected a lap number at 3"));
        QCOMPARE(DataSelectionModel::lapNumbersError("3 4"), std::string("expected ',' or '-' at 3"));
        QCOMPARE(DataSelectionModel::lapNumbersError("a"), std::string("expected a lap number at 1"));
        QCOMPARE(DataSelectionModel::lapNumbersError("2,9999999"), std::string("lap number too large at 3"));
        // Descending ranges normalised; terms kept in order.
        const auto terms = DataSelectionModel::parseLapNumbers("7-3, 9");
        QVERIFY(terms.has_value());
        QCOMPARE(terms->size(), std::size_t{2});
        QCOMPARE(terms->at(0), (std::pair<int, int>{3, 7}));
        QCOMPARE(terms->at(1), (std::pair<int, int>{9, 9}));
        for (const FilterSpec::Kind k :
             {FilterSpec::Kind::All, FilterSpec::Kind::Selected, FilterSpec::Kind::Fastest,
              FilterSpec::Kind::Fastest3, FilterSpec::Kind::WithinOnePercent, FilterSpec::Kind::LapNumbers}) {
            QVERIFY(parseFilterKind(filterKindName(k)) == std::optional<FilterSpec::Kind>(k));
        }
        QVERIFY(!parseFilterKind("lapnumbers").has_value());
    }

    // 06 section 5.4: filters per source, persisted in project.json dataWindow; never on the undo stack.
    void filtersArePersistedViewState() {
        AppRuntime rt;
        const auto dir = scratchDir("filters");
        const auto folder = dir / "Project";
        ProjectSession session(pathsFor(dir / "config"));
        QVERIFY(session.newProject(folder, "Filters").ok());
        DataSelectionModel selection(&session);
        SyntheticOptions o;
        o.path = "filters-a.ld";
        const auto a = makeSource(o, rt.cache());
        o.path = "filters-b.ld";
        const auto b = makeSource(o, rt.cache());
        selection.sourcePublished(a);
        selection.sourcePublished(b);
        QCOMPARE(selection.displayedRanges(a->id).size(), rangeCount(*a));
        QSignalSpy changed(&selection, &DataSelectionModel::filterChanged);
        QSignalSpy rejected(&selection, &DataSelectionModel::filterRejected);
        FilterSpec laps;
        laps.kind = FilterSpec::Kind::LapNumbers;
        laps.laps = "2-3";
        selection.setFilter(a->id, laps);
        QCOMPARE(changed.count(), 1);
        // Laps 2 and 3, plus the Main range (filters never hide a slot).
        const auto shown = selection.displayedRanges(a->id);
        QVERIFY(shown.size() >= 2 && shown.size() <= 3);
        // A parse error keeps the previous filter and tells the editor.
        FilterSpec bad = laps;
        bad.laps = "2-";
        selection.setFilter(a->id, bad);
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(rejected.first().at(1).toString(), QStringLiteral("expected a lap number at 3"));
        QVERIFY(selection.filter(a->id) == laps);
        QCOMPARE(session.undoStack().count(), 0);
        // project.json dataWindow: the default and the source's hash key (D-162).
        const QJsonObject filters = dataWindowOf(session).value(QStringLiteral("filters")).toObject();
        const QString key = QStringLiteral("xxh3:") + QString::fromStdString(a->id.hex());
        QCOMPARE(filters.value(key).toObject().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("lapNumbers"));
        QCOMPARE(filters.value(key).toObject().value(QStringLiteral("laps")).toString(),
                 QStringLiteral("2-3"));
        QCOMPARE(filters.value(QStringLiteral("default")).toObject().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("all"));
        QVERIFY(!dataWindowOf(session).value(QStringLiteral("applyToAllSources")).toBool());
        // Apply to all sources: the flag and every source's filter.
        selection.setMain(rangeRef(*a, 2));
        selection.applyFilterToAllSources();
        QVERIFY(selection.filter(b->id) == laps);
        QVERIFY(dataWindowOf(session).value(QStringLiteral("applyToAllSources")).toBool());
        QVERIFY(session.save().ok());
        // A closed source keeps its filter in the project; reopening the project restores it unconditionally.
        selection.closeSource(b->id);
        QVERIFY(dataWindowOf(session)
                    .value(QStringLiteral("filters"))
                    .toObject()
                    .contains(QStringLiteral("xxh3:") + QString::fromStdString(b->id.hex())));
        ProjectSession reopened(pathsFor(dir / "config"));
        QVERIFY(reopened.openProject(folder).ok());
        DataSelectionModel restored(&reopened);
        QVERIFY(restored.applyToAllSources());
        restored.sourcePublished(b);
        QVERIFY(restored.filter(b->id) == laps);
        // A per-source change clears the flag (06 section 5.4).
        FilterSpec fastest;
        fastest.kind = FilterSpec::Kind::Fastest;
        restored.setFilter(b->id, fastest);
        QVERIFY(!restored.applyToAllSources());
        QVERIFY(!dataWindowOf(reopened).value(QStringLiteral("applyToAllSources")).toBool());
    }

    // 06 section 7 item 3: the word-prefix search semantics.
    void channelsSearchSemantics() {
        const auto accepts = [](const char* name, const char* query, bool loose = false) {
            return ChannelsModel::filterAccepts(QString::fromUtf8(name), QString::fromUtf8(query), loose);
        };
        QVERIFY(accepts("Wheel Speed FL", ""));
        QVERIFY(accepts("Wheel Speed FL", "W S FL"));
        QVERIFY(accepts("Wheel Speed FL", "wheel"));
        QVERIFY(accepts("Wheel Speed FL", "sp fl"));
        QVERIFY(accepts("Water Temp (ECU)", "temp (ecu)"));
        QVERIFY(accepts("Water Temp (ECU)", "ecu"));
        QVERIFY(accepts("Engine_RPM", "rpm"));
        QVERIFY(accepts("Brake-Pres/Front", "pres front"));
        QVERIFY(accepts("Throttle Pos[0]", "0"));
        QVERIFY(!accepts("Wheel Speed FL", "speed wheel"));  // in order
        QVERIFY(!accepts("Wheel Speed FL", "w w"));          // distinct name words
        QVERIFY(!accepts("Wheel Speed FL", "peed"));         // a prefix, not a substring
        QVERIFY(accepts("Wheel Speed FL", "peed", true));    // loose: substrings
        QVERIFY(accepts("Wheel Speed FL", "eel eed", true)); // still in order
        QVERIFY(!accepts("Wheel Speed FL", "eed eel", true));
        QVERIFY(!accepts("Engine RPM", "engine rpm max"));
        QCOMPARE(ChannelsModel::searchWords(QStringLiteral(" Water  Temp (ECU)[1]_a-b/c ")),
                 (QStringList{QStringLiteral("Water"), QStringLiteral("Temp"), QStringLiteral("ECU"),
                              QStringLiteral("1"), QStringLiteral("a"), QStringLiteral("b"),
                              QStringLiteral("c")}));
    }

    // 06 section 7 item 1: the Alias category per source and the Project node's constants.
    void channelsTreeCategories() {
        AppRuntime rt;
        const auto dir = scratchDir("channels-tree");
        ProjectSession session(pathsFor(dir / "config"));
        QVERIFY(session.loadDefaults().ok());
        session.setUnits(shippedUnits());
        MathsService maths(rt.pool(), rt.dispatcher(), shippedUnits());
        connectProjectSession(session, &maths, nullptr);
        DataSelectionModel selection(&session);
        ChannelsModel model(selection, maths);
        SyntheticOptions o;
        o.path = "tree.ld";
        const auto src = makeSource(o, rt.cache());
        maths.addSource(src);
        selection.sourcePublished(src);
        maths::MathModule user;
        user.id = "user";
        maths::MathConstant k;
        k.name = "Track Length";
        k.value.v = 2000.0;
        k.value.unit = *shippedUnits()->find("m");
        user.constants.push_back(k);
        session.undoStack().push(new EditMathModulesCommand(session, {user}, QStringLiteral("Modules")));
        project::Aliases aliases;
        aliases.table.rows.push_back(
            model::AliasRow{"Speed Any", {{"Wheel Speed FL", true}}, -1, std::nullopt});
        aliases.table.rows.push_back(model::AliasRow{"Dist", {{"Corr Dist", true}}, -1, std::nullopt});
        aliases.table.rows.push_back(model::AliasRow{"Nothing", {{"Nope", true}}, -1, std::nullopt});
        session.undoStack().push(new EditAliasesCommand(session, aliases, QStringLiteral("Aliases")));
        model.rebuild();
        QCOMPARE(model.rowCount(), 2); // the source and "Project"
        const QModelIndex source = model.index(0, 0);
        QCOMPARE(childTexts(model, source),
                 (QStringList{QStringLiteral("Logged"), QStringLiteral("Maths"), QStringLiteral("Alias")}));
        const QModelIndex alias = childNamed(model, source, QStringLiteral("Alias"));
        QCOMPARE(model.rowCount(alias), 2); // "Nothing" resolves in no way
        QCOMPARE(model.data(model.index(0, 0, alias), ChannelsModel::ChannelNameRole).toString(),
                 QStringLiteral("Dist"));
        QVERIFY(model.data(model.index(1, 0, alias)).toString().startsWith(QStringLiteral("Speed Any (")));
        const QModelIndex project = model.index(1, 0);
        QCOMPARE(model.data(project).toString(), QStringLiteral("Project"));
        const QModelIndex constants = childNamed(model, project, QStringLiteral("Constants"));
        QVERIFY(constants.isValid());
        QCOMPARE(model.data(model.index(0, 0, constants)).toString(), QStringLiteral("Track Length (m)"));
        // A disabled maths output is not a channel.
        user.enabled = false;
        maths::MathItem i;
        i.name = "Off";
        i.expr = "1";
        user.items.push_back(i);
        session.undoStack().push(new EditMathModulesCommand(session, {user}, QStringLiteral("Modules")));
        model.rebuild();
        const QModelIndex mathsNode = childNamed(model, model.index(0, 0), QStringLiteral("Maths"));
        QVERIFY(!childTexts(model, mathsNode).contains(QStringLiteral("Off")));
        QCOMPARE(model.rowCount(), 1); // the disabled module's constants are gone too
    }
};

QTEST_GUILESS_MAIN(TstFiltersSearch)
#include "tst_filters_search.moc"
