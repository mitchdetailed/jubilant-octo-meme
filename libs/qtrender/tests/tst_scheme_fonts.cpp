// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SchemeFonts (04 section 6.1 item 4, 06 section 16): the TextStyleId -> font role table, QFont attributes
// (point size, weight, italic, NoSubpixelAntialias), the caption fallback, and the golden font registration
// (tests/golden/fonts: Noto Sans, OFL-1.1; D-127).

#include "qtrender_test_support.hpp"

#include <esses/qtrender/scheme_fonts.hpp>

#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>
#include <QTest>
#include <filesystem>

using namespace esses;

Q_DECLARE_METATYPE(esses::plot::TextStyleId)

class TestSchemeFonts : public QObject {
    Q_OBJECT
public:
    static void initMain() { qttest::prepareFontEnvironment(); }

private Q_SLOTS:
    void goldenFontsRegister() {
        const std::filesystem::path dir =
            std::filesystem::path(qttest::repoRoot().toStdWString()) / "tests" / "golden" / "fonts";
        const int n = qt::SchemeFonts::registerApplicationFonts(dir);
        QVERIFY2(n >= 2, "tests/golden/fonts must hold Noto Sans Regular and Bold");
        QCOMPARE(qt::SchemeFonts::registerApplicationFonts(dir), n); // idempotent
        QCOMPARE(qt::SchemeFonts::registerApplicationFonts(dir / "missing"), 0);
        QVERIFY(QFontDatabase::families().contains(QStringLiteral("Noto Sans")));
        const qt::SchemeFonts fonts(qttest::testScheme());
        QCOMPARE(QFontInfo(fonts.font(plot::TextStyleId::Axis)).family(), QStringLiteral("Noto Sans"));
        QVERIFY(QFontInfo(fonts.font(plot::TextStyleId::Caption)).bold()); // weight 600 resolves to Bold
    }

    void styleToRoleTable_data() {
        QTest::addColumn<plot::TextStyleId>("style");
        QTest::addColumn<bool>("rotated");
        QTest::addColumn<QString>("role");
        QTest::newRow("Axis") << plot::TextStyleId::Axis << false << "axis";
        QTest::newRow("Axis rotated") << plot::TextStyleId::Axis << true << "axisRotated";
        QTest::newRow("Caption") << plot::TextStyleId::Caption << false << "caption";
        QTest::newRow("Banner") << plot::TextStyleId::Banner << false << "caption";
        QTest::newRow("Key") << plot::TextStyleId::Key << false << "key";
        QTest::newRow("KeyValue") << plot::TextStyleId::KeyValue << false << "key";
        QTest::newRow("Band") << plot::TextStyleId::Band << false << "small";
        QTest::newRow("Measurement") << plot::TextStyleId::Measurement << false << "small";
        QTest::newRow("GaugeUnits") << plot::TextStyleId::GaugeUnits << false << "small";
        QTest::newRow("Gauge") << plot::TextStyleId::Gauge << false << "gauge";
        QTest::newRow("Table") << plot::TextStyleId::Table << false << "reportTable";
        QTest::newRow("Equation") << plot::TextStyleId::Equation << false << "equation";
        QTest::newRow("ColourBand") << plot::TextStyleId::ColourBand << false << "colourBands";
        QTest::newRow("TrackLabel") << plot::TextStyleId::TrackLabel << false << "trackLabel";
        QTest::newRow("Key rotated") << plot::TextStyleId::Key << true << "key";
    }
    void styleToRoleTable() {
        QFETCH(plot::TextStyleId, style);
        QFETCH(bool, rotated);
        QFETCH(QString, role);
        QCOMPARE(QString::fromUtf8(qt::SchemeFonts::roleName(style, rotated).data(),
                                   static_cast<qsizetype>(qt::SchemeFonts::roleName(style, rotated).size())),
                 role);
        const project::Scheme scheme = qttest::testScheme();
        const qt::SchemeFonts fonts(scheme);
        const project::FontSpec spec = scheme.font(role.toStdString());
        const QFont f = fonts.font(style, rotated);
        QCOMPARE(f.pointSizeF(), spec.pointSize);
        QCOMPARE(static_cast<int>(f.weight()), spec.weight);
        QCOMPARE(f.italic(), spec.italic);
        QCOMPARE(f.family(), QString::fromStdString(spec.family));
        QVERIFY((f.styleStrategy() & QFont::NoSubpixelAntialias) != 0);
    }

    void roleByNameAndCaptionFallback() {
        const qt::SchemeFonts fonts(qttest::testScheme());
        QCOMPARE(fonts.role(QStringLiteral("valuesItem")).pointSizeF(), 9.0);
        QCOMPARE(fonts.role(QStringLiteral("reportTable")).pointSizeF(), 9.5);
        const QFont missing = fonts.role(QStringLiteral("noSuchRole"));
        QCOMPARE(missing.pointSizeF(), 9.0); // the caption font
        QCOMPARE(static_cast<int>(missing.weight()), 600);
    }

    void defaultFontsWithoutAScheme() {
        const qt::SchemeFonts fonts;
        const QFont f = fonts.font(plot::TextStyleId::Gauge);
        QCOMPARE(f.family(), QStringLiteral("Noto Sans")); // project::FontSpec{}
        QCOMPARE(f.pointSizeF(), 9.0);
    }

    void idsAndGenerations() {
        const qt::SchemeFonts a(qttest::testScheme(), 4);
        const qt::SchemeFonts b(qttest::testScheme(), 4);
        const qt::SchemeFonts copy = a; // NOLINT(performance-unnecessary-copy-initialization)
        QCOMPARE(a.generation(), std::uint64_t{4});
        QVERIFY(a.id() != b.id());
        QCOMPARE(copy.id(), a.id());
        QCOMPARE(a.scheme().name, std::string("Qtrender Test"));
    }
};

QTEST_MAIN(TestSchemeFonts)
#include "tst_scheme_fonts.moc"
