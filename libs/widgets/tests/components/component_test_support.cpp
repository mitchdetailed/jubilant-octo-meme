// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "components/component_test_support.hpp"

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/hash.hpp>
#include <esses/base/utf8.hpp>
#include <esses/base/uuid.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/marker.hpp>
#include <esses/qtrender/scheme_fonts.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/store/series.hpp>

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace esses::widgets::cmptest {

namespace {

constexpr std::string_view kImporter = "motec.ld";

Micros sec(double s) {
    return static_cast<Micros>(std::llround(s * 1e6));
}

model::Channel channel(const model::SourceId& id, const std::string& name, units::UnitId unit, Rate rate,
                       const std::vector<float>& values, store::ChunkCache& cache) {
    const units::UnitRegistry& reg = *shippedUnits();
    model::Channel c;
    c.desc.name = name;
    c.desc.loggedName = name;
    c.desc.id = model::channelIdLogged(id, model::kNormalLayer, name, kImporter);
    c.desc.unit = unit;
    c.desc.quantity = reg.quantityOf(unit);
    c.desc.dtype = store::Dtype::F32;
    c.desc.rate = rate;
    c.desc.dps = 2;
    store::ColumnOptions co;
    co.cache = &cache;
    c.series = store::UniformSeries{
        rate, 0, store::Column(store::InMemoryProvider::fromF32(values, {}, "memory:" + name), co)};
    return c;
}

Uuid idOf(const std::string& id) {
    if (!id.empty()) {
        return Uuid::parse(id);
    }
    return uuidRandom();
}

model::OpaqueJson jsonOf(const QJsonObject& j) {
    Result<model::OpaqueJson> doc =
        model::OpaqueJson::parse(QJsonDocument(j).toJson(QJsonDocument::Compact).toStdString());
    if (!doc) {
        throw std::runtime_error("bad component body: " + doc.error().toString());
    }
    return std::move(doc).value();
}

} // namespace

std::filesystem::path repoRoot() {
    return pathFromUtf8(ESSES_TEST_REPO_ROOT);
}

void prepareFontEnvironment() {
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        const std::filesystem::path dir = repoRoot() / "tests" / "golden" / "fonts";
        qputenv("QT_QPA_FONTDIR", QString::fromStdWString(dir.wstring()).toLocal8Bit());
    }
}

int registerGoldenFonts() {
    static const int count =
        qt::SchemeFonts::registerApplicationFonts(repoRoot() / "tests" / "golden" / "fonts");
    return count;
}

std::shared_ptr<const units::UnitRegistry> shippedUnits() {
    static const std::shared_ptr<const units::UnitRegistry> registry = [] {
        Result<units::UnitRegistry> r = units::UnitRegistry::loadFiles(repoRoot() / "data" / "units.json");
        if (!r) {
            throw std::runtime_error("cannot load data/units.json: " + r.error().toString());
        }
        return std::make_shared<const units::UnitRegistry>(std::move(r).value());
    }();
    return registry;
}

std::shared_ptr<const project::Scheme> shippedScheme(const std::string& name) {
    std::vector<std::string> log;
    Result<project::Scheme> s =
        project::Scheme::load(repoRoot() / "data" / "schemes" / (name + ".scheme.json"), log);
    if (!s) {
        throw std::runtime_error("cannot load the " + name + " scheme: " + s.error().toString());
    }
    return std::make_shared<const project::Scheme>(std::move(s).value());
}

std::shared_ptr<const model::DataSource> makeSource(const SourceOptions& o, store::ChunkCache& cache) {
    const units::UnitRegistry& reg = *shippedUnits();
    auto src = std::make_shared<model::DataSource>();
    src->id = hash128(std::string_view(o.path));
    src->prov.path = pathFromUtf8(o.path);
    src->prov.importerId = std::string(kImporter);
    src->prov.hash = src->id;

    double total = 0.0;
    for (const double s : o.lapSeconds) {
        total += s;
    }
    const Rate rate{o.rateHz, 1};
    const auto n = static_cast<std::size_t>(std::llround(total * o.rateHz));
    std::vector<float> fl(n);
    std::vector<float> fr(n);
    std::vector<float> throttle(n);
    double lapStart = 0.0;
    std::size_t lap = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / o.rateHz;
        while (lap + 1 < o.lapSeconds.size() && t >= lapStart + o.lapSeconds[lap]) {
            lapStart += o.lapSeconds[lap];
            ++lap;
        }
        const double period = o.lapSeconds[lap];
        const double u = (t - lapStart) / period;
        // Integrates to exactly trackLengthM over a lap: the sine has a whole period per lap.
        const double v = o.trackLengthM / period * (1.0 + 0.3 * std::sin(2.0 * std::numbers::pi * u));
        fl[i] = static_cast<float>(v);
        fr[i] = static_cast<float>(v * 0.97);
        throttle[i] =
            static_cast<float>(50.0 + 50.0 * std::cos(2.0 * std::numbers::pi * u + o.throttlePhase));
    }

    model::DataLayer layer;
    layer.name = std::string(model::kNormalLayer);
    layer.duration = sec(total);
    if (o.wheelSpeeds) {
        const units::UnitId ms = *reg.find("m/s");
        layer.addChannel(channel(src->id, "Wheel Speed FL", ms, rate, fl, cache));
        layer.addChannel(channel(src->id, "Wheel Speed FR", ms, rate, fr, cache));
    }
    layer.addChannel(channel(src->id, "Throttle", *reg.find("%"), rate, throttle, cache));
    layer.markers.push_back(
        model::makeLoggedMarker(model::MarkerClass::SOL, 0, "", "", "", 0, 0, model::Provenance::Derived));
    double boundary = 0.0;
    for (std::size_t k = 0; k + 1 < o.lapSeconds.size(); ++k) {
        boundary += o.lapSeconds[k];
        layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::BCN, sec(boundary), "Beacons",
                                                        "Beacon", {}, 77, 0, model::Provenance::Companion));
    }
    layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::EOL, sec(total), "", "", "", 0, 0,
                                                    model::Provenance::Derived));
    model::sortMarkers(layer.markers);
    layer.baseMarkers = layer.markers;
    src->layers.push_back(std::move(layer));
    src->details.push_back(model::Detail{"File Name", std::string(pathToUtf8(src->prov.path.filename())),
                                         model::Provenance::Derived, std::nullopt});
    src->baseDetails = src->details;
    analysis::RuleRunner::circuitDefault().run(*src);
    return src;
}

const model::Range& lapAt(const model::DataSource& src, std::size_t index) {
    const model::RangeGroup* g = src.layers.front().ranges.find("Outings:Laps");
    if (g == nullptr || index >= g->ranges.size()) {
        throw std::runtime_error("no lap " + std::to_string(index));
    }
    return g->ranges[index];
}

model::RangeRef lapRef(const model::DataSource& src, std::size_t index) {
    const model::Range& r = lapAt(src, index);
    model::RangeRef ref;
    ref.src = src.id;
    ref.layer = std::string(model::kNormalLayer);
    ref.groupPath = "Outings:Laps";
    ref.range = r.id;
    ref.name = r.name;
    return ref;
}

project::ComponentSpec timeDistanceSpec(const std::vector<std::vector<std::string>>& groups,
                                        project::Rect rect, int z, const std::string& id) {
    QJsonObject body;
    body.insert(
        QStringLiteral("display"),
        QJsonObject{{QStringLiteral("traceStyle"), QStringLiteral("line")},
                    {QStringLiteral("lineWidth"), 1},
                    {QStringLiteral("showGrid"), true},
                    {QStringLiteral("key"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("labels")},
                                                        {QStringLiteral("pos"), QStringLiteral("left")},
                                                        {QStringLiteral("values"), true}}}});
    QJsonArray gj;
    for (const std::vector<std::string>& g : groups) {
        QJsonArray traces;
        for (const std::string& c : g) {
            traces.append(QJsonObject{{QStringLiteral("channel"), QString::fromStdString(c)}});
        }
        gj.append(QJsonObject{
            {QStringLiteral("name"), QString::fromStdString(g.empty() ? std::string() : g.front())},
            {QStringLiteral("scaleMode"), QStringLiteral("channel")},
            {QStringLiteral("traces"), traces}});
    }
    body.insert(QStringLiteral("groups"), gj);
    project::ComponentSpec spec;
    spec.id = idOf(id);
    spec.type = "time_distance";
    spec.rect = rect;
    spec.z = z;
    spec.body = jsonOf(body);
    return spec;
}

project::ComponentSpec valuesListSpec(const std::vector<std::string>& channels, project::Rect rect, int z,
                                      const std::string& id) {
    QJsonArray rows;
    for (const std::string& c : channels) {
        rows.append(QJsonObject{{QStringLiteral("channel"), QString::fromStdString(c)}});
    }
    const QJsonObject body{{QStringLiteral("channels"), rows},
                           {QStringLiteral("columns"), QJsonObject{{QStringLiteral("datum"), false},
                                                                   {QStringLiteral("delta"), true},
                                                                   {QStringLiteral("inlineUnits"), true}}},
                           {QStringLiteral("showHeading"), true},
                           {QStringLiteral("autoSizeColumns"), true}};
    project::ComponentSpec spec;
    spec.id = idOf(id);
    spec.type = "values_list";
    spec.rect = rect;
    spec.z = z;
    spec.body = jsonOf(body);
    return spec;
}

World::World(const std::string& domain, bool markGuiThreadNoBlock)
    : rt([markGuiThreadNoBlock] {
          qt::AppRuntimeOptions o;
          o.workers = 4;
          o.cacheBudget = std::size_t{256} << 20;
          o.markGuiThreadNoBlock = markGuiThreadNoBlock;
          return o;
      }()),
      maths(rt.pool(), rt.dispatcher(), shippedUnits()), distance(maths, rt.dispatcher()),
      render(rt.pool(), rt.dispatcher(), selection, maths, distance), scheme(shippedScheme("light")) {
    sheet.id = uuidRandom();
    sheet.name = "Sheet";
    sheet.domain = domain;
    view = std::make_unique<qt::WorksheetViewModel>(sheet, workspace, selection, distance);
    context = std::unique_ptr<WorksheetContext>(new WorksheetContext{
        *view, selection, maths, render, commands, scheme, QString::fromStdString(sheet.id.str())});
}

World::~World() = default;

std::shared_ptr<const model::DataSource> World::publish(const SourceOptions& options) {
    std::shared_ptr<const model::DataSource> s = makeSource(options, rt.cache());
    maths.addSource(s);
    distance.prefetch(s->id);
    selection.sourcePublished(s);
    return s;
}

bool World::distanceReady(const std::shared_ptr<const model::DataSource>& src) {
    const auto c = distance.chain(lapRef(*src, 1), Priority::Interactive);
    return c != nullptr && c->complete();
}

QRgb pixelAt(const QImage& img, int x, int y) {
    return img.convertToFormat(QImage::Format_ARGB32).pixel(x, y);
}

int countColour(const QImage& image, const QRect& rect, project::Rgba rgba) {
    const QImage img = image.convertToFormat(QImage::Format_ARGB32);
    const QRgb want = qRgb(static_cast<int>((rgba >> 24) & 0xFFU), static_cast<int>((rgba >> 16) & 0xFFU),
                           static_cast<int>((rgba >> 8) & 0xFFU));
    const QRect r = rect.intersected(img.rect());
    int n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = r.left(); x <= r.right(); ++x) {
            if ((line[x] & 0x00FFFFFFU) == (want & 0x00FFFFFFU)) {
                ++n;
            }
        }
    }
    return n;
}

std::vector<project::Rgba> strokeColours(const plot::DrawList& list) {
    // Traces are the polylines drawn under a panel scissor (frames and outlines are drawn outside one).
    std::vector<project::Rgba> out;
    bool clipped = false;
    for (const plot::DrawCmd& c : list.cmds) {
        if (c.kind == plot::CmdKind::Scissor) {
            clipped = c.rect.w > 0.0F;
        } else if (c.kind == plot::CmdKind::Polyline && clipped) {
            out.push_back(c.colour);
        }
    }
    return out;
}

} // namespace esses::widgets::cmptest
