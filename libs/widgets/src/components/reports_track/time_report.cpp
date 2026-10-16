// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TimeReport (04 section 13). Owner: src/components/reports_track/.

#include "report_table.hpp"

#include <esses/analysis/time_report_engine.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/report_model.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/components/time_report.hpp>

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QShowEvent>
#include <QTableView>
#include <QVBoxLayout>

namespace esses::widgets {

namespace {

QJsonObject bodyObject(const project::ComponentSpec& spec) {
    if (!spec.body.isObject()) {
        return {};
    }
    return QJsonDocument::fromJson(QByteArray::fromStdString(spec.body.compact())).object();
}

} // namespace

TimeReport::TimeReport(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent)
    : ComponentWidget(spec, context, parent) {
    model_ = new qt::ReportModel(context.render.pool(), context.render.dispatcher(), context.selection,
                                 context.maths, context.render.statCache(), this);
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
    // ALT+n reaches keyPressEvent from the table views, which ignore ALT chords (the event propagates).
    applyBody();
}

TimeReport::~TimeReport() = default;

ComponentWidget::OverlayPolicy TimeReport::overlayPolicy() const {
    return OverlayPolicy::AllOverlays;
}

qt::CommandContext TimeReport::commandContext() const {
    return qt::CommandContext::Component;
}

void TimeReport::applySpec(const project::ComponentSpec& spec) {
    ComponentWidget::applySpec(spec);
    applyBody();
}

void TimeReport::applyBody() {
    analysis::TimeReportSpec report;
    if (spec_.body.isObject()) {
        if (Result<analysis::TimeReportSpec> parsed = analysis::TimeReportSpec::fromJson(spec_.body)) {
            report = std::move(parsed).value();
        }
    }
    const QJsonObject body = bodyObject(spec_);
    model_->setTimeFormat(body.value(QStringLiteral("timeFormat")).toString(QStringLiteral("project")));
    model_->setTimeReport(report, spec_.id.isNil() ? QString() : QString::fromStdString(spec_.id.str()));
    table_->setCaption(spec_.caption.autoText || spec_.caption.text.empty()
                           ? tr("Time Report")
                           : QString::fromStdString(spec_.caption.text),
                       spec_.caption.show);
}

bool TimeReport::toggleBand(int n) {
    QJsonObject body = bodyObject(spec_);
    QJsonArray bands = body.value(QStringLiteral("bands")).toArray();
    if (n < 0 || n >= bands.size()) {
        return false; // bands beyond the list are no-ops (04 section 7.2 item 4)
    }
    QJsonObject band = bands.at(n).toObject();
    const bool visible = band.value(QStringLiteral("visible")).toBool(true);
    band.insert(QStringLiteral("visible"), !visible);
    bands.replace(n, band);
    body.insert(QStringLiteral("bands"), bands);
    Result<JsonDoc> doc = JsonDoc::parse(QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString());
    if (!doc) {
        return false;
    }
    project::ComponentSpec next = spec_;
    next.body = std::move(doc).value();
    Q_EMIT specEditRequested(next, visible ? tr("Hide band %1").arg(n) : tr("Show band %1").arg(n));
    return true;
}

bool TimeReport::handleCommand(const QString& id, const QVariantList& args) {
    if (id == QLatin1String("component.colour_band")) {
        if (!args.isEmpty()) {
            toggleBand(args.front().toInt());
        }
        return true; // a band beyond the list does nothing (04 section 7.2 item 4)
    }
    return ComponentWidget::handleCommand(id, args);
}

void TimeReport::keyPressEvent(QKeyEvent* event) {
    // ALT+n where no command reached the report (no shell): only while the keymap binds it (D-203 (o)).
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    if (mods == Qt::AltModifier && event->key() >= Qt::Key_0 && event->key() <= Qt::Key_9) {
        const int n = event->key() - Qt::Key_0;
        if (keyBound(QStringLiteral("component.colour_band"), *event, {QStringLiteral("Alt+%1").arg(n)})) {
            toggleBand(n);
            event->accept();
            return;
        }
    }
    ComponentWidget::keyPressEvent(event);
}

void TimeReport::onSelectionChanged(const plot::SelectionSnapshot&) {
    requestRefresh();
}

void TimeReport::onDataChanged(const model::SourceId& source, std::uint64_t) {
    if (context_.selection.source(source) != nullptr) {
        requestRefresh();
    }
}

void TimeReport::onChannelDataChanged(const maths::ChannelKey&) {
    // The report shows range times, no channel: a maths output becoming ready changes nothing on it (the
    // ranges and the distance arrive through onDataChanged; D-203 (z)).
}

void TimeReport::requestRefresh() {
    if (detail::deferWhileHidden(*this)) {
        refreshPending_ = true;
        return;
    }
    refreshPending_ = false;
    model_->refresh();
}

void TimeReport::showEvent(QShowEvent* event) {
    ComponentWidget::showEvent(event);
    if (refreshPending_) {
        requestRefresh();
    }
}

void TimeReport::onSchemeChanged(const project::Scheme& scheme) {
    scheme_ = std::make_shared<const project::Scheme>(scheme);
    table_->applyScheme(*scheme_);
    onFocusChanged(isFocusedComponent());
}

void TimeReport::onFocusChanged(bool focused) {
    table_->setFocusFrame(focused, detail::colourOf(scheme_->role("cptFocusBorder", 0x2060FFFFU)));
}

QStringList TimeReport::contextCommands() const {
    return {QStringLiteral("component.properties"), QStringLiteral("component.export_data"),
            QStringLiteral("component.print_clipboard")};
}

bool TimeReport::exportData(ExportSink& sink, const ExportOptions&) const {
    return sink.write(model_->toCsv().toUtf8());
}

QImage TimeReport::renderToImage(qreal dpr) const {
    table_->settleLayout();
    return ComponentWidget::renderToImage(dpr); // QWidget::render of the table (04 section 7.2 item 9)
}

QTableView* TimeReport::tableView() const {
    return table_->bodyView();
}

QTableView* TimeReport::frozenView() const {
    return table_->frozenView();
}

QString TimeReport::toTsv() const {
    return model_->toTsv();
}

void TimeReport::copyTsv() const {
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(model_->toTsv());
    }
}

} // namespace esses::widgets
