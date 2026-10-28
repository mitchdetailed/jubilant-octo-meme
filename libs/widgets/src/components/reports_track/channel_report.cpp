// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelReport (04 section 12). Owner: src/components/reports_track/.

#include "report_table.hpp"

#include <esses/analysis/report_spec.hpp>
#include <esses/base/casefold.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/report_model.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/components/channel_report.hpp>

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QScopeGuard>
#include <QShowEvent>
#include <QTableView>
#include <QVBoxLayout>

namespace esses::widgets {

namespace {

constexpr std::uint32_t kMinHighlight = 0x2060FFFFU; // 04 section 12 item 8
constexpr std::uint32_t kMaxHighlight = 0xFF3030FFU;

QJsonObject bodyObject(const project::ComponentSpec& spec) {
    if (!spec.body.isObject()) {
        return {};
    }
    return QJsonDocument::fromJson(QByteArray::fromStdString(spec.body.compact())).object();
}

std::optional<JsonDoc> docOf(const QJsonObject& o) {
    Result<JsonDoc> doc = JsonDoc::parse(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString());
    if (!doc) {
        return std::nullopt;
    }
    return std::move(doc).value();
}

std::optional<std::uint32_t> highlightOf(const QJsonObject& highlight, const char* key,
                                         std::uint32_t fallback) {
    const QJsonObject h = highlight.value(QLatin1String(key)).toObject();
    if (!h.value(QStringLiteral("on")).toBool(true)) {
        return std::nullopt;
    }
    return detail::parseBodyColour(h.value(QStringLiteral("colour")).toString(), fallback);
}

} // namespace

ChannelReport::ChannelReport(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent)
    : ComponentWidget(spec, context, parent) {
    model_ = new qt::ReportModel(context.render.pool(), context.render.dispatcher(), context.selection,
                                 context.maths, context.render.statCache(), this);
    model_->setDistanceService(&context.view.distanceService());
    table_ = new detail::ReportTable(model_, this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(table_);
    setFocusProxy(table_);
    scheme_ = context.scheme != nullptr ? context.scheme : std::make_shared<const project::Scheme>();
    table_->applyScheme(*scheme_);
    table_->bodyView()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_->bodyView(), &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        // On the heap and asynchronous: the component may go while the menu is open (a document rebuild, a
        // closed source); the menu then goes with it and nothing runs on a destroyed object (D-203 (q)).
        auto* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addAction(tr("Copy as TSV"), this, [this] { copyTsv(); });
        menu->popup(table_->bodyView()->viewport()->mapToGlobal(pos));
    });
    applyBody();
}

ChannelReport::~ChannelReport() = default;

ComponentWidget::OverlayPolicy ChannelReport::overlayPolicy() const {
    return OverlayPolicy::AllOverlays;
}

qt::CommandContext ChannelReport::commandContext() const {
    return qt::CommandContext::Component;
}

void ChannelReport::applySpec(const project::ComponentSpec& spec) {
    ComponentWidget::applySpec(spec);
    applyBody();
}

void ChannelReport::applyBody() {
    const QJsonObject body = bodyObject(spec_);
    analysis::ReportSpec report;
    if (const std::optional<JsonDoc> doc = docOf(body.value(QStringLiteral("report")).toObject())) {
        if (Result<analysis::ReportSpec> parsed = analysis::ReportSpec::fromJson(*doc)) {
            report = std::move(parsed).value();
        }
    }
    report.layout = body.value(QStringLiteral("layout")).toString() == QLatin1String("channelsInColumns")
                        ? analysis::ReportLayout::ChannelsInColumns
                        : analysis::ReportLayout::ChannelsInRows;
    report.deltaVsOverlay0 = body.value(QStringLiteral("deltaVsOverlay0")).toBool(false);
    const QJsonObject highlight = body.value(QStringLiteral("highlight")).toObject();
    qt::ReportModel::Highlight h;
    h.min = highlightOf(highlight, "min", kMinHighlight);
    h.max = highlightOf(highlight, "max", kMaxHighlight);
    model_->setHighlight(h);
    model_->setTimeFormat(body.value(QStringLiteral("timeFormat")).toString(QStringLiteral("project")));
    zoomLinked_ = report.options.zoomLinked;
    model_->setZoom(zoomLinked_ ? std::optional<plot::ViewState>(viewState()) : std::nullopt);
    model_->setChannelReport(report, spec_.id.isNil() ? QString() : QString::fromStdString(spec_.id.str()));
    table_->setCaption(spec_.caption.autoText || spec_.caption.text.empty()
                           ? tr("Channel Report")
                           : QString::fromStdString(spec_.caption.text),
                       spec_.caption.show);
}

void ChannelReport::onViewChanged(const plot::ViewState& view, plot::ChangeMask what) {
    if (!zoomLinked_) {
        return; // the component ignores links otherwise (04 section 12 item 4)
    }
    const auto zoomBits =
        static_cast<plot::ChangeMask>(plot::mask(plot::Change::Zoom) | plot::mask(plot::Change::Domain));
    if ((what & zoomBits) != 0) {
        model_->setZoom(view);
        requestRefresh();
    }
}

void ChannelReport::onSelectionChanged(const plot::SelectionSnapshot&) {
    requestRefresh();
}

void ChannelReport::onDataChanged(const model::SourceId& source, std::uint64_t) {
    if (context_.selection.source(source) != nullptr) {
        requestRefresh();
    }
}

void ChannelReport::onChannelDataChanged(const maths::ChannelKey& key) {
    // Only the channels the report lists (a log open readies dozens of maths outputs; D-203 (z)).
    for (const QString& name : channelNames()) {
        if (caseFold(name.toStdString()) == key.nameCf) {
            onDataChanged(key.src, 0);
            return;
        }
    }
}

void ChannelReport::requestRefresh() {
    if (detail::deferWhileHidden(*this)) {
        refreshPending_ = true;
        return;
    }
    refreshPending_ = false;
    model_->refresh();
}

void ChannelReport::showEvent(QShowEvent* event) {
    ComponentWidget::showEvent(event);
    if (refreshPending_) {
        requestRefresh();
    }
}

void ChannelReport::onSchemeChanged(const project::Scheme& scheme) {
    scheme_ = std::make_shared<const project::Scheme>(scheme);
    table_->applyScheme(*scheme_);
    onFocusChanged(isFocusedComponent());
}

void ChannelReport::onFocusChanged(bool focused) {
    table_->setFocusFrame(focused, detail::colourOf(scheme_->role("cptFocusBorder", 0x2060FFFFU)));
}

QStringList ChannelReport::channelNames() const {
    QStringList names;
    const QJsonArray channels = bodyObject(spec_)
                                    .value(QStringLiteral("report"))
                                    .toObject()
                                    .value(QStringLiteral("channels"))
                                    .toArray();
    for (const QJsonValue& c : channels) {
        const QString name = c.toObject().value(QStringLiteral("name")).toString();
        if (!name.isEmpty() && !names.contains(name)) {
            names << name;
        }
    }
    return names;
}

void ChannelReport::addChannel(const QString& name, bool) {
    if (name.isEmpty() || channelNames().contains(name)) {
        return;
    }
    QJsonObject body = bodyObject(spec_);
    QJsonObject report = body.value(QStringLiteral("report")).toObject();
    QJsonArray channels = report.value(QStringLiteral("channels")).toArray();
    channels.append(
        QJsonObject{{QStringLiteral("name"), name},
                    {QStringLiteral("stats"),
                     QJsonArray{QStringLiteral("avg"), QStringLiteral("min"), QStringLiteral("max")}}});
    report.insert(QStringLiteral("channels"), channels);
    body.insert(QStringLiteral("report"), report);
    const std::optional<JsonDoc> doc = docOf(body);
    if (!doc) {
        return;
    }
    project::ComponentSpec next = spec_;
    next.body = *doc;
    Q_EMIT specEditRequested(next, tr("Add %1").arg(name));
}

QStringList ChannelReport::contextCommands() const {
    return {QStringLiteral("component.properties"), QStringLiteral("component.export_data"),
            QStringLiteral("component.print_clipboard")};
}

bool ChannelReport::exportData(ExportSink& sink, const ExportOptions&) const {
    // The table as shown (its scope and zoom link decide the ranges; 04 section 12 item 7).
    return sink.write(model_->toCsv().toUtf8());
}

QImage ChannelReport::renderToImage(qreal dpr) const {
    table_->settleLayout();
    table_->setExporting(true); // the picture does not depend on the focus (report_table.hpp)
    const auto shown = qScopeGuard([this] { table_->setExporting(false); });
    return ComponentWidget::renderToImage(dpr); // QWidget::render of the table (04 section 7.2 item 9)
}

QTableView* ChannelReport::tableView() const {
    return table_->bodyView();
}

QTableView* ChannelReport::frozenView() const {
    return table_->frozenView();
}

QString ChannelReport::toTsv() const {
    return model_->toTsv();
}

void ChannelReport::copyTsv() const {
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(model_->toTsv());
    }
}

} // namespace esses::widgets
