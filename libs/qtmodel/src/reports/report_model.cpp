// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ReportModel (04 sections 12-13; D-199): the inputs on the GUI thread, the engines on the pool, the display
// grid. Owner: the reports work (src/reports/).

#include "../channel_resolution.hpp"
#include "../range_lookup.hpp"

#include <esses/base/future.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/model/layer.hpp>
#include <esses/plotcore/axis_layout.hpp>
#include <esses/plotcore/domain_mapper.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/report_model.hpp>

#include <QMetaObject>
#include <QPointer>
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace esses::qt {

namespace {

using analysis::ReportCellState;
using analysis::ReportSlotRole;

QString fromUtf8(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

ReportSlotRole roleOf(plot::SlotKind kind) {
    switch (kind) {
    case plot::SlotKind::Main:
        return ReportSlotRole::Main;
    case plot::SlotKind::Overlay0:
        return ReportSlotRole::Overlay0;
    case plot::SlotKind::Overlay:
        return ReportSlotRole::Overlay;
    }
    return ReportSlotRole::Main;
}

// The channel table of one run: (source, canonical layer, name) -> what the lookup returns.
using ChannelKey = std::tuple<const model::DataSource*, std::string, std::string>;
using ChannelMap = std::map<ChannelKey, analysis::ReportChannelData>;

plot::TimeFormat timeFormatOf(const QString& component, const QString& project, int& decimals) {
    decimals = 3;
    if (component == QLatin1String("s.mmm")) {
        return plot::TimeFormat::Seconds;
    }
    if (component == QLatin1String("m:ss.mmm")) {
        return plot::TimeFormat::MinutesSeconds;
    }
    if (project == QLatin1String("ss.fff")) {
        return plot::TimeFormat::Seconds;
    }
    if (project == QLatin1String("h:mm:ss.f")) {
        decimals = 1;
        return plot::TimeFormat::Hours;
    }
    return plot::TimeFormat::MinutesSeconds;
}

QString joinGrid(const QStringList& headers, const std::vector<QStringList>& rows, QChar separator) {
    QString out;
    const auto line = [&](const QStringList& fields) {
        QStringList quoted;
        for (const QString& f : fields) {
            quoted << ReportModel::quoteField(f, separator);
        }
        out += quoted.join(separator) + QLatin1Char('\n');
    };
    line(headers);
    for (const QStringList& r : rows) {
        line(r);
    }
    return out;
}

} // namespace

// ---- the run and its outcome ----------------------------------------------------------------------------

struct ReportModel::Run {
    Kind kind = Kind::Channel;
    analysis::ReportSpec channelSpec;
    analysis::TimeReportSpec timeSpec;
    analysis::ReportInputs inputs;
    std::vector<int> slotNumbers; // per source: the overlay number (Overlay role), else 0
    std::shared_ptr<analysis::StatCache> cache;
    std::shared_ptr<const units::UnitRegistry> units; // keeps inputs.units alive on the worker
};

struct ReportModel::Outcome {
    bool cancelled = false;
    QString error;
    std::optional<analysis::ReportTable> channel;
    std::optional<analysis::TimeReportTable> time;
};

struct ReportModel::Impl {
    Impl(TaskPool& p, Dispatcher& d, DataSelectionModel& s, MathsService& m,
         std::shared_ptr<analysis::StatCache> c)
        : pool(p), dispatcher(d), selection(s), maths(m), cache(std::move(c)) {
        if (cache == nullptr) {
            cache = std::make_shared<analysis::StatCache>();
        }
    }

    struct Cell {
        QString text;
        std::optional<std::uint32_t> rgba;
        bool italic = false;
        bool bold = false;
        int state = -1; // ReportCellState of a value cell, -1 for a label
    };

    TaskPool& pool;
    Dispatcher& dispatcher;
    DataSelectionModel& selection;
    MathsService& maths;
    std::shared_ptr<analysis::StatCache> cache;
    QPointer<DistanceService> distance;
    QString componentId;
    analysis::ReportSpec channelSpec;
    analysis::TimeReportSpec timeSpec;
    Highlight highlight;
    QString timeFormat = QStringLiteral("project");
    QString projectTimeFormat = QStringLiteral("m:ss.fff");
    std::optional<plot::ViewState> zoom;

    bool queued = false;
    int inFlight = 0;
    std::uint64_t submitted = 0; // serial of the newest run
    std::uint64_t shown = 0;     // serial of the run on display
    std::shared_ptr<const Run> given;
    Outcome outcome;

    QStringList headers;
    std::vector<std::vector<Cell>> grid;
    std::vector<int> rowKinds;

    std::optional<std::pair<Micros, Micros>> clipOf(const model::DataSource& src, const model::RangeRef& ref,
                                                    const model::Range& r) {
        if (!zoom) {
            return std::nullopt;
        }
        const model::DataLayer* layer = detail::layerOf(src, ref.layer);
        if (layer == nullptr) {
            return std::nullopt;
        }
        std::shared_ptr<const plot::DistanceChain> chain;
        if (zoom->domain == plot::Domain::Distance) {
            if (distance.isNull()) {
                return std::nullopt;
            }
            chain = distance->chain(ref, Priority::Background);
            if (chain == nullptr) {
                return std::nullopt;
            }
        }
        const plot::DomainMapper mapper(zoom->domain, detail::geomOf(r), detail::outingAt(*layer, r.start),
                                        chain, Rate{}, 0);
        if (zoom->domain == plot::Domain::Distance && !mapper.hasDistance()) {
            return std::nullopt;
        }
        Micros a = mapper.timeAt(zoom->zoom.a);
        Micros b = mapper.timeAt(zoom->zoom.b);
        if (b < a) {
            std::swap(a, b);
        }
        return std::make_pair(a, b);
    }

    // The channel report's ranges in scope (04 section 12 item 2).
    void channelSources(const plot::SelectionSnapshot& snap, Run& run) {
        std::vector<analysis::ReportSource>& out = run.inputs.sources;
        const bool clip = channelSpec.options.zoomLinked && zoom.has_value();
        std::set<std::tuple<model::SourceId, std::string, model::RangeId>> seen;
        const auto add = [&](const std::shared_ptr<const model::DataSource>& src, const model::RangeRef& ref,
                             const model::Range& r, ReportSlotRole role, int number) {
            const std::string canonical(model::canonicalLayerName(ref.layer));
            if (!seen.emplace(ref.src, canonical, ref.range).second) {
                return;
            }
            analysis::ReportSource s;
            s.role = role;
            s.source = src;
            s.layer = canonical;
            s.range = ref;
            if (clip) {
                s.clip = clipOf(*src, ref, r);
            }
            out.push_back(std::move(s));
            run.slotNumbers.push_back(role == ReportSlotRole::Overlay ? number : 0);
        };
        const std::vector<plot::Slot> slots = snap.visibleSlots();
        if (channelSpec.scope == analysis::ReportScope::AllOutings) {
            // Every outing of every open source; the outings holding a selected overlay take its role.
            std::map<std::tuple<model::SourceId, model::RangeId>, std::pair<ReportSlotRole, int>> roles;
            for (const plot::Slot& slot : slots) {
                const auto src = selection.source(slot.range.src);
                const model::Range* r = src == nullptr ? nullptr : detail::findRange(*src, slot.range);
                const model::DataLayer* layer =
                    src == nullptr ? nullptr : detail::layerOf(*src, slot.range.layer);
                const model::Range* outing =
                    (r == nullptr || layer == nullptr) ? nullptr : detail::outingRangeAt(*layer, r->start);
                if (outing != nullptr) {
                    roles.emplace(std::make_tuple(src->id, outing->id),
                                  std::make_pair(roleOf(slot.kind), slot.number));
                }
            }
            for (const std::shared_ptr<const model::DataSource>& src : selection.sources()) {
                const std::string layer(model::kNormalLayer);
                const model::RangeGroup* g = detail::groupOf(*src, layer, detail::kOutingsGroup);
                if (g == nullptr) {
                    continue;
                }
                for (const model::Range& outing : g->ranges) {
                    const auto it = roles.find(std::make_tuple(src->id, outing.id));
                    const ReportSlotRole role = it == roles.end() ? ReportSlotRole::Main : it->second.first;
                    const int number = it == roles.end() ? 0 : it->second.second;
                    add(src, detail::makeRef(*src, layer, detail::kOutingsGroup, outing), outing, role,
                        number);
                }
            }
            return;
        }
        for (const plot::Slot& slot : slots) {
            const auto src = selection.source(slot.range.src);
            const model::Range* r = src == nullptr ? nullptr : detail::findRange(*src, slot.range);
            if (r == nullptr) {
                continue;
            }
            if (channelSpec.scope == analysis::ReportScope::SelectedRanges) {
                add(src, slot.range, *r, roleOf(slot.kind), slot.number);
                continue;
            }
            const model::DataLayer* layer = detail::layerOf(*src, slot.range.layer);
            const model::Range* outing = layer == nullptr ? nullptr : detail::outingRangeAt(*layer, r->start);
            if (outing != nullptr) {
                add(src, detail::makeRef(*src, slot.range.layer, detail::kOutingsGroup, *outing), *outing,
                    roleOf(slot.kind), slot.number);
            }
        }
    }

    // The time report's laps (04 section 13 item 1): of the selected ranges, or of the outings around them.
    void lapSources(const plot::SelectionSnapshot& snap, Run& run) {
        std::set<std::tuple<model::SourceId, std::string, model::RangeId>> seen;
        for (const plot::Slot& slot : snap.visibleSlots()) {
            const auto src = selection.source(slot.range.src);
            const model::Range* r = src == nullptr ? nullptr : detail::findRange(*src, slot.range);
            const model::DataLayer* layer =
                src == nullptr ? nullptr : detail::layerOf(*src, slot.range.layer);
            if (r == nullptr || layer == nullptr) {
                continue;
            }
            const std::string canonical(model::canonicalLayerName(slot.range.layer));
            const bool isLap = slot.range.groupPath == detail::kLapsGroup;
            Micros from = r->start;
            Micros to = r->end;
            if (timeSpec.scope == analysis::ReportScope::SelectedOutings) {
                if (const model::Range* outing = detail::outingRangeAt(*layer, r->start)) {
                    from = outing->start;
                    to = outing->end;
                }
            }
            std::vector<const model::Range*> laps;
            if (isLap && timeSpec.scope == analysis::ReportScope::SelectedRanges) {
                laps.push_back(r);
            } else {
                for (const model::Range* lap : detail::rangesWithin(*layer, detail::kLapsGroup, from, to)) {
                    if (lap->start >= from && lap->end <= to) { // laps inside the range or outing
                        laps.push_back(lap);
                    }
                }
            }
            for (const model::Range* lap : laps) {
                if (!seen.emplace(src->id, canonical, lap->id).second) {
                    continue;
                }
                analysis::ReportSource s;
                s.role = roleOf(slot.kind);
                s.source = src;
                s.layer = canonical;
                s.range = detail::makeRef(*src, canonical, detail::kLapsGroup, *lap);
                run.inputs.sources.push_back(std::move(s));
                run.slotNumbers.push_back(slot.kind == plot::SlotKind::Overlay ? slot.number : 0);
            }
        }
    }

    // Every report channel resolved once per (source, layer) on the GUI thread (MathsService is GUI-only).
    void resolveChannels(Run& run) {
        auto table = std::make_shared<ChannelMap>();
        if (run.kind == Kind::Channel) {
            for (const analysis::ReportSource& s : run.inputs.sources) {
                for (const analysis::ReportChannel& c : run.channelSpec.channels) {
                    ChannelKey key{s.source.get(), s.layer, c.name};
                    if (table->contains(key)) {
                        continue;
                    }
                    const plot::ResolvedChannel rc = detail::resolveChannel(
                        maths, *s.source, s.layer, ChannelRequest{c.name, c.displayUnit, std::nullopt},
                        Priority::Background);
                    analysis::ReportChannelData data;
                    data.series = rc.series;
                    data.descriptor = rc.descriptor;
                    data.status = rc.status;
                    data.toDisplay = rc.toDisplay;
                    data.displayUnit = rc.displayUnit;
                    data.dps = rc.dps;
                    table->emplace(std::move(key), std::move(data));
                }
            }
        }
        run.units = maths.units();
        run.inputs.units = run.units.get();
        run.inputs.channels = [table](const analysis::ReportSource& s, std::string_view name) {
            const auto it = table->find(ChannelKey{s.source.get(), s.layer, std::string(name)});
            return it != table->end() ? it->second : analysis::ReportChannelData{};
        };
    }
};

// ---- ReportModel ----------------------------------------------------------------------------------------

ReportModel::ReportModel(TaskPool& pool, Dispatcher& dispatcher, DataSelectionModel& selection,
                         MathsService& maths, std::shared_ptr<analysis::StatCache> cache, QObject* parent)
    : QAbstractTableModel(parent),
      impl_(std::make_unique<Impl>(pool, dispatcher, selection, maths, std::move(cache))) {}

ReportModel::~ReportModel() = default;

void ReportModel::setChannelReport(const analysis::ReportSpec& spec, const QString& componentId) {
    kind_ = Kind::Channel;
    impl_->channelSpec = spec;
    impl_->componentId = componentId;
    refresh();
}

void ReportModel::setTimeReport(const analysis::TimeReportSpec& spec, const QString& componentId) {
    kind_ = Kind::Time;
    impl_->timeSpec = spec;
    impl_->componentId = componentId;
    refresh();
}

void ReportModel::setHighlight(const Highlight& highlight) {
    impl_->highlight = highlight;
    rebuildGrid();
}

void ReportModel::setTimeFormat(const QString& format) {
    impl_->timeFormat = format;
    rebuildGrid();
}

void ReportModel::setProjectTimeFormat(const QString& format) {
    if (impl_->projectTimeFormat == format) {
        return; // the shell re-applies the project options on every document change
    }
    impl_->projectTimeFormat = format;
    rebuildGrid();
}

void ReportModel::setZoom(std::optional<plot::ViewState> view) {
    impl_->zoom = std::move(view);
}

void ReportModel::setDistanceService(DistanceService* distance) {
    impl_->distance = distance;
}

void ReportModel::refresh() {
    if (impl_->queued) {
        return;
    }
    impl_->queued = true;
    QMetaObject::invokeMethod(this, &ReportModel::run, Qt::QueuedConnection);
}

bool ReportModel::busy() const {
    return impl_->queued || impl_->inFlight > 0;
}

void ReportModel::run() {
    Impl& d = *impl_;
    if (!d.queued) {
        return; // already ran in this turn
    }
    d.queued = false;
    auto given = std::make_shared<Run>();
    given->kind = kind_;
    given->channelSpec = d.channelSpec;
    given->timeSpec = d.timeSpec;
    given->cache = d.cache;
    const plot::SelectionSnapshot snap = d.selection.snapshot();
    if (kind_ == Kind::Channel) {
        d.channelSources(snap, *given);
    } else {
        d.lapSources(snap, *given);
    }
    d.resolveChannels(*given);

    const std::uint64_t serial = ++d.submitted;
    ++d.inFlight;
    if (kind_ == Kind::Channel && given->channelSpec.channels.empty()) {
        // Nothing to compute (ReportEngine refuses a spec without channels): an empty table at once.
        auto outcome = std::make_shared<Outcome>();
        outcome->channel = analysis::ReportTable{};
        outcome->channel->transposed = given->channelSpec.layout == analysis::ReportLayout::ChannelsInColumns;
        deliver(serial, given, outcome);
        return;
    }
    const std::string id = d.componentId.isEmpty()
                               ? "anon-" + std::to_string(reinterpret_cast<std::uintptr_t>(this))
                               : d.componentId.toStdString();
    Future<std::shared_ptr<Outcome>> future =
        d.pool.submit(Priority::Background, TaskKey{"report/" + id}, CancelToken{},
                      [given](const CancelToken& cancel) -> Result<std::shared_ptr<Outcome>> {
                          auto outcome = std::make_shared<Outcome>();
                          if (given->kind == Kind::Channel) {
                              Result<analysis::ReportTable> t = analysis::ReportEngine::cells(
                                  given->channelSpec, given->inputs, *given->cache, cancel);
                              if (!t) {
                                  return Failure{t.error()};
                              }
                              outcome->channel = std::move(t).value();
                          } else {
                              Result<analysis::TimeReportTable> t =
                                  analysis::TimeReportEngine::build(given->timeSpec, given->inputs, cancel);
                              if (!t) {
                                  return Failure{t.error()};
                              }
                              outcome->time = std::move(t).value();
                          }
                          return outcome;
                      });
    Dispatcher* dispatcher = &d.dispatcher; // the posting context (libs/qtmodel/README.md "Threading")
    const QPointer<ReportModel> guard(this);
    future.onReady([dispatcher, guard, serial, given](const Result<std::shared_ptr<Outcome>>& result) {
        std::shared_ptr<Outcome> outcome;
        if (result) {
            outcome = *result;
        } else {
            outcome = std::make_shared<Outcome>();
            outcome->cancelled = result.error().code == ErrorCode::Cancelled;
            outcome->error = fromUtf8(result.error().toString());
        }
        dispatcher->post(dispatcher, [guard, serial, given, outcome]() {
            if (ReportModel* self = guard.data()) {
                self->deliver(serial, given, outcome);
            }
        });
    });
}

void ReportModel::deliver(std::uint64_t serial, const std::shared_ptr<const Run>& given,
                          const std::shared_ptr<Outcome>& outcome) {
    Impl& d = *impl_;
    d.inFlight = std::max(0, d.inFlight - 1);
    if (outcome->cancelled || serial < d.shown) {
        return; // a replaced run, or one older than the table on display
    }
    d.shown = serial;
    d.given = given;
    d.outcome = std::move(*outcome);
    ++deliveries_;
    rebuildGrid();
    Q_EMIT computed();
}

void ReportModel::rebuildGrid() {
    Impl& d = *impl_;
    // Built aside, then published (M3 item 30, D-210 (e)): see the end.
    QStringList headers;
    std::vector<std::vector<Impl::Cell>> grid;
    std::vector<int> rowKinds;
    const auto cellOf = [&d](const analysis::ReportCell& c, const analysis::ReportColumn& col) {
        Impl::Cell out;
        out.state = static_cast<int>(c.state);
        switch (c.state) {
        case ReportCellState::Ok:
            out.text = QString::number(c.value, 'f', std::max(0, col.dps));
            if (c.isMax && d.highlight.max) {
                out.rgba = d.highlight.max;
            } else if (c.isMin && d.highlight.min) {
                out.rgba = d.highlight.min;
            }
            break;
        case ReportCellState::Blank:
            break;
        case ReportCellState::Pending:
            out.text = QString(QChar(0x2026));
            break;
        case ReportCellState::Missing:
            out.text = tr("(missing)");
            out.italic = true;
            break;
        case ReportCellState::Error:
            out.text = tr("(error)");
            out.italic = true;
            break;
        }
        return out;
    };
    if (d.given != nullptr && d.given->kind == Kind::Channel && d.outcome.channel) {
        const analysis::ReportTable& t = *d.outcome.channel;
        const analysis::ReportSpec& spec = d.given->channelSpec;
        QStringList columnLabels;
        for (const analysis::ReportColumn& col : t.columns) {
            QString label =
                col.channel < spec.channels.size() ? fromUtf8(spec.channels[col.channel].name) : QString();
            label += QLatin1Char(' ') + fromUtf8(analysis::reportStatName(col.stat));
            if (col.deltaVsOverlay0) {
                label += QLatin1Char(' ') + QString(QChar(0x0394)) + tr(" vs Overlay 0");
            }
            if (!col.unit.empty()) {
                label += QStringLiteral(" [") + fromUtf8(col.unit) + QLatin1Char(']');
            }
            columnLabels << label;
        }
        QStringList rowLabels;
        for (const analysis::ReportHeading& h : t.rows) {
            QStringList parts;
            for (const std::string& p : h.path) {
                parts << fromUtf8(p);
            }
            QString label = parts.join(QStringLiteral(" / "));
            if (h.role == ReportSlotRole::Overlay0) {
                label += tr(" (Overlay 0)");
            } else if (h.role == ReportSlotRole::Overlay) {
                const int n = h.source < d.given->slotNumbers.size() ? d.given->slotNumbers[h.source] : 0;
                label += n > 0 ? tr(" (Overlay %1)").arg(n) : tr(" (Overlay)");
            }
            rowLabels << label;
        }
        QStringList levels;
        for (const std::string& h : spec.headers) {
            levels << fromUtf8(model::lastGroupSegment(h));
        }
        const QString headingTitle = levels.isEmpty() ? tr("Range") : levels.join(QStringLiteral(" / "));
        if (t.transposed) { // channelsInColumns: a row per heading
            headers << headingTitle << columnLabels;
            for (std::size_t r = 0; r < t.rows.size(); ++r) {
                std::vector<Impl::Cell> row;
                Impl::Cell label;
                label.text = rowLabels[static_cast<qsizetype>(r)];
                label.italic = !t.rows[r].trusted;
                row.push_back(label);
                for (std::size_t c = 0; c < t.columns.size(); ++c) {
                    Impl::Cell cell = cellOf(t.at(r, c), t.columns[c]);
                    cell.italic = cell.italic || !t.rows[r].trusted;
                    row.push_back(std::move(cell));
                }
                grid.push_back(std::move(row));
                rowKinds.push_back(0);
            }
        } else { // channelsInRows: a row per (channel, stat)
            headers << tr("Channel") << rowLabels;
            for (std::size_t c = 0; c < t.columns.size(); ++c) {
                std::vector<Impl::Cell> row;
                Impl::Cell label;
                label.text = columnLabels[static_cast<qsizetype>(c)];
                row.push_back(label);
                for (std::size_t r = 0; r < t.rows.size(); ++r) {
                    Impl::Cell cell = cellOf(t.at(r, c), t.columns[c]);
                    cell.italic = cell.italic || !t.rows[r].trusted;
                    row.push_back(std::move(cell));
                }
                grid.push_back(std::move(row));
                rowKinds.push_back(0);
            }
        }
    } else if (d.given != nullptr && d.given->kind == Kind::Time && d.outcome.time) {
        const analysis::TimeReportTable& t = *d.outcome.time;
        const analysis::TimeReportSpec& spec = d.given->timeSpec;
        int decimals = 3;
        const plot::TimeFormat format = timeFormatOf(d.timeFormat, d.projectTimeFormat, decimals);
        headers << tr("Lap");
        for (const std::string& s : t.sections) {
            headers << fromUtf8(s);
        }
        headers << tr("Lap Time");
        const std::size_t columns = t.sections.size() + 1;
        const auto addRow = [&](const analysis::TimeReportRow& r, int kind) {
            std::vector<Impl::Cell> row;
            Impl::Cell label;
            label.text = fromUtf8(r.name);
            label.italic = !r.trusted;
            row.push_back(label);
            for (std::size_t c = 0; c < columns; ++c) {
                Impl::Cell cell;
                cell.italic = !r.trusted;
                cell.state = static_cast<int>(ReportCellState::Ok);
                if (c < r.cells.size() && r.cells[c].time) {
                    const analysis::TimeReportCell& tc = r.cells[c];
                    const std::optional<Micros> time = tc.time;
                    cell.text = fromUtf8(plot::AxisLayout::formatTime(time.value_or(0), format, decimals));
                    if (kind == 0) {
                        if (tc.band >= 0 && static_cast<std::size_t>(tc.band) < spec.bands.size()) {
                            cell.rgba = spec.bands[static_cast<std::size_t>(tc.band)].colour;
                        }
                        cell.bold = tc.fastest;
                    }
                } else {
                    cell.text = QString(QChar(0x2014));
                    cell.state = static_cast<int>(ReportCellState::Blank);
                }
                row.push_back(std::move(cell));
            }
            grid.push_back(std::move(row));
            rowKinds.push_back(kind);
        };
        for (const analysis::TimeReportRow& r : t.rows) {
            addRow(r, 0);
        }
        if (t.eclectic) {
            addRow(*t.eclectic, 1);
        }
        if (t.rollingMin) {
            addRow(*t.rollingMin, 2);
        }
        if (t.total) {
            addRow(*t.total, 3);
        }
    }
    // The same rows and columns (header labels, row labels, row kinds) as on display: the cells change in
    // place (dataChanged over the whole table, every role), so a view keeps its selection and current cell
    // across a refresh; any other table resets the model.
    const auto sameRows = [&grid, &d] {
        if (grid.size() != d.grid.size()) {
            return false;
        }
        for (std::size_t r = 0; r < grid.size(); ++r) {
            const std::vector<Impl::Cell>& a = grid[r];
            const std::vector<Impl::Cell>& b = d.grid[r];
            if (a.size() != b.size() || (!a.empty() && a.front().text != b.front().text)) {
                return false;
            }
        }
        return true;
    };
    if (headers == d.headers && rowKinds == d.rowKinds && sameRows()) {
        d.grid = std::move(grid);
        if (!d.grid.empty() && !d.headers.isEmpty()) {
            Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
        }
        return;
    }
    beginResetModel();
    d.headers = std::move(headers);
    d.grid = std::move(grid);
    d.rowKinds = std::move(rowKinds);
    endResetModel();
}

int ReportModel::pendingCells() const {
    if (impl_->outcome.channel) {
        return static_cast<int>(impl_->outcome.channel->pending());
    }
    return 0;
}

QString ReportModel::lastError() const {
    return impl_->outcome.error;
}

const std::optional<analysis::ReportTable>& ReportModel::channelTable() const {
    return impl_->outcome.channel;
}

const std::optional<analysis::TimeReportTable>& ReportModel::timeTable() const {
    return impl_->outcome.time;
}

QString ReportModel::quoteField(const QString& field, QChar separator) {
    const bool quote = field.contains(QLatin1Char('"')) || field.contains(QLatin1Char(',')) ||
                       field.contains(QLatin1Char('%')) || field.contains(QLatin1Char('\n')) ||
                       field.contains(QLatin1Char('\r')) || field.contains(separator);
    if (!quote) {
        return field;
    }
    QString out = field;
    out.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + out + QLatin1Char('"');
}

QString ReportModel::toTsv() const {
    std::vector<QStringList> rows;
    for (const auto& r : impl_->grid) {
        QStringList fields;
        for (const Impl::Cell& c : r) {
            fields << c.text;
        }
        rows.push_back(fields);
    }
    return joinGrid(impl_->headers, rows, QLatin1Char('\t'));
}

QString ReportModel::toCsv() const {
    std::vector<QStringList> rows;
    for (const auto& r : impl_->grid) {
        QStringList fields;
        for (const Impl::Cell& c : r) {
            fields << c.text;
        }
        rows.push_back(fields);
    }
    return joinGrid(impl_->headers, rows, QLatin1Char(','));
}

int ReportModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(impl_->grid.size());
}

int ReportModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(impl_->headers.size());
}

QVariant ReportModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount()) {
        return {};
    }
    const auto& row = impl_->grid[static_cast<std::size_t>(index.row())];
    if (static_cast<std::size_t>(index.column()) >= row.size()) {
        return {};
    }
    const Impl::Cell& c = row[static_cast<std::size_t>(index.column())];
    switch (role) {
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
        return c.text;
    case Qt::TextAlignmentRole:
        return index.column() == 0 ? QVariant(static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter))
                                   : QVariant(static_cast<int>(Qt::AlignRight | Qt::AlignVCenter));
    case BackgroundRgbaRole:
        return c.rgba ? QVariant(static_cast<quint32>(c.rgba.value_or(0))) : QVariant();
    case ItalicRole:
        return c.italic;
    case BoldRole:
        return c.bold;
    case CellStateRole:
        return c.state;
    case RowKindRole:
        return impl_->rowKinds[static_cast<std::size_t>(index.row())];
    default:
        return {};
    }
}

QVariant ReportModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) {
        return {};
    }
    if (orientation == Qt::Horizontal) {
        return section >= 0 && section < impl_->headers.size() ? QVariant(impl_->headers[section])
                                                               : QVariant();
    }
    return section + 1;
}

} // namespace esses::qt
