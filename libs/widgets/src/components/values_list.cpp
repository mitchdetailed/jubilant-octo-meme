// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ValuesList (04 section 15): the body (channels, columns, heading, widths) applied to a QTableView over a
// ValuesModel, and addChannel.

#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/values_model.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/qtrender/scheme_fonts.hpp>
#include <esses/widgets/components/values_list.hpp>

#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

ValuesList::ValuesList(const project::ComponentSpec& spec, WorksheetContext& context, QWidget* parent)
    : ComponentWidget(spec, context, parent) {
    model_ = new qt::ValuesModel(context.selection, context.maths, this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setTabKeyNavigation(false); // TAB cycles the worksheet's components (04 section 7.2 item 1)
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->setWordWrap(false);
    setFocusProxy(table_);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(table_);
    model_->setWorksheet(&context.view);
    if (context.scheme != nullptr) {
        applyFonts(*context.scheme);
    }
    applyBody();
}

ValuesList::~ValuesList() = default;

void ValuesList::applySpec(const project::ComponentSpec& spec) {
    ComponentWidget::applySpec(spec);
    applyBody();
}

void ValuesList::applyBody() {
    // Rows (04 section 15 item 5): the same parsing as RenderService and the Values window.
    model_->setChannelRequests(qt::componentChannels(spec_));
    const QJsonObject body = bodyObject(spec_);
    const QJsonObject columns = body.value(QStringLiteral("columns")).toObject();
    table_->setColumnHidden(qt::ValuesModel::DeltaCol, !columns.value(QStringLiteral("delta")).toBool(true));
    table_->setColumnHidden(qt::ValuesModel::UnitCol,
                            !columns.value(QStringLiteral("inlineUnits")).toBool(true));
    table_->horizontalHeader()->setVisible(body.value(QStringLiteral("showHeading")).toBool(true));
    QHeaderView* header = table_->horizontalHeader();
    if (body.value(QStringLiteral("autoSizeColumns")).toBool(true)) {
        header->setSectionResizeMode(QHeaderView::ResizeToContents);
    } else {
        header->setSectionResizeMode(QHeaderView::Interactive);
        const QJsonObject widths = body.value(QStringLiteral("widths")).toObject();
        const auto width = [&widths](const char* key, int fallback) {
            return widths.value(QLatin1String(key)).toInt(fallback);
        };
        table_->setColumnWidth(qt::ValuesModel::NameCol, width("name", 120));
        table_->setColumnWidth(qt::ValuesModel::MainCol, width("value", 80));
        table_->setColumnWidth(qt::ValuesModel::ReferenceCol, width("value", 80));
        table_->setColumnWidth(qt::ValuesModel::DeltaCol, width("delta", 80));
    }
}

void ValuesList::applyFonts(const project::Scheme& scheme) {
    const qt::SchemeFonts fonts(scheme);
    table_->setFont(fonts.role(QStringLiteral("valuesItem")));
    table_->horizontalHeader()->setFont(fonts.role(QStringLiteral("valuesHeader")));
}

void ValuesList::onViewChanged(const plot::ViewState&, plot::ChangeMask) {
    // The ValuesModel follows the WorksheetViewModel itself (ValuesModel::setWorksheet).
}

void ValuesList::onSchemeChanged(const project::Scheme& scheme) {
    applyFonts(scheme);
}

QStringList ValuesList::channelNames() const {
    return model_->channels();
}

void ValuesList::addChannel(const QString& name, bool) {
    if (name.isEmpty() || model_->channels().contains(name)) {
        return;
    }
    QJsonObject body = bodyObject(spec_);
    QJsonArray channels = body.value(QStringLiteral("channels")).toArray();
    channels.append(QJsonObject{{QStringLiteral("channel"), name}});
    body.insert(QStringLiteral("channels"), channels);
    Result<model::OpaqueJson> json =
        model::OpaqueJson::parse(QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString());
    if (!json) {
        return;
    }
    project::ComponentSpec next = spec_;
    next.body = std::move(json).value();
    Q_EMIT specEditRequested(next, tr("Add %1").arg(name));
}

QImage ValuesList::renderToImage(qreal dpr) const {
    return ComponentWidget::renderToImage(dpr); // QWidget::render of the table (04 section 7.2 item 9)
}

} // namespace esses::widgets
