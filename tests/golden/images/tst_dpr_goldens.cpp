// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Fractional-DPR geometry of every drawn MVP component type (M4 hidpi package; 07 section 7.1 row 3,
// AC-M4-07; 04 section 6.6 items 4-5; D-025, D-209): the builders' Static, Dynamic and Chrome DrawLists at
// DPR 1.25 and 1.5 (FixedTextMetrics, so platform independent) of time_distance (two M1 cases of
// libs/plotcore/tests/td_golden_cases.hpp), scatter, histogram, outing, gauge_numeric and gauge_bar (the
// first case of each in plots_golden_cases.hpp) and track_position (a rectangle circuit driven at constant
// speed, built here from integer points: no transcendental function feeds a coordinate). For each case and
// DPR:
//   * exact DrawList JSON goldens, checked on every OS:
//   tests/golden/images/dpr_sweep/<type>/<case>@<dpr>x.json
//     (the goldens at DPR 1 and 2 stay those of tests/golden/drawlist/, authoritative as before; D-025);
//     ESSES_UPDATE_GOLDENS=1 rewrites them, a mismatch writes <build>/tests/golden/artefacts/dpr_sweep/;
//   * the layers are lround(logical x DPR) device pixels and carry the DPR (04 section 6.6 items 1-2);
//   * crisp: every rectangle on whole device pixels and every axis-aligned line wider than 1 px of a whole
//     width on pixel edges (04 section 6.6 item 4, libs/widgets/tests/m4_hidpi/dpr_test_support.hpp);
//   * no layout drift: the same commands in the same order as at DPR 1, and every scissor and filled
//     rectangle within two logical pixels of its DPR 1 place (each side rounds once to its own pixel grid);
//   * the Static list rasterises into an image of that device size tagged with the DPR (LayerCache, the path
//     of the widgets, OffscreenRenderer and esses-render).
// The widgets' own sweep at DPR 1, 1.25, 1.5 and 2 (all ten types, real windows) is
// libs/widgets/tests/m4_hidpi/tst_dpr_sweep.cpp.

#include "../../../libs/plotcore/tests/plots_golden_cases.hpp"
#include "../../../libs/qtmodel/tests/reports/report_track_fixtures.hpp"
#include "../../../libs/widgets/tests/m4_hidpi/dpr_test_support.hpp"

#include <esses/base/cancel.hpp>
#include <esses/base/env.hpp>
#include <esses/base/read_file.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/marker.hpp>
#include <esses/qtrender/layer_cache.hpp>
#include <esses/qtrender/offscreen_renderer.hpp>
#include <esses/qtrender/scheme_fonts.hpp>

#include <QTest>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace esses;
using namespace esses::plot;

namespace {

constexpr double kDriftLogical = 2.0;
constexpr std::array<float, 2> kFractional{1.25F, 1.5F};

std::filesystem::path goldenDir(const std::string& type) {
    return std::filesystem::path(ESSES_TEST_REPO_ROOT) / "tests" / "golden" / "images" / "dpr_sweep" / type;
}

std::filesystem::path artefactsDir() {
    return std::filesystem::path(ESSES_TEST_BINARY_DIR) / "artefacts" / "dpr_sweep";
}

std::string dprTag(float dpr) {
    return dpr == 1.25F ? "1.25" : (dpr == 1.5F ? "1.5" : std::to_string(static_cast<int>(dpr)));
}

std::string readText(const std::filesystem::path& p) {
    Result<std::string> text = readWholeFile(p);
    if (!text) {
        return {};
    }
    std::string out;
    for (const char c : *text) {
        if (c != '\r') {
            out += c;
        }
    }
    return out;
}

void writeText(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// One case: its type, name, logical size and inputs at a DPR.
struct SweepCase {
    std::string type;
    std::string name;
    int width = 0;
    int height = 0;
    std::function<ComponentBuildInputs(float dpr)> inputs;
};

struct Layers {
    DrawList staticList;
    DrawList dynamicList;
    DrawList chromeList;
    bool ok = false;
    std::string error;
};

Layers build(const SweepCase& c, float dpr) {
    Layers out;
    const qt::ComponentBuilders b = qt::OffscreenRenderer::builtinBuilders(c.type);
    const ComponentBuildInputs in = c.inputs(dpr);
    Result<ComponentBuild> s = b.buildStatic(in, CancelToken{});
    if (!s) {
        out.error = s.error().message;
        return out;
    }
    out.staticList = std::move(s).value().list;
    out.dynamicList = b.buildDynamic(in);
    out.chromeList = b.buildChrome(in, false);
    out.ok = true;
    return out;
}

std::string goldenText(const SweepCase& c, float dpr, const Layers& l) {
    std::string s = "{\"case\":\"" + c.name + "\",\"type\":\"" + c.type + "\",\"dpr\":" + dprTag(dpr) + ",\n";
    s += "\"static\":" + l.staticList.toJson();
    s += ",\"dynamic\":" + l.dynamicList.toJson();
    s += ",\"chrome\":" + l.chromeList.toJson();
    s += "}\n";
    return s;
}

QString kindsOf(const DrawList& list) {
    QString out;
    for (const DrawCmd& cmd : list.cmds) {
        out += QString::number(static_cast<int>(cmd.kind)) + QLatin1Char(',');
    }
    return out;
}

double rectDrift(const RectPx& a, float dprA, const RectPx& b, float dprB) {
    const auto l = [](float v, float d) { return static_cast<double>(v) / static_cast<double>(d); };
    return std::max({std::abs(l(a.x, dprA) - l(b.x, dprB)), std::abs(l(a.y, dprA) - l(b.y, dprB)),
                     std::abs(l(a.w, dprA) - l(b.w, dprB)), std::abs(l(a.h, dprA) - l(b.h, dprB))});
}

// ---- track_position: a 700 m x 300 m rectangle (report_track_fixtures.hpp rectMap, integer points), three
// laps of 20 s at constant speed, a beacon between the laps.

constexpr Micros kSec = test::kSec;

struct TrackRun {
    std::vector<LapPiece> pieces;
    std::shared_ptr<model::DataSource> source;
};

TrackRun constantLaps(double lengthM) {
    TrackRun r;
    for (int lap = 0; lap < 3; ++lap) {
        LapPiece p;
        p.start = static_cast<Micros>(lap) * 20 * kSec;
        p.end = static_cast<Micros>(lap + 1) * 20 * kSec;
        p.lookup = test::FakeLapDistance::constant(p.start, p.end, lengthM / 20.0);
        r.pieces.push_back(std::move(p));
    }
    r.source = std::make_shared<model::DataSource>();
    model::DataLayer layer;
    for (int lap = 1; lap < 3; ++lap) {
        model::Marker m;
        m.cls = model::MarkerClass::BCN;
        m.group = "Beacons";
        m.t = static_cast<Micros>(lap) * 20 * kSec;
        layer.markers.push_back(m);
    }
    r.source->layers.push_back(std::move(layer));
    return r;
}

SlotData trackSlot(const TrackRun& r, std::size_t lap, SlotKind kind, int number) {
    SlotData s;
    s.slot.kind = kind;
    s.slot.number = number;
    s.slot.range.name = "Lap " + std::to_string(lap + 1);
    s.source = r.source;
    s.range = RangeGeom{r.pieces[lap].start, r.pieces[lap].end};
    s.outing = OutingGeom{r.pieces.front().start, r.pieces.back().end};
    s.chain = std::make_shared<const DistanceChain>(r.pieces, r.pieces[lap].start);
    return s;
}

} // namespace

class TstDprGoldens : public QObject {
    Q_OBJECT
public:
    static void initMain() {
        if (!qEnvironmentVariableIsSet("QT_QPA_FONTDIR")) {
            qputenv("QT_QPA_FONTDIR",
                    (std::filesystem::path(ESSES_TEST_REPO_ROOT) / "tests" / "golden" / "fonts")
                        .generic_string()
                        .c_str());
        }
    }

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(qt::SchemeFonts::registerApplicationFonts(std::filesystem::path(ESSES_TEST_REPO_ROOT) /
                                                          "tests" / "golden" / "fonts") > 0);
        QVERIFY(test::makeResident(fx_.a));
        QVERIFY(test::makeResident(fx_.b));
        QVERIFY(test::makeResident(fx_.c));
        const std::vector<test::GoldenCase> td = test::goldenCases(fx_);
        for (const char* name : {"single_trace", "two_panels"}) {
            for (const test::GoldenCase& gc : td) {
                if (gc.name == name) {
                    cases_.push_back(
                        SweepCase{"time_distance", gc.name, gc.widthLogical, gc.heightLogical,
                                  [this, gc](float dpr) { return test::inputsOf(fx_, gc, dpr); }});
                }
            }
        }
        std::vector<std::string> seen;
        for (const test::PlotGoldenCase& pc : test::plotGoldenCases(fx_)) {
            if (std::find(seen.begin(), seen.end(), pc.type) != seen.end()) {
                continue; // the first case of each type
            }
            seen.push_back(pc.type);
            cases_.push_back(SweepCase{pc.type, pc.name, pc.width, pc.height, [this, pc](float dpr) {
                                           return test::plotGoldenInputs(fx_, pc, dpr);
                                       }});
        }
        track_ = constantLaps(2000.0);
        other_ = constantLaps(2000.0);
        map_ = std::make_shared<const analysis::TrackMap>(qt::rttest::rectMap("Sweep Venue"));
        cases_.push_back(SweepCase{"track_position", "track_rectangle", 320, 240, [this](float dpr) {
                                       ComponentBuildInputs in;
                                       in.spec.type = "track_position";
                                       in.spec.caption.show = true;
                                       in.spec.caption.autoText = true;
                                       in.spec.body = JsonDoc::parse(R"({"showSectionLabels":true})").value();
                                       in.view.domain = Domain::Time;
                                       in.view.zoom = {2.0e6, 9.0e6};
                                       in.view.cursorT = 6 * kSec;
                                       in.view.version = 1;
                                       in.slots = {trackSlot(track_, 1, SlotKind::Main, 0),
                                                   trackSlot(other_, 2, SlotKind::Overlay, 1)};
                                       in.selection.main = in.slots[0].slot.range;
                                       in.selection.overlays = {in.slots[1].slot.range};
                                       in.scheme =
                                           std::make_shared<const project::Scheme>(test::lightScheme());
                                       in.metrics = std::make_shared<const FixedTextMetrics>();
                                       in.layout.widthPx = static_cast<int>(std::lround(320.0F * dpr));
                                       in.layout.heightPx = static_cast<int>(std::lround(240.0F * dpr));
                                       in.layout.dpr = dpr;
                                       in.trackMap = map_;
                                       return in;
                                   }});
        // Every drawn MVP type is here.
        QStringList types;
        for (const SweepCase& c : cases_) {
            if (!types.contains(QString::fromStdString(c.type))) {
                types << QString::fromStdString(c.type);
            }
        }
        types.sort();
        QCOMPARE(types, (QStringList{QStringLiteral("gauge_bar"), QStringLiteral("gauge_numeric"),
                                     QStringLiteral("histogram"), QStringLiteral("outing"),
                                     QStringLiteral("scatter"), QStringLiteral("time_distance"),
                                     QStringLiteral("track_position")}));
    }

    void fractionalDpr_data() {
        QTest::addColumn<int>("index");
        QTest::addColumn<float>("dpr");
        for (std::size_t i = 0; i < cases_.size(); ++i) {
            for (const float dpr : kFractional) {
                QTest::newRow((cases_[i].type + "/" + cases_[i].name + "@" + dprTag(dpr) + "x").c_str())
                    << static_cast<int>(i) << dpr;
            }
        }
    }

    void fractionalDpr() {
        QFETCH(int, index);
        QFETCH(float, dpr);
        const SweepCase& c = cases_.at(static_cast<std::size_t>(index));
        const Layers l = build(c, dpr);
        QVERIFY2(l.ok, l.error.c_str());
        const Layers one = build(c, 1.0F);
        QVERIFY2(one.ok, one.error.c_str());

        // Device pixels at this DPR.
        const int w = static_cast<int>(std::lround(static_cast<float>(c.width) * dpr));
        const int h = static_cast<int>(std::lround(static_cast<float>(c.height) * dpr));
        for (const DrawList* list : {&l.staticList, &l.dynamicList, &l.chromeList}) {
            QCOMPARE(list->dpr, dpr);
            QCOMPARE(list->widthPx, w);
            QCOMPARE(list->heightPx, h);
        }

        // Crisp: the axis-aligned geometry covers whole device pixels as the backend draws each layer.
        QStringList problems =
            widgets::dprtest::crispnessProblems(l.staticList, QStringLiteral("static"), true);
        problems << widgets::dprtest::crispnessProblems(l.dynamicList, QStringLiteral("dynamic"), false);
        problems << widgets::dprtest::crispnessProblems(l.chromeList, QStringLiteral("chrome"), false);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("; "))));

        // No drift from DPR 1: the same commands, the rectangles in the same logical places.
        const std::array<std::pair<const DrawList*, const DrawList*>, 3> pairs{
            {{&l.staticList, &one.staticList},
             {&l.dynamicList, &one.dynamicList},
             {&l.chromeList, &one.chromeList}}};
        for (const auto& [at, base] : pairs) {
            QCOMPARE(kindsOf(*at), kindsOf(*base));
            for (std::size_t k = 0; k < at->cmds.size(); ++k) {
                const DrawCmd& a = at->cmds[k];
                if (a.kind != CmdKind::Scissor && a.kind != CmdKind::FilledRect) {
                    continue;
                }
                const double d = rectDrift(a.rect, dpr, base->cmds[k].rect, 1.0F);
                QVERIFY2(d <= kDriftLogical,
                         qPrintable(QStringLiteral("cmd %1 moved %2 logical px").arg(k).arg(d)));
            }
        }

        // The Static list rasterises at this device size and DPR.
        qt::LayerCache cache;
        cache.resize(QSize(c.width, c.height), static_cast<qreal>(dpr));
        cache.rasteriseStatic(l.staticList);
        QCOMPARE(cache.staticImage().size(), QSize(w, h));
        QCOMPARE(cache.staticImage().devicePixelRatio(), static_cast<qreal>(dpr));

        // The exact golden.
        const std::string actual = goldenText(c, dpr, l);
        const std::string file = c.name + "@" + dprTag(dpr) + "x.json";
        const std::filesystem::path path = goldenDir(c.type) / file;
        if (getEnv(envName("UPDATE_GOLDENS")).value_or("") == "1") {
            writeText(path, actual);
            return;
        }
        const std::string expected = readText(path);
        if (expected != actual) {
            const std::filesystem::path out = artefactsDir() / (c.type + "_" + file);
            writeText(out, actual);
            QFAIL(qPrintable(QStringLiteral("DrawList golden %1/%2 differs%3; actual written to %4")
                                 .arg(QString::fromStdString(c.type), QString::fromStdString(file),
                                      expected.empty() ? QStringLiteral(" (golden missing)") : QString(),
                                      QString::fromStdWString(out.wstring()))));
        }
    }

private:
    test::Fixture fx_;
    std::vector<SweepCase> cases_;
    TrackRun track_;
    TrackRun other_;
    std::shared_ptr<const analysis::TrackMap> map_;
};

QTEST_MAIN(TstDprGoldens)
#include "tst_dpr_goldens.moc"
