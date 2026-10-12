// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "qtmodel_test_support.hpp"

#include <esses/base/hash.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/registry.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/marker.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/store/series.hpp>

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace esses::qt::test {

std::filesystem::path repoRoot() {
    return pathFromUtf8(ESSES_TEST_REPO_ROOT);
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

QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

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

} // namespace

std::shared_ptr<const model::DataSource> makeSource(const SyntheticOptions& o, store::ChunkCache& cache) {
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
        throttle[i] = static_cast<float>(50.0 + 50.0 * std::cos(2.0 * std::numbers::pi * u));
    }

    model::DataLayer layer;
    layer.name = std::string(model::kNormalLayer);
    layer.duration = sec(total);
    if (o.wheelSpeeds) {
        const units::UnitId ms = *reg.find("m/s");
        layer.addChannel(channel(src->id, "Wheel Speed FL", ms, rate, fl, cache));
        layer.addChannel(channel(src->id, "Wheel Speed FR", ms, rate, fr, cache));
    }
    if (o.throttle) {
        layer.addChannel(channel(src->id, "Throttle", *reg.find("%"), rate, throttle, cache));
    }
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

const model::Range& rangeAt(const model::DataSource& src, std::size_t index, const std::string& groupPath) {
    const model::RangeGroup* g = src.layers.front().ranges.find(groupPath);
    if (g == nullptr || index >= g->ranges.size()) {
        throw std::runtime_error("no range " + std::to_string(index) + " in " + groupPath);
    }
    return g->ranges[index];
}

std::size_t rangeCount(const model::DataSource& src, const std::string& groupPath) {
    const model::RangeGroup* g = src.layers.front().ranges.find(groupPath);
    return g == nullptr ? 0 : g->ranges.size();
}

model::RangeRef rangeRef(const model::DataSource& src, std::size_t index, const std::string& groupPath) {
    const model::Range& r = rangeAt(src, index, groupPath);
    model::RangeRef ref;
    ref.src = src.id;
    ref.layer = std::string(model::kNormalLayer);
    ref.groupPath = groupPath;
    ref.range = r.id;
    ref.name = r.name;
    return ref;
}

std::filesystem::path writeLd(const model::DataSource& src, const std::filesystem::path& dir,
                              const std::string& name) {
    const auto registry = formats::ImporterRegistry::withBuiltins(shippedUnits().get());
    formats::ExportView view;
    view.source = std::make_shared<const model::DataSource>(src);
    const std::filesystem::path out = dir / pathFromUtf8(name + ".ld");
    auto written = registry->writer("motec.ld")->write(out, view, {}, {}, {});
    if (!written) {
        throw std::runtime_error("cannot write " + pathToUtf8(out) + ": " + written.error().toString());
    }
    return out;
}

std::filesystem::path scratchDir(const std::string& name) {
    const std::filesystem::path dir =
        pathFromUtf8(ESSES_TEST_BINARY_DIR) / "qtmodel-scratch" / pathFromUtf8(name);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace esses::qt::test
