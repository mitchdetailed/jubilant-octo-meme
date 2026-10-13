// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "bench_support.hpp"

#include <esses/analysis/rule_runner.hpp>
#include <esses/base/paths.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/thread_role.hpp>
#include <esses/base/uuid.hpp>
#include <esses/project/scheme.hpp>
#include <esses/project/shipped_data.hpp>
#include <esses/units/unit_registry.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace esses::bench {

namespace {

Paths resolvedPaths() {
    auto p = Paths::resolve();
    if (!p) {
        throw std::runtime_error("Paths::resolve: " + p.error().toString());
    }
    return *p;
}

std::shared_ptr<const units::UnitRegistry> loadUnits(const Paths& paths) {
    auto r = units::UnitRegistry::loadFiles(paths.dataDir() / "units.json");
    if (!r) {
        throw std::runtime_error("units: " + r.error().toString());
    }
    return std::make_shared<const units::UnitRegistry>(std::move(r).value());
}

std::shared_ptr<const project::Scheme> lightScheme(const Paths& paths) {
    std::vector<std::string> log;
    auto s = project::Scheme::load(project::shippedFile(paths, project::kLightScheme), log);
    if (!s) {
        throw std::runtime_error("scheme: " + s.error().toString());
    }
    return std::make_shared<const project::Scheme>(std::move(s).value());
}

} // namespace

Stats statsOf(std::vector<double> samples) {
    Stats s;
    s.n = samples.size();
    if (samples.empty()) {
        return s;
    }
    std::sort(samples.begin(), samples.end());
    const auto at = [&samples](double q) {
        const double pos = q * static_cast<double>(samples.size() - 1);
        const auto lo = static_cast<std::size_t>(std::floor(pos));
        const auto hi = static_cast<std::size_t>(std::ceil(pos));
        return samples[lo] + (samples[hi] - samples[lo]) * (pos - static_cast<double>(lo));
    };
    s.min = samples.front();
    s.median = at(0.5);
    s.p95 = at(0.95);
    s.max = samples.back();
    return s;
}

QJsonObject toJson(const Stats& s) {
    return QJsonObject{{QStringLiteral("n"), static_cast<qint64>(s.n)},
                       {QStringLiteral("minMs"), s.min},
                       {QStringLiteral("medianMs"), s.median},
                       {QStringLiteral("p95Ms"), s.p95},
                       {QStringLiteral("maxMs"), s.max}};
}

bool ensureBenchFile(const QString& file, const QStringList& genArgs, QString& error) {
    if (QFileInfo::exists(file)) {
        return true;
    }
    const QString gen = QStandardPaths::findExecutable(QStringLiteral("esses-gen-fixtures"),
                                                       {QCoreApplication::applicationDirPath()});
    if (gen.isEmpty()) {
        error = QStringLiteral("esses-gen-fixtures not found next to this executable");
        return false;
    }
    QProcess p;
    p.setProcessChannelMode(QProcess::ForwardedChannels);
    p.start(gen, QStringList{QStringLiteral("--bench"), file} + genArgs);
    if (!p.waitForFinished(30 * 60 * 1000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        error = QStringLiteral("esses-gen-fixtures failed (exit %1)").arg(p.exitCode());
        return false;
    }
    return QFileInfo::exists(file);
}

bool spinUntil(const std::function<bool()>& done, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
    return true;
}

project::ComponentSpec graphSpec(const std::vector<std::vector<std::string>>& groups) {
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
    const QJsonObject body{
        {QStringLiteral("display"),
         QJsonObject{{QStringLiteral("traceStyle"), QStringLiteral("line")},
                     {QStringLiteral("lineWidth"), 1},
                     {QStringLiteral("showGrid"), true},
                     {QStringLiteral("key"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("labels")},
                                                         {QStringLiteral("pos"), QStringLiteral("left")},
                                                         {QStringLiteral("values"), true}}}}},
        {QStringLiteral("groups"), gj}};
    project::ComponentSpec spec;
    spec.id = uuidRandom();
    spec.type = "time_distance";
    spec.caption.show = true;
    spec.caption.autoText = true;
    auto doc = model::OpaqueJson::parse(QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString());
    if (!doc) {
        throw std::runtime_error("graph body: " + doc.error().toString());
    }
    spec.body = std::move(doc).value();
    return spec;
}

int drawnTraceColours(const plot::ComponentBuild& build) {
    QSet<quint32> colours;
    for (const plot::DrawCmd& c : build.list.cmds) {
        if (c.kind == plot::CmdKind::Polyline && c.vertexCount > 8) {
            colours.insert(c.colour);
        }
    }
    return static_cast<int>(colours.size());
}

Harness::Harness(unsigned workers) {
    const Paths paths = resolvedPaths();
    qt::AppRuntimeOptions o;
    o.workers = workers;
    o.cacheBudget = std::size_t{4} << 30;
    o.markGuiThreadNoBlock = true; // as in esses (01 section 5.3 rule 4)
    rt_ = std::make_unique<qt::AppRuntime>(o);
    units_ = loadUnits(paths);
    maths_ = std::make_unique<qt::MathsService>(rt_->pool(), rt_->dispatcher(), units_);
    static_cast<void>(maths_->loadSettingsSchema(paths.dataDir()));
    distance_ = std::make_unique<qt::DistanceService>(*maths_, rt_->dispatcher());
    selection_ = std::make_unique<qt::DataSelectionModel>(nullptr);
    workspace_ = std::make_unique<qt::WorkspaceViewModel>();
    render_ =
        std::make_unique<qt::RenderService>(rt_->pool(), rt_->dispatcher(), *selection_, *maths_, *distance_);
    commands_ = std::make_unique<qt::CommandRegistry>();
    loader_ = std::make_unique<qt::SourceLoader>(rt_->pool(), rt_->dispatcher(), units_);
    loader_->setRuleRunner(analysis::RuleRunner::circuitDefault());
    loader_->setMathsService(maths_.get());
    loader_->setDistanceService(distance_.get());
    loader_->setUseSidecars(false);
    qt::connectSourceLoader(*loader_, *selection_, nullptr);
    QObject::connect(loader_.get(), &qt::SourceLoader::sourcePublished,
                     [this](std::shared_ptr<const model::DataSource> s) { source_ = std::move(s); });
    QObject::connect(loader_.get(), &qt::SourceLoader::openFailed,
                     [this](const QString& path, const QString& message) {
                         failure_ = path + QStringLiteral(": ") + message;
                     });
    sheet_.id = uuidRandom();
    sheet_.name = "Bench";
    sheet_.domain = "time";
    view_ = std::make_unique<qt::WorksheetViewModel>(sheet_, *workspace_, *selection_, *distance_);
    context_ = std::unique_ptr<widgets::WorksheetContext>(
        new widgets::WorksheetContext{*view_, *selection_, *maths_, *render_, *commands_, lightScheme(paths),
                                      QString::fromStdString(sheet_.id.str())});
}

Harness::~Harness() {
    // Everything that holds a Column (sources, the graph's inputs) goes before the runtime's ChunkCache.
    host_.reset();
    context_.reset();
    view_.reset();
    loader_.reset();
    render_.reset();
    distance_.reset();
    maths_.reset();
    selection_.reset();
    source_.reset();
    rt_->shutdown();
    rt_.reset();
    ThreadRole::set(ThreadRole::Normal);
}

widgets::TimeDistanceGraph& Harness::showGraph(const project::ComponentSpec& spec, QSize size) {
    host_ = std::make_unique<QWidget>();
    host_->resize(size);
    widgets::ComponentWidget* c = widgets::ComponentWidget::create(spec, *context_, host_.get());
    graph_ = qobject_cast<widgets::TimeDistanceGraph*>(c);
    if (graph_ == nullptr) {
        throw std::runtime_error("time_distance is not registered (registerBuiltinComponents)");
    }
    graph_->setGeometry(QRect(QPoint(0, 0), size));
    graph_->setFocusedComponent(true); // reports its trace width and sample period to the view (cursor steps)
    host_->show();
    return *graph_;
}

void Harness::open(const std::filesystem::path& file) {
    source_.reset();
    failure_.clear();
    loader_->open(file);
}

bool Harness::idle() const {
    const TaskPool::Stats s = rt_->pool().stats();
    std::size_t queued = 0;
    for (const std::size_t q : s.queued) {
        queued += q;
    }
    return queued == 0 && s.running == 0 && s.parked == 0;
}

void report(QJsonObject& rows, const QString& id, const Stats& runMedians, double limitMs) {
    QTextStream out(stdout);
    const bool pass = runMedians.median <= limitMs;
    out << QStringLiteral("%1  median of %2 run medians %3 ms (min %4, max %5)  limit %6 ms  %7\n")
               .arg(id, -34)
               .arg(runMedians.n)
               .arg(runMedians.median, 0, 'f', 3)
               .arg(runMedians.min, 0, 'f', 3)
               .arg(runMedians.max, 0, 'f', 3)
               .arg(limitMs, 0, 'f', 0)
               .arg(pass ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    out.flush();
    QJsonObject row = toJson(runMedians);
    row.insert(QStringLiteral("limitMs"), limitMs);
    row.insert(QStringLiteral("pass"), pass);
    rows.insert(id, row);
}

QString defaultRuns() {
    const int n = qEnvironmentVariableIntValue("ESSES_BENCH_REPETITIONS");
    return QString::number(n > 0 ? n : 5);
}

QString defaultJsonPath(const QString& name) {
    const QString dir = qEnvironmentVariable("ESSES_BENCH_OUT");
    return dir.isEmpty() ? QString() : QDir(dir).filePath(name + QStringLiteral(".json"));
}

QJsonArray benchmarkEntries(const QJsonObject& rows) {
    QJsonArray out;
    for (auto it = rows.begin(); it != rows.end(); ++it) {
        const QJsonObject r = it->toObject();
        out.append(QJsonObject{{QStringLiteral("name"), it.key()},
                               {QStringLiteral("run_name"), it.key()},
                               {QStringLiteral("run_type"), QStringLiteral("aggregate")},
                               {QStringLiteral("aggregate_name"), QStringLiteral("median")},
                               {QStringLiteral("repetitions"), r.value(QStringLiteral("n"))},
                               {QStringLiteral("real_time"), r.value(QStringLiteral("medianMs"))},
                               {QStringLiteral("cpu_time"), r.value(QStringLiteral("medianMs"))},
                               {QStringLiteral("time_unit"), QStringLiteral("ms")},
                               {QStringLiteral("limit_ms"), r.value(QStringLiteral("limitMs"))}});
    }
    return out;
}

bool writeJson(const QString& path, QJsonObject doc, const QJsonObject& rows) {
    doc.insert(QStringLiteral("benchmarks"), benchmarkEntries(rows));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           f.write(QJsonDocument(doc).toJson(QJsonDocument::Indented)) >= 0;
}

} // namespace esses::bench
