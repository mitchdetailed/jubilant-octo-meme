// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelStatusModel (06 section 10; AC-M2-12): maths items and built-in outputs, constants, aliases,
// mappings and missing workbook references, with their status and resolution trace.

#include "../range_lookup.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/utf8.hpp>
#include <esses/qtmodel/channel_status_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/render_service.hpp>

#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace esses::qt {

namespace {

QString qs(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString kindText(maths::ItemKind k) {
    switch (k) {
    case maths::ItemKind::Expression:
        return QStringLiteral("Expression");
    case maths::ItemKind::Filter:
        return QStringLiteral("Filter");
    case maths::ItemKind::ScaleOffset:
        return QStringLiteral("Scale & Offset");
    case maths::ItemKind::Table2D:
        return QStringLiteral("2D Table");
    case maths::ItemKind::Table3D:
        return QStringLiteral("3D Table");
    case maths::ItemKind::Builtin:
        return QStringLiteral("Built-in");
    case maths::ItemKind::Plugin:
        return QStringLiteral("Plugin");
    case maths::ItemKind::Regression:
        return QStringLiteral("Regression");
    }
    return QStringLiteral("Expression");
}

// Channel Status text of 03 section 7.3.
QString statusText(const maths::MathStatus& st) {
    switch (st.state) {
    case maths::MathState::Pending:
        return QStringLiteral("Pending");
    case maths::MathState::Evaluating:
        return st.progress > 0.0F
                   ? QStringLiteral("Calculating %1 %").arg(static_cast<int>(st.progress * 100.0F + 0.5F))
                   : QStringLiteral("Calculating");
    case maths::MathState::Ok:
        return QStringLiteral("OK");
    case maths::MathState::Warning:
        return QStringLiteral("Warning: ") + qs(st.summary);
    case maths::MathState::Error:
        return QStringLiteral("Error: ") + qs(st.summary);
    case maths::MathState::Disabled:
        return st.summary.starts_with("Disabled") ? qs(st.summary) : QStringLiteral("Disabled");
    }
    return {};
}

QString traceLines(const model::ResolutionTrace& t) {
    QStringList lines;
    for (const model::ResolutionStep& s : t.steps) {
        QString line = qs(model::resolutionStepKindName(s.kind)) + QStringLiteral(": '") + qs(s.from) +
                       QStringLiteral("'");
        if (!s.to.empty()) {
            line += QStringLiteral(" -> '") + qs(s.to) + QStringLiteral("'");
        }
        if (!s.note.empty()) {
            line += QStringLiteral(" (") + qs(s.note) + QStringLiteral(")");
        }
        lines.push_back(line);
    }
    return lines.join(QLatin1Char('\n'));
}

QString keyText(const maths::ChannelKey& key) {
    return qs(key.src.hex()) + QLatin1Char('/') + qs(key.layer) + QLatin1Char('/') + qs(key.nameCf);
}

QString sourceName(const model::DataSource& src) {
    const std::string name = pathToUtf8(src.prov.path.filename());
    return name.empty() ? qs(src.id.hex().substr(0, 8)) : qs(name);
}

QString rateText(const std::optional<model::ChannelDescriptor>& d) {
    if (!d || !d->rate || !d->rate->valid()) {
        return {};
    }
    return QString::number(d->rate->hz(), 'g', 6) + QStringLiteral(" Hz");
}

QString unitText(const units::UnitRegistry* units, units::UnitId unit) {
    if (units == nullptr || unit == units::kNoUnit || !units->contains(unit)) {
        return {};
    }
    return qs(units->unit(unit).label());
}

// An alias whose candidates are maths outputs: the first enabled candidate that names one (the resolver sees
// the logged channels only).
std::optional<model::ResolutionTrace> aliasThroughMaths(const model::AliasRow& alias,
                                                        const MathsService& maths,
                                                        const model::SourceId& source,
                                                        std::string_view layer) {
    for (const model::AliasCandidate& c : alias.candidates) {
        if (!c.enabled || !maths.isMathsChannel(source, layer, c.name)) {
            continue;
        }
        model::ResolutionTrace t;
        t.requested = alias.name;
        t.steps.push_back(
            model::ResolutionStep{model::ResolutionStep::Kind::Alias, alias.name, c.name, "maths"});
        const maths::ChannelKey key = maths::ChannelKey::make(source, layer, c.name);
        if (const std::optional<model::ChannelDescriptor> d = maths.descriptor(key)) {
            t.result = d->id;
            if (const auto& units = maths.units(); units != nullptr && units->contains(d->unit)) {
                t.unitLabel = std::string(units->unit(d->unit).label());
            }
        }
        return t;
    }
    return std::nullopt;
}

} // namespace

ChannelStatusModel::ChannelStatusModel(MathsService& maths, ProjectSession* session, QObject* parent)
    : QAbstractTableModel(parent), maths_(maths), session_(session), coalesce_(new QTimer(this)) {
    coalesce_->setSingleShot(true);
    coalesce_->setInterval(100); // 10 Hz (06 section 10)
    connect(coalesce_, &QTimer::timeout, this, &ChannelStatusModel::refresh);
    connect(&maths_, &MathsService::statusChanged, this, [this] { refreshLater(); });
    if (session_ != nullptr) {
        connect(session_, &ProjectSession::documentsChanged, this, [this] { refreshLater(); });
    }
    rows_ = buildRows();
}

ChannelStatusModel::~ChannelStatusModel() = default;

int ChannelStatusModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int ChannelStatusModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : kColumnCount;
}

QVariant ChannelStatusModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || static_cast<std::size_t>(index.row()) >= rows_.size()) {
        return {};
    }
    const Row& r = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case SourceColumn:
            return r.source;
        case ChannelColumn:
            return r.channel;
        case KindColumn:
            return r.kind;
        case StatusColumn:
            return r.status;
        case ResolvedColumn:
            return r.resolved;
        case RateColumn:
            return r.rate;
        case UnitColumn:
            return r.unit;
        default:
            return {};
        }
    case Qt::ToolTipRole:
        return index.column() == ResolvedColumn || index.column() == StatusColumn ? r.trace : QVariant();
    case TraceRole:
        return r.trace;
    case ErrorRole:
        return r.error;
    case KeyRole:
        return r.key ? keyText(*r.key) : QString();
    case ModuleRole:
        return r.module;
    default:
        return {};
    }
}

QVariant ChannelStatusModel::headerData(int section, Qt::Orientation orientation, int role) const {
    static const std::array<QString, kColumnCount> kHeaders{
        QStringLiteral("Source"), QStringLiteral("Channel"),     QStringLiteral("Kind"),
        QStringLiteral("Status"), QStringLiteral("Resolved to"), QStringLiteral("Rate"),
        QStringLiteral("Unit")};
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 || section >= kColumnCount) {
        return {};
    }
    return kHeaders[static_cast<std::size_t>(section)];
}

void ChannelStatusModel::refreshLater() {
    if (!coalesce_->isActive()) {
        coalesce_->start();
    }
}

void ChannelStatusModel::refresh() {
    coalesce_->stop();
    std::vector<Row> next = buildRows();
    const bool sameRows =
        next.size() == rows_.size() && std::equal(next.begin(), next.end(), rows_.begin(),
                                                  [](const Row& a, const Row& b) { return a.id == b.id; });
    if (sameRows) {
        rows_ = std::move(next);
        if (!rows_.empty()) {
            Q_EMIT dataChanged(index(0, 0), index(static_cast<int>(rows_.size()) - 1, kColumnCount - 1));
        }
        return;
    }
    beginResetModel();
    rows_ = std::move(next);
    endResetModel();
}

int ChannelStatusModel::rowOf(const maths::ChannelKey& key) const {
    int first = -1;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].key && *rows_[i].key == key) {
            if (rows_[i].head) {
                return static_cast<int>(i);
            }
            if (first < 0) {
                first = static_cast<int>(i);
            }
        }
    }
    return first;
}

std::vector<ChannelStatusModel::Row> ChannelStatusModel::buildRows() const {
    std::vector<Row> rows;
    const std::shared_ptr<const units::UnitRegistry>& units = maths_.units();

    // Maths items and built-in outputs (chain order per layer).
    std::map<std::string, int> seen; // identity -> occurrences (repeated names in one module)
    for (const MathsService::StatusRow& s : maths_.statusRows()) {
        const auto src = maths_.source(s.source);
        Row r;
        r.source = src != nullptr ? sourceName(*src) : qs(s.source.hex().substr(0, 8));
        r.channel = qs(s.name);
        r.kind = kindText(s.kind);
        r.status = statusText(s.status);
        r.error = s.status.state == maths::MathState::Error;
        r.module = qs(s.moduleId);
        r.key = s.key;
        r.head = s.descriptor.has_value();
        r.resolved = s.kind == maths::ItemKind::Builtin
                         ? qs("'" + s.name + "' \xE2\x86\x92 built-in " + s.moduleId)
                         : qs(s.trace.summary());
        r.trace = traceLines(s.trace);
        if (s.descriptor) {
            r.rate = rateText(s.descriptor);
            r.unit = unitText(units.get(), s.descriptor->unit);
        }
        const std::string identity =
            "m/" + s.source.hex() + "/" + s.layer + "/" + s.moduleId + "/" + s.key.nameCf;
        r.id = qs(identity + "/" + std::to_string(seen[identity]++));
        rows.push_back(std::move(r));
    }

    // Constants of the enabled global modules.
    for (const maths::MathModule& m : maths_.modules()) {
        if (!m.enabled) {
            continue;
        }
        for (const maths::MathConstant& c : m.constants) {
            Row r;
            r.source = QStringLiteral("Project");
            r.channel = qs(c.name);
            r.kind = QStringLiteral("Constant");
            r.status = c.enabled ? QStringLiteral("OK") : QStringLiteral("Disabled");
            r.module = qs(m.id);
            QString value = QString::number(c.value.v, 'g', 12);
            if (const QString u = unitText(units.get(), c.value.unit); !u.isEmpty()) {
                value += QLatin1Char(' ') + u;
            }
            r.resolved = value;
            r.unit = unitText(units.get(), c.value.unit);
            r.id = qs("c/" + m.id + "/" + caseFold(c.name));
            rows.push_back(std::move(r));
        }
    }

    if (session_ == nullptr) {
        return rows;
    }
    const model::ChannelResolver resolver(session_->mappingTable(), session_->aliasTable(), units.get());
    std::vector<std::shared_ptr<const model::DataSource>> sources;
    for (const model::SourceId& id : maths_.sources()) {
        if (auto src = maths_.source(id)) {
            sources.push_back(std::move(src));
        }
    }

    // Aliases per open source.
    for (const auto& src : sources) {
        const model::DataLayer* layer = detail::layerOf(*src, model::kNormalLayer);
        if (layer == nullptr) {
            continue;
        }
        for (const model::AliasRow& alias : session_->aliasTable().rows) {
            Row r;
            r.source = sourceName(*src);
            r.channel = qs(alias.name);
            r.kind = QStringLiteral("Alias");
            model::ChannelResolver::Resolution res = resolver.resolve(alias.name, *layer, src->id);
            model::ResolutionTrace trace = res.trace;
            if (!trace.resolved()) {
                if (std::optional<model::ResolutionTrace> viaMaths =
                        aliasThroughMaths(alias, maths_, src->id, model::kNormalLayer)) {
                    trace = std::move(*viaMaths);
                }
            }
            if (trace.resolved()) {
                r.status = QStringLiteral("OK");
            } else {
                const auto error = std::find_if(trace.steps.begin(), trace.steps.end(), [](const auto& s) {
                    return std::string_view(s.note).starts_with("Error");
                });
                r.status = error != trace.steps.end() ? qs(error->note) : QStringLiteral("(missing)");
                r.error = error != trace.steps.end();
            }
            if (res.channel != nullptr) {
                r.rate = rateText(res.channel->desc);
                r.unit = unitText(units.get(), res.channel->desc.unit);
            }
            r.resolved = trace.resolved() ? qs(trace.summary()) : QStringLiteral("(missing)");
            r.trace = traceLines(trace);
            r.id = qs("a/" + src->id.hex() + "/" + caseFold(alias.name));
            rows.push_back(std::move(r));
        }
    }

    // Mappings: per source that logs the name; a mapping no open source uses is one Project row.
    for (const project::Mappings::Row& m : session_->mappings().rows) {
        bool used = false;
        for (const auto& src : sources) {
            const model::DataLayer* layer = detail::layerOf(*src, model::kNormalLayer);
            if (layer == nullptr ||
                (layer->findLogged(m.from) == nullptr && layer->find(m.from) == nullptr)) {
                continue;
            }
            used = true;
            model::ChannelResolver::Resolution res = resolver.resolve(m.from, *layer, src->id);
            Row r;
            r.source = sourceName(*src);
            r.channel = qs(m.to);
            r.kind = QStringLiteral("Mapping");
            r.status = res.trace.resolved() ? QStringLiteral("OK") : QStringLiteral("(missing)");
            // The mapping is applied at load (05 section 2 step 4c): show the hop even when the resolver's
            // trace records it as a rename.
            r.resolved = qs("'" + m.from + "' \xE2\x86\x92 mapping \xE2\x86\x92 '" + m.to + "'");
            if (res.channel != nullptr) {
                r.rate = rateText(res.channel->desc);
                r.unit = unitText(units.get(), res.channel->desc.unit);
                if (!r.unit.isEmpty()) {
                    r.resolved += QStringLiteral(" (") + r.unit + QStringLiteral(")");
                }
            }
            r.trace = traceLines(res.trace);
            r.id = qs("p/" + src->id.hex() + "/" + caseFold(m.from));
            rows.push_back(std::move(r));
        }
        if (!used) {
            Row r;
            r.source = QStringLiteral("Project");
            r.channel = qs(m.to);
            r.kind = QStringLiteral("Mapping");
            r.status = QStringLiteral("Not used");
            r.resolved = qs("'" + m.from + "' \xE2\x86\x92 missing");
            r.trace =
                QStringLiteral("Mapping: '%1' -> '%2' (no open source logs '%1')").arg(qs(m.from), qs(m.to));
            r.id = qs("p/-/" + caseFold(m.from));
            rows.push_back(std::move(r));
        }
    }

    // Workbook references that resolve in no way: "(missing)", never dropped (06 section 10).
    std::vector<std::string> names;
    std::set<std::string> namesCf;
    for (const project::WorkbookFile& wb : session_->workbooks()) {
        for (const project::WorksheetDef& ws : wb.worksheets) {
            for (const project::ComponentSpec& c : ws.components) {
                for (const ChannelRequest& req : componentChannels(c)) {
                    if (!req.name.empty() && namesCf.insert(caseFold(req.name)).second) {
                        names.push_back(req.name);
                    }
                }
            }
        }
    }
    for (const auto& src : sources) {
        const model::DataLayer* layer = detail::layerOf(*src, model::kNormalLayer);
        if (layer == nullptr) {
            continue;
        }
        for (const std::string& name : names) {
            if (maths_.isMathsChannel(src->id, model::kNormalLayer, name)) {
                continue;
            }
            const model::ChannelResolver::Resolution res = resolver.resolve(name, *layer, src->id);
            if (res.channel != nullptr) {
                continue;
            }
            const model::AliasRow* alias = resolver.findAlias(name);
            if (alias != nullptr && aliasThroughMaths(*alias, maths_, src->id, model::kNormalLayer)) {
                continue;
            }
            Row r;
            r.source = sourceName(*src);
            r.channel = qs(name);
            r.kind = QStringLiteral("Reference");
            r.status = QStringLiteral("(missing)");
            r.resolved = QStringLiteral("(missing)");
            model::ResolutionTrace trace = res.trace;
            if (trace.steps.empty() || trace.steps.back().kind != model::ResolutionStep::Kind::Missing) {
                trace.steps.push_back(model::ResolutionStep{
                    model::ResolutionStep::Kind::Missing, name, {}, "referenced by a workbook"});
            }
            r.trace = traceLines(trace);
            r.id = qs("r/" + src->id.hex() + "/" + caseFold(name));
            rows.push_back(std::move(r));
        }
    }
    return rows;
}

} // namespace esses::qt
