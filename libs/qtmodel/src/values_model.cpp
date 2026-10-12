// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ValuesModel (06 section 8, 04 sections 3.2 and 15; AC-M1-01): values at the cursor for Main and the
// reference overlay through the single conversion rule x_c = map_M.xOf(M.start + cursorT),
// t_k = map_k.timeAt(x_c); never blocks (Resampler::sampleAt reads resident chunks through tryPin).

#include "channel_resolution.hpp"
#include "range_lookup.hpp"

#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/values_model.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/store/resampler.hpp>

#include <QCollator>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <optional>

namespace esses::qt {

namespace {

constexpr int kRetryMs = 50;
const QString kPending = QStringLiteral("…");

struct Cell {
    enum class State : std::uint8_t { Missing, Pending, Error, Invalid, Valid };
    State state = State::Missing;
    double value = 0.0; // display units
    int dps = 2;
    QString unit;
};

QString text(const Cell& c) {
    switch (c.state) {
    case Cell::State::Pending:
        return kPending;
    case Cell::State::Error:
        return QStringLiteral("Error");
    case Cell::State::Valid:
        return QString::number(c.value, 'f', std::clamp(c.dps, 0, 9));
    default:
        return {};
    }
}

} // namespace

ValuesModel::ValuesModel(DataSelectionModel& selection, MathsService& maths, QObject* parent)
    : QAbstractTableModel(parent), selection_(selection), maths_(maths), retry_(new QTimer(this)) {
    retry_->setSingleShot(true);
    retry_->setInterval(kRetryMs);
    connect(retry_, &QTimer::timeout, this, &ValuesModel::recompute);
    connect(&selection_, &DataSelectionModel::changed, this,
            [this](const plot::SelectionSnapshot&, DataSelectionModel::Changes what) {
                if (allChannels_ && (what.testFlag(DataSelectionModel::Change::Main) ||
                                     what.testFlag(DataSelectionModel::Change::Sources))) {
                    reset(); // the rows are the Main source's channels
                } else {
                    recompute();
                }
            });
    connect(&maths_, &MathsService::channelReady, this, [this] { recompute(); });
}

ValuesModel::~ValuesModel() = default;

int ValuesModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int ValuesModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ValuesModel::data(const QModelIndex& index, int role) const {
    if (role != Qt::DisplayRole || !index.isValid()) {
        return {};
    }
    return cellText(index.row(), index.column());
}

QVariant ValuesModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case NameCol:
        return tr("Name");
    case MainCol:
        return tr("Main");
    case ReferenceCol:
        return tr("Overlay");
    case DeltaCol:
        return tr("Δ");
    case UnitCol:
        return tr("Unit");
    default:
        return {};
    }
}

std::vector<ChannelRequest> ValuesModel::effectiveRows() const {
    std::vector<ChannelRequest> rows;
    if (allChannels_) {
        const auto src = selection_.hasMain() ? selection_.source(selection_.snapshot().main.src) : nullptr;
        const model::DataLayer* layer =
            src == nullptr ? nullptr : detail::layerOf(*src, selection_.snapshot().main.layer);
        if (layer != nullptr) {
            QStringList names;
            for (const model::Channel& c : layer->channels) {
                names << QString::fromStdString(c.desc.name);
            }
            for (const maths::ChannelKey& key : maths_.outputs(src->id, layer->name)) {
                if (const auto d = maths_.descriptor(key)) {
                    names << QString::fromStdString(d->name);
                }
            }
            QCollator collator;
            collator.setNumericMode(true);
            collator.setCaseSensitivity(Qt::CaseInsensitive);
            std::stable_sort(names.begin(), names.end(), [&collator](const QString& a, const QString& b) {
                return collator.compare(a, b) < 0;
            });
            for (const QString& n : names) {
                rows.push_back(ChannelRequest{n.toStdString(), {}, std::nullopt});
            }
        }
        return rows;
    }
    if (!requests_.empty()) {
        return requests_;
    }
    for (const QString& n : channels_) {
        rows.push_back(ChannelRequest{n.toStdString(), {}, std::nullopt});
    }
    return rows;
}

void ValuesModel::reset() {
    beginResetModel();
    rows_ = effectiveRows();
    cells_.assign(rows_.size(), RowCells{});
    endResetModel();
    recompute();
}

void ValuesModel::setChannels(const QStringList& names) {
    channels_ = names;
    requests_.clear();
    reset();
}

void ValuesModel::setChannelRequests(const std::vector<ChannelRequest>& requests) {
    requests_ = requests;
    channels_.clear();
    for (const ChannelRequest& r : requests) {
        channels_ << QString::fromStdString(r.name);
    }
    reset();
}

void ValuesModel::setAllChannels(bool on) {
    if (allChannels_ == on) {
        return;
    }
    allChannels_ = on;
    reset();
}

void ValuesModel::setWorksheet(WorksheetViewModel* worksheet) {
    // Both connections are tracked, so re-pointing the model (every worksheet switch) never accumulates
    // connections on a worksheet.
    disconnect(worksheetConnection_);
    disconnect(destroyedConnection_);
    worksheet_ = worksheet;
    if (worksheet_ != nullptr) {
        worksheetConnection_ = connect(worksheet_, &WorksheetViewModel::changed, this, &ValuesModel::refresh);
        destroyedConnection_ = connect(worksheet_, &QObject::destroyed, this, [this] {
            worksheet_ = nullptr;
            recompute();
        });
    }
    recompute();
}

void ValuesModel::refresh(plot::ChangeMask) {
    recompute();
}

void ValuesModel::recompute() {
    bool pending = false;
    const plot::SelectionSnapshot& snap = selection_.snapshot();
    const bool live = worksheet_ != nullptr && selection_.hasMain();
    const plot::ViewState view = live ? worksheet_->state() : plot::ViewState{};
    DistanceService* distance = live ? &worksheet_->distanceService() : nullptr;

    // The mapper of a slot (04 section 3.2): its range geometry with offsets, outing and chain.
    const auto mapperOf = [&](const model::RangeRef& ref) -> std::optional<plot::DomainMapper> {
        const auto src = selection_.source(ref.src);
        const model::Range* r = src == nullptr ? nullptr : detail::findRange(*src, ref);
        const model::DataLayer* layer = src == nullptr ? nullptr : detail::layerOf(*src, ref.layer);
        if (r == nullptr || layer == nullptr) {
            return std::nullopt;
        }
        const plot::OutingGeom outing = r->kind == model::RangeKind::Outing
                                            ? plot::OutingGeom{r->start, r->end}
                                            : detail::outingAt(*layer, r->start);
        return plot::DomainMapper(view.domain, detail::geomOf(*r), outing,
                                  distance->chain(ref, Priority::Visible), Rate{}, 0);
    };
    std::optional<plot::DomainMapper> mainMap;
    std::optional<model::RangeRef> reference;
    std::optional<plot::DomainMapper> refMap;
    double xc = 0.0;
    if (live) {
        mainMap = mapperOf(snap.main);
        reference = snap.referenceOverlay();
        if (reference) {
            refMap = mapperOf(*reference);
        }
        if (mainMap) {
            xc = mainMap->xOf(mainMap->range().start + view.cursorT);
        }
    }
    const auto cellFor = [&](const model::RangeRef& ref, const std::optional<plot::DomainMapper>& map,
                             const ChannelRequest& request, bool isMain) {
        Cell c;
        const auto src = selection_.source(ref.src);
        if (src == nullptr || !map) {
            return c;
        }
        const plot::ResolvedChannel rc =
            detail::resolveChannel(maths_, *src, ref.layer, request, Priority::Background);
        c.dps = rc.dps;
        c.unit = QString::fromStdString(rc.displayUnit);
        if (!rc.descriptor) {
            return c; // not in this source
        }
        if (rc.status == model::ChannelStatus::Error) {
            c.state = Cell::State::Error;
            return c;
        }
        if (!rc.series) {
            c.state = Cell::State::Pending;
            return c;
        }
        if (view.domain == plot::Domain::Distance && !map->hasDistance()) {
            return c; // no distance data for this source
        }
        const Micros t = isMain ? map->range().start + view.cursorT : map->timeAt(xc);
        const store::ValueSample s = store::Resampler::sampleAt(*rc.series, t, rc.descriptor->interp);
        if (s.state == store::SampleState::Absent) {
            c.state = Cell::State::Pending;
        } else if (s.state == store::SampleState::Invalid) {
            c.state = Cell::State::Invalid;
        } else {
            c.state = Cell::State::Valid;
            c.value = rc.toDisplay(s.value);
        }
        return c;
    };
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        RowCells cells;
        if (live) {
            const Cell m = cellFor(snap.main, mainMap, rows_[i], true);
            const Cell r = reference ? cellFor(*reference, refMap, rows_[i], false) : Cell{};
            cells.main = text(m);
            cells.reference = text(r);
            cells.unit = m.unit;
            if (m.state == Cell::State::Valid && r.state == Cell::State::Valid) {
                const double d = m.value - r.value;
                cells.delta = (d >= 0.0 ? QStringLiteral("+") : QString()) +
                              QString::number(d, 'f', std::clamp(m.dps, 0, 9));
            }
            cells.pending = m.state == Cell::State::Pending || r.state == Cell::State::Pending;
            pending = pending || cells.pending;
        }
        cells_[i] = std::move(cells);
    }
    if (pending) {
        retry_->start();
    }
    if (!rows_.empty()) {
        Q_EMIT dataChanged(index(0, MainCol), index(static_cast<int>(rows_.size()) - 1, UnitCol));
    }
}

QString ValuesModel::cellText(int row, int column) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return {};
    }
    const auto r = static_cast<std::size_t>(row);
    switch (column) {
    case NameCol:
        return QString::fromStdString(rows_[r].name);
    case MainCol:
        return cells_[r].main;
    case ReferenceCol:
        return cells_[r].reference;
    case DeltaCol:
        return cells_[r].delta;
    case UnitCol:
        return cells_[r].unit;
    default:
        return {};
    }
}

} // namespace esses::qt
