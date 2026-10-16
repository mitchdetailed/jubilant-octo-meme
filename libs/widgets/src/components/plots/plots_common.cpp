// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Internal helpers of src/components/plots/ (plots_common.hpp).

#include "plots_common.hpp"

#include <esses/maths/channel_key.hpp>
#include <esses/model/resolver.hpp>
#include <esses/plotcore/domain_mapper.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/store/resampler.hpp>

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <variant>

namespace esses::widgets::plots {

namespace {

constexpr Micros kExportPadUs = 2'000'000; // "Selected ranges padded by 2 s" (04 section 7.2 item 8)

} // namespace

int bandArg(const QVariantList& args) {
    if (args.isEmpty()) {
        return -1;
    }
    bool ok = false;
    const int n = args.front().toInt(&ok);
    return ok ? n : -1;
}

std::optional<QByteArray> runOffGui(WorksheetContext& context, const QString& key,
                                    std::function<std::optional<QByteArray>(const CancelToken&)> work) {
    struct Outcome {
        bool done = false;
        std::optional<QByteArray> bytes;
    };
    auto outcome = std::make_shared<Outcome>();
    auto produced = std::make_shared<std::optional<QByteArray>>();
    QEventLoop loop;
    QObject guard; // the posting context: alive for the whole wait
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);
    context.render.submitBuild(
        context.worksheetId, key, Priority::Visible,
        [work = std::move(work), produced](const CancelToken& cancel) -> Result<plot::ComponentBuild> {
            *produced = work(cancel);
            return plot::ComponentBuild{}; // never Cancelled: the result is always delivered
        },
        &guard,
        [outcome, produced, &loop](Result<plot::ComponentBuild>) {
            outcome->done = true;
            outcome->bytes = std::move(*produced);
            loop.quit();
        });
    if (!outcome->done) {
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    return outcome->done ? std::move(outcome->bytes) : std::nullopt;
}

plot::ComponentBuildInputs exportInputs(plot::ComponentBuildInputs inputs, ExportOptions::Range range) {
    if (inputs.slots.empty() || range == ExportOptions::Range::CurrentZoom) {
        return inputs;
    }
    const plot::SlotData& main = inputs.slots.front();
    plot::RangeGeom geom = main.range;
    geom.offsetUs = 0;
    geom.offsetM = 0.0;
    const plot::DomainMapper map(inputs.view.domain, geom, main.outing, main.chain, Rate{}, 0);
    if (range == ExportOptions::Range::AllData) {
        inputs.view.zoom = map.outingExtent();
    } else { // SelectedPadded
        const double a = map.xOf(std::max(main.outing.start, main.range.start - kExportPadUs));
        const double b = map.xOf(std::min(main.outing.end, main.range.end + kExportPadUs));
        inputs.view.zoom = plot::XSpan{std::min(a, b), std::max(a, b)};
    }
    return inputs;
}

QByteArray csvNumber(double v) {
    if (!std::isfinite(v)) {
        return {};
    }
    return QByteArray::number(v, 'g', 12);
}

QByteArray csvText(const QString& text) {
    QByteArray out = text.toUtf8();
    out.replace("\"", "\"\"");
    return '"' + out + '"';
}

QString slotLabel(const plot::SlotData& slot) {
    switch (slot.slot.kind) {
    case plot::SlotKind::Main:
        return QStringLiteral("Main");
    case plot::SlotKind::Overlay0:
        return QStringLiteral("Overlay 0");
    case plot::SlotKind::Overlay:
        break;
    }
    return QStringLiteral("Overlay %1").arg(slot.slot.number);
}

std::optional<units::QuantityId> quantityOf(WorksheetContext& context, const std::string& channel) {
    if (!context.selection.hasMain()) {
        return std::nullopt;
    }
    const plot::SelectionSnapshot& s = context.selection.snapshot();
    const auto src = context.selection.source(s.main.src);
    if (src == nullptr) {
        return std::nullopt;
    }
    const std::string layer(model::canonicalLayerName(s.main.layer));
    if (context.maths.isMathsChannel(src->id, layer, channel)) {
        if (const auto d = context.maths.descriptor(maths::ChannelKey::make(src->id, layer, channel))) {
            return d->quantity;
        }
        return std::nullopt;
    }
    const model::DataLayer* l = src->layer(layer);
    if (l == nullptr) {
        return std::nullopt;
    }
    const model::ChannelResolver resolver({}, {}, context.maths.units().get());
    const model::ChannelResolver::Resolution r = resolver.resolve(channel, *l, src->id);
    if (r.channel == nullptr) {
        return std::nullopt;
    }
    return r.alias ? r.alias->quantity : r.channel->desc.quantity;
}

int wheelNotches(int& remainder, int angleDelta) {
    if (angleDelta == 0) {
        return 0;
    }
    if ((angleDelta > 0) != (remainder > 0) && remainder != 0) {
        remainder = 0;
    }
    remainder += angleDelta;
    const int notches = remainder / 120;
    remainder -= notches * 120;
    return notches;
}

void zoomAboutCursor(WorksheetContext& context, const plot::ComponentBuildInputs& inputs, double factor) {
    const plot::ViewState& view = context.view.state();
    double c = static_cast<double>(view.cursorT);
    if (!inputs.slots.empty()) {
        const plot::SlotData& main = inputs.slots.front();
        plot::RangeGeom geom = main.range;
        geom.offsetUs = 0;
        geom.offsetM = 0.0;
        const plot::DomainMapper map(view.domain, geom, main.outing, main.chain, Rate{}, 0);
        if (view.domain == plot::Domain::Time || map.hasDistance()) {
            c = map.xOf(main.range.start + view.cursorT);
        }
    }
    const plot::XSpan z = view.zoom;
    c = std::clamp(c, std::min(z.a, z.b), std::max(z.a, z.b));
    context.view.zoomTo(plot::XSpan{c - (c - z.a) * factor, c + (z.b - c) * factor});
}

std::optional<QByteArray> timeSeriesCsv(const plot::ComponentBuildInputs& inputs,
                                        const std::vector<std::size_t>& channels, const CancelToken& cancel) {
    if (inputs.slots.empty()) {
        return std::nullopt;
    }
    const plot::SlotData& main = inputs.slots.front();
    plot::RangeGeom geom = main.range;
    geom.offsetUs = 0;
    geom.offsetM = 0.0;
    const plot::DomainMapper map(inputs.view.domain, geom, main.outing, main.chain, Rate{}, 0);
    if (inputs.view.domain == plot::Domain::Distance && !map.hasDistance()) {
        return std::nullopt;
    }
    const Micros t0 = std::max(main.outing.start, map.timeAt(inputs.view.zoom.a));
    const Micros t1 = std::min(main.outing.end, map.timeAt(inputs.view.zoom.b));
    std::vector<const plot::ResolvedChannel*> cols;
    const store::UniformSeries* base = nullptr;
    const plot::ResolvedChannel* baseRc = nullptr;
    for (const std::size_t i : channels) {
        if (i >= main.channels.size()) {
            continue;
        }
        const plot::ResolvedChannel& rc = main.channels[i];
        if (!rc.descriptor || !rc.series || rc.status == model::ChannelStatus::Pending) {
            continue;
        }
        cols.push_back(&rc);
        if (base == nullptr) {
            base = std::get_if<store::UniformSeries>(&*rc.series);
            baseRc = base != nullptr ? &rc : nullptr;
        }
    }
    if (cols.empty() || base == nullptr || baseRc == nullptr || !base->rate.valid() || t1 <= t0) {
        return std::nullopt;
    }
    // The sample indexes of a uniform series within [from, to), clamped to the series.
    const auto indexes = [](const store::UniformSeries& u, SampleIdx size, Micros from, Micros to) {
        const SampleIdx a = from <= u.t0 ? 0 : u.rate.indexAt(from - 1, u.t0) + 1;
        const SampleIdx b = to <= u.t0 ? 0 : u.rate.indexAt(to - 1, u.t0) + 1;
        return std::pair<SampleIdx, SampleIdx>{std::clamp<SampleIdx>(a, 0, size),
                                               std::clamp<SampleIdx>(b, 0, size)};
    };
    const auto [i0, i1] = indexes(*base, store::seriesSize(*baseRc->series), t0, t1);
    if (i1 <= i0) {
        return std::nullopt;
    }
    // Pin what the rows read, so Resampler::sampleAt finds every chunk resident.
    std::vector<store::ChunkHandle> pins;
    for (const plot::ResolvedChannel* rc : cols) {
        const store::Column& values = store::seriesValues(*rc->series);
        const SampleIdx size = values.size();
        if (size == 0) {
            continue;
        }
        SampleIdx first = 0;
        SampleIdx last = size - 1;
        if (const auto* u = std::get_if<store::UniformSeries>(&*rc->series);
            u != nullptr && u->rate.valid()) {
            const auto [a, b] = indexes(*u, size, t0, t1);
            first = std::max<SampleIdx>(0, a - 1); // the neighbours interpolation reads
            last = std::min<SampleIdx>(size - 1, b);
        }
        Result<store::ChunkHandle> pin = values.require(first, last, Priority::Background, cancel);
        if (!pin) {
            return std::nullopt;
        }
        pins.push_back(std::move(*pin));
    }
    QByteArray out = "Time [s]";
    for (const plot::ResolvedChannel* rc : cols) {
        out += ',';
        out += csvText(QString::fromStdString(
            rc->displayUnit.empty() ? rc->name : rc->name + " [" + rc->displayUnit + "]"));
    }
    out += '\n';
    for (SampleIdx i = i0; i < i1; ++i) {
        if ((i - i0) % 4096 == 0 && cancel.checkpoint()) {
            return std::nullopt;
        }
        const Micros t = base->rate.timeAt(i, base->t0);
        out += csvNumber(static_cast<double>(t) / 1e6);
        for (const plot::ResolvedChannel* rc : cols) {
            const store::ValueSample v = store::Resampler::sampleAt(*rc->series, t, rc->descriptor->interp);
            out += ',';
            out += v.state == store::SampleState::Valid ? csvNumber(rc->toDisplay(v.value)) : QByteArray();
        }
        out += '\n';
    }
    return out;
}

} // namespace esses::widgets::plots
