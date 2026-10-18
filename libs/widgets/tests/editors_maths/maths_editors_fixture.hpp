// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The workspace the maths-editors Qt Tests share (header only, included by tests/editors_maths/tst_*.cpp
// after qtmodel's test support): the runtime, a ProjectSession (shipped defaults, optionally a new project
// folder), MathsService, DistanceService, SourceLoader, DataSelectionModel wired as MainWindow wires them
// (connectSourceLoader, connectProjectSession, connectMathsSelection), synthetic sources written as .ld and
// opened through the loader, and helpers for maths modules and extra channels.
#pragma once

#include "../../../qtmodel/tests/qtmodel_test_support.hpp"

#include <esses/base/paths.hpp>
#include <esses/base/utf8.hpp>
#include <esses/maths/math_module.hpp>
#include <esses/model/channel.hpp>
#include <esses/model/source.hpp>
#include <esses/qtmodel/app_runtime.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/session_restore.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/store/column.hpp>
#include <esses/store/memory_provider.hpp>
#include <esses/store/series.hpp>

#include <QSignalSpy>
#include <QTest>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace esses::widgets::mathstest {

inline Paths pathsFor(const std::filesystem::path& config) {
    PathOverrides o;
    o.configDir = config;
    o.dataDir = qt::test::repoRoot() / "data";
    o.cacheDir = config / "cache";
    o.logDir = config / "logs";
    auto p = Paths::resolve(o);
    if (!p) {
        throw std::runtime_error("Paths::resolve failed: " + p.error().toString());
    }
    return std::move(p).value();
}

inline qt::AppRuntimeOptions runtimeOptions() {
    qt::AppRuntimeOptions o;
    o.workers = 4;
    o.cacheBudget = std::size_t{256} << 20;
    return o;
}

inline maths::MathItem item(const std::string& name, const std::string& expr) {
    maths::MathItem i;
    i.name = name;
    i.expr = expr;
    return i;
}

inline maths::MathModule module(const std::string& id, std::vector<maths::MathItem> items) {
    maths::MathModule m;
    m.id = id;
    m.items = std::move(items);
    return m;
}

// A copy of `src` with one more channel of the Normal layer (values in the base unit of `unitSymbol`).
inline std::shared_ptr<model::DataSource> withChannel(const model::DataSource& src, const std::string& name,
                                                      const std::string& unitSymbol,
                                                      const std::vector<float>& values,
                                                      store::ChunkCache& cache, int dps = 2) {
    const units::UnitRegistry& reg = *qt::test::shippedUnits();
    auto copy = std::make_shared<model::DataSource>(src);
    model::DataLayer& layer = copy->layers.front();
    const Rate rate = Rate::fromHz(50);
    model::Channel c;
    c.desc.name = name;
    c.desc.loggedName = name;
    c.desc.id = model::channelIdLogged(copy->id, model::kNormalLayer, name, "motec.ld");
    c.desc.unit = *reg.find(unitSymbol);
    c.desc.quantity = reg.quantityOf(c.desc.unit);
    c.desc.dtype = store::Dtype::F32;
    c.desc.rate = rate;
    c.desc.dps = dps;
    store::ColumnOptions co;
    co.cache = &cache;
    c.series = store::UniformSeries{
        rate, 0, store::Column(store::InMemoryProvider::fromF32(values, {}, "memory:" + name), co)};
    layer.addChannel(std::move(c));
    return copy;
}

// The number of 50 Hz samples of a synthetic source's Normal layer.
inline std::size_t sampleCount(const model::DataSource& src) {
    return static_cast<std::size_t>(
        std::llround(static_cast<double>(src.layers.front().duration) * 50.0 / 1e6));
}

struct Workspace {
    explicit Workspace(const std::string& name, bool project = false)
        : rt(runtimeOptions()), dir(qt::test::scratchDir(name)), session(pathsFor(dir / "config")),
          maths(rt.pool(), rt.dispatcher(), qt::test::shippedUnits()), distance(maths, rt.dispatcher()),
          loader(rt.pool(), rt.dispatcher(), qt::test::shippedUnits()) {
        if (!session.loadDefaults().ok()) {
            throw std::runtime_error("loadDefaults failed");
        }
        session.setUnits(qt::test::shippedUnits());
        if (project && !session.newProject(dir / "Project", "Project").ok()) {
            throw std::runtime_error("newProject failed");
        }
        selection = std::make_unique<qt::DataSelectionModel>(&session);
        static_cast<void>(maths.loadSettingsSchema(qt::test::repoRoot() / "data"));
        loader.setRuleRunner(analysis::RuleRunner::circuitDefault());
        loader.setMathsService(&maths);
        loader.setDistanceService(&distance);
        qt::connectSourceLoader(loader, *selection, nullptr);
        qt::connectProjectSession(session, &maths, &loader);
        qt::connectMathsSelection(*selection, maths, &distance);
    }
    ~Workspace() = default; // services first, the runtime last (it shuts down in its destructor)
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    // Writes `src` as <dir>/<name>.ld and opens it through the loader; the published snapshot.
    std::shared_ptr<const model::DataSource> open(const model::DataSource& src, const std::string& name) {
        const std::filesystem::path ld = qt::test::writeLd(src, dir, name);
        QSignalSpy published(&loader, &qt::SourceLoader::sourcePublished);
        loader.open(ld);
        if (!QTest::qWaitFor([&published] { return published.count() >= 1; }, 30000)) {
            throw std::runtime_error("the source did not publish");
        }
        const auto id = published.first().at(0).value<std::shared_ptr<const model::DataSource>>()->id;
        return loader.source(id);
    }

    std::shared_ptr<const model::DataSource> openSynthetic(const std::string& name,
                                                           qt::test::SyntheticOptions o = {}) {
        o.path = name + ".ld";
        const auto src = qt::test::makeSource(o, rt.cache());
        return open(*src, name);
    }

    void setModules(std::vector<maths::MathModule> modules) {
        session.undoStack().push(
            new qt::EditMathModulesCommand(session, std::move(modules), QStringLiteral("Modules")));
    }

    qt::AppRuntime rt;
    std::filesystem::path dir;
    qt::ProjectSession session;
    qt::MathsService maths;
    qt::DistanceService distance;
    qt::SourceLoader loader;
    std::unique_ptr<qt::DataSelectionModel> selection;
};

inline void registerMetaTypes() {
    qRegisterMetaType<std::shared_ptr<const model::DataSource>>(
        "std::shared_ptr<const esses::model::DataSource>");
    qRegisterMetaType<maths::ChannelKey>("esses::maths::ChannelKey");
    qRegisterMetaType<model::SourceId>("esses::model::SourceId");
}

} // namespace esses::widgets::mathstest
