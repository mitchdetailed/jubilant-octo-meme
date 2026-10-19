// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Internal helpers of src/components/plots/ (plots_common.hpp).

#include "plots_common.hpp"

#include <esses/base/read_file.hpp>
#include <esses/base/uuid.hpp>
#include <esses/formats/csv.hpp>
#include <esses/formats/importer.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/resolver.hpp>
#include <esses/plotcore/domain_mapper.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/store/resampler.hpp>

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <set>
#include <system_error>
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

void zoomAboutCursor(ComponentWidget& component, const plot::ComponentBuildInputs& inputs, double factor) {
    const plot::ViewState view = component.viewState();
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
    component.setViewZoom(plot::XSpan{c - (c - z.a) * factor, c + (z.b - c) * factor});
}

std::optional<QByteArray> legacyCsv(const plot::ComponentBuildInputs& inputs,
                                    const std::vector<std::size_t>& channels, const LegacyExportInfo& info,
                                    const CancelToken& cancel) {
    if (inputs.slots.empty() || inputs.slots.front().source == nullptr) {
        return std::nullopt;
    }
    const plot::SlotData& main = inputs.slots.front();
    const model::DataSource& from = *main.source;
    const model::DataLayer* fromLayer = from.layer(main.layer);
    if (fromLayer == nullptr) {
        return std::nullopt;
    }
    plot::RangeGeom geom = main.range;
    geom.offsetUs = 0;
    geom.offsetM = 0.0;
    const plot::DomainMapper map(inputs.view.domain, geom, main.outing, main.chain, Rate{}, 0);
    if (inputs.view.domain == plot::Domain::Distance && !map.hasDistance()) {
        return std::nullopt;
    }
    const Micros t0 = std::max(main.outing.start, map.timeAt(inputs.view.zoom.a));
    const Micros t1 = std::min(main.outing.end, map.timeAt(inputs.view.zoom.b));
    if (t1 <= t0) {
        return std::nullopt;
    }

    // A source of its own for the writer: the Main source's details and one layer holding the component's
    // channels under the names the component gives them (maths outputs included), each with a fresh id.
    auto src = std::make_shared<model::DataSource>();
    src->id = from.id;
    src->prov = from.prov;
    src->details = from.details;
    std::string rangeName = main.slot.range.name;
    if (const model::RangeGroup* group = fromLayer->ranges.find(main.slot.range.groupPath)) {
        for (const model::Range& r : group->ranges) {
            if (r.id == main.slot.range.range) {
                rangeName = r.name;
            }
        }
    }
    const auto setDetail = [&src](const char* id, const std::string& text) {
        std::erase_if(src->details, [id](const model::Detail& d) { return d.id == id; });
        if (!text.empty()) {
            model::Detail d;
            d.id = id;
            d.value = text;
            src->details.push_back(std::move(d));
        }
    };
    setDetail("Export Range", rangeName);
    setDetail("Export Worksheet", info.worksheet);
    setDetail("Export Workbook", std::string());
    model::DataLayer layer;
    layer.name = std::string(model::kNormalLayer);
    layer.origin = fromLayer->origin;
    layer.duration = fromLayer->duration;
    formats::ExportView view;
    QJsonObject units;
    std::set<std::string> names;
    for (const std::size_t i : channels) {
        if (i >= main.channels.size()) {
            continue;
        }
        const plot::ResolvedChannel& rc = main.channels[i];
        if (!rc.descriptor || !rc.series || rc.status == model::ChannelStatus::Pending ||
            !names.insert(rc.name).second) {
            continue;
        }
        model::Channel ch;
        ch.desc = *rc.descriptor;
        ch.desc.name = rc.name;
        ch.desc.id = model::ChannelId{uuidRandom()};
        ch.series = *rc.series;
        if (inputs.units != nullptr && inputs.units->contains(rc.displayUnitId) &&
            rc.displayUnitId != rc.descriptor->unit) {
            units.insert(QString::fromStdString(rc.name),
                         QString::fromStdString(inputs.units->unit(rc.displayUnitId).symbol));
        }
        view.channels.push_back(ch.desc.id);
        layer.addChannel(std::move(ch));
    }
    if (view.channels.empty()) {
        return std::nullopt;
    }
    src->layers.push_back(std::move(layer));
    view.source = src;
    view.layer = std::string(model::kNormalLayer);
    view.start = t0;
    view.end = t1;
    formats::WriterOptions options;
    if (!units.isEmpty()) {
        const QJsonObject o{{QStringLiteral("csv"), QJsonObject{{QStringLiteral("units"), units}}}};
        Result<JsonDoc> doc = JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString());
        if (!doc) {
            return std::nullopt;
        }
        options.options = std::move(doc).value();
    }

    // The writer writes a file (atomically); its bytes are read back and the file removed.
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return std::nullopt;
    }
    const std::filesystem::path path = dir / ("esses-export-" + uuidRandom().str() + ".csv");
    const std::unique_ptr<formats::IWriter> writer = formats::makeLegacyCsvWriter(inputs.units.get());
    const Result<formats::WriteReport> written = writer->write(path, view, options, {}, cancel);
    if (!written) {
        std::filesystem::remove(path, ec);
        return std::nullopt;
    }
    Result<std::string> bytes = readWholeFile(path, "the exported CSV");
    std::filesystem::remove(path, ec);
    if (!bytes) {
        return std::nullopt;
    }
    return QByteArray::fromStdString(*bytes);
}

} // namespace esses::widgets::plots
