// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The report components' table (report_table.hpp). Owner: src/components/reports_track/.

#include "report_table.hpp"

#include <esses/qtmodel/report_model.hpp>
#include <esses/qtrender/scheme_fonts.hpp>

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QScrollBar>
#include <QTableView>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <optional>

namespace esses::widgets::detail {

namespace {

constexpr int kFrameLogical = 2; // the focus border (04 section 7.2 item 1)

void setupView(QTableView* view) {
    view->setTabKeyNavigation(false); // TAB cycles the worksheet's components (04 section 7.2 item 1)
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setSelectionMode(QAbstractItemView::ContiguousSelection);
    view->setWordWrap(false);
    view->setCornerButtonEnabled(false);
    view->verticalHeader()->setVisible(false);
    view->horizontalHeader()->setHighlightSections(false);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
}

} // namespace

std::uint32_t parseBodyColour(const QString& text, std::uint32_t fallback) {
    if (!text.startsWith(QLatin1Char('#')) || (text.size() != 7 && text.size() != 9)) {
        return fallback;
    }
    bool ok = false;
    const std::uint32_t v = text.mid(1).toUInt(&ok, 16);
    if (!ok) {
        return fallback;
    }
    return text.size() == 7 ? ((v << 8) | 0xFFU) : v;
}

QColor colourOf(std::uint32_t rgba) {
    return QColor::fromRgb(static_cast<int>((rgba >> 24) & 0xFFU), static_cast<int>((rgba >> 16) & 0xFFU),
                           static_cast<int>((rgba >> 8) & 0xFFU), static_cast<int>(rgba & 0xFFU));
}

// ---- ReportStyleProxy -----------------------------------------------------------------------------------

ReportStyleProxy::ReportStyleProxy(QObject* parent) : QIdentityProxyModel(parent) {}

void ReportStyleProxy::setBaseFont(const QFont& font) {
    font_ = font;
    if (rowCount() > 0 && columnCount() > 0) {
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::FontRole});
    }
}

void ReportStyleProxy::setTableBackground(QColor colour) {
    background_ = colour;
}

QColor ReportStyleProxy::tint(std::uint32_t rgba, QColor background) {
    const QColor c = colourOf(rgba);
    const double a = 0.45;
    const auto mix = [a](int fg, int bg) {
        return std::clamp(static_cast<int>(std::lround(a * fg + (1.0 - a) * bg)), 0, 255);
    };
    return QColor::fromRgb(mix(c.red(), background.red()), mix(c.green(), background.green()),
                           mix(c.blue(), background.blue()));
}

QVariant ReportStyleProxy::data(const QModelIndex& index, int role) const {
    switch (role) {
    case Qt::FontRole: {
        QFont f = font_;
        f.setItalic(QIdentityProxyModel::data(index, qt::ReportModel::ItalicRole).toBool());
        f.setBold(QIdentityProxyModel::data(index, qt::ReportModel::BoldRole).toBool() ||
                  QIdentityProxyModel::data(index, qt::ReportModel::RowKindRole).toInt() != 0);
        return f;
    }
    case Qt::BackgroundRole: {
        const QVariant rgba = QIdentityProxyModel::data(index, qt::ReportModel::BackgroundRgbaRole);
        if (!rgba.isValid()) {
            return {};
        }
        return tint(rgba.toUInt(), background_);
    }
    default:
        return QIdentityProxyModel::data(index, role);
    }
}

// ---- ReportTable ----------------------------------------------------------------------------------------

ReportTable::ReportTable(QAbstractItemModel* model, QWidget* parent)
    : QWidget(parent), proxy_(new ReportStyleProxy(this)), caption_(new QLabel(this)),
      frozen_(new QTableView(this)), body_(new QTableView(this)) {
    proxy_->setSourceModel(model);
    for (QTableView* v : {frozen_, body_}) {
        v->setModel(proxy_);
        setupView(v);
    }
    frozen_->setSelectionModel(body_->selectionModel());
    frozen_->setObjectName(QStringLiteral("reportFrozen"));
    body_->setObjectName(QStringLiteral("reportBody"));
    frozen_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    frozen_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    frozen_->setFocusPolicy(Qt::NoFocus);
    frozen_->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    frozen_->horizontalHeader()->setStretchLastSection(false);
    body_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    connect(frozen_->verticalScrollBar(), &QScrollBar::valueChanged, body_->verticalScrollBar(),
            &QScrollBar::setValue);
    connect(body_->verticalScrollBar(), &QScrollBar::valueChanged, frozen_->verticalScrollBar(),
            &QScrollBar::setValue);
    connect(proxy_, &QAbstractItemModel::modelReset, this, &ReportTable::relayoutColumns);
    connect(proxy_, &QAbstractItemModel::layoutChanged, this, &ReportTable::relayoutColumns);

    caption_->setObjectName(QStringLiteral("reportCaption"));
    caption_->setContentsMargins(4, 1, 4, 1);
    caption_->setAutoFillBackground(true);
    auto* tables = new QHBoxLayout;
    tables->setContentsMargins(0, 0, 0, 0);
    tables->setSpacing(0);
    tables->addWidget(frozen_);
    tables->addWidget(body_, 1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kFrameLogical, kFrameLogical, kFrameLogical, kFrameLogical);
    layout->setSpacing(0);
    layout->addWidget(caption_);
    layout->addLayout(tables, 1);
    setFocusProxy(body_);
    relayoutColumns();
}

ReportTable::~ReportTable() = default;

void ReportTable::relayoutColumns() {
    const int columns = proxy_->columnCount();
    for (int c = 0; c < columns; ++c) {
        frozen_->setColumnHidden(c, c != 0);
        body_->setColumnHidden(c, c == 0);
    }
    if (columns == 0) {
        frozen_->setFixedWidth(0);
        return;
    }
    // Widths from the cells' text and fonts (not QTableView::sizeHintForColumn, which depends on whether the
    // view has been laid out yet): the same table always gets the same columns.
    for (int c = 1; c < columns; ++c) {
        body_->setColumnWidth(c, std::min(contentWidth(c), 260));
    }
    const int width = std::min(contentWidth(0), 320);
    frozen_->setColumnWidth(0, width);
    frozen_->setFixedWidth(width + 2 * frozen_->frameWidth());
    settleLayout();
}

void ReportTable::settleLayout() {
    // The new frozen width takes effect now, not at the next LayoutRequest: a Print to Clipboard right after
    // a delivery paints the table as it will be shown.
    if (QLayout* l = layout()) {
        l->activate();
    }
}

int ReportTable::contentWidth(int column) const {
    constexpr int kCellPadding = 12;   // the item's text margins and the grid line
    constexpr int kHeaderPadding = 16; // the section's margins
    const QFontMetrics header(body_->horizontalHeader()->font());
    int width =
        header.horizontalAdvance(proxy_->headerData(column, Qt::Horizontal).toString()) + kHeaderPadding;
    // One QFontMetrics per run of cells in the same font, and each (font, text) advance cached across
    // deliveries: every channelReady or zoom step of a large report re-measures from the cache (D-203 (aa)).
    constexpr qsizetype kMaxCachedWidths = 65536;
    std::optional<QFontMetrics> fm;
    QFont current;
    QString fontKey;
    for (int r = 0; r < proxy_->rowCount(); ++r) {
        const QModelIndex i = proxy_->index(r, column);
        const QFont font = i.data(Qt::FontRole).value<QFont>();
        if (!fm || font != current) {
            current = font;
            fm.emplace(font);
            fontKey = font.key() + QChar(0x1F);
        }
        const QString text = i.data(Qt::DisplayRole).toString();
        const QString key = fontKey + text;
        int advance = 0;
        if (const auto it = widthCache_.constFind(key); it != widthCache_.cend()) {
            advance = it.value();
        } else {
            advance = fm->horizontalAdvance(text);
            if (widthCache_.size() >= kMaxCachedWidths) {
                widthCache_.clear();
            }
            widthCache_.insert(key, advance);
        }
        width = std::max(width, advance + kCellPadding);
    }
    return width;
}

bool deferWhileHidden(const QWidget& report) {
    const QWidget* top = report.window();
    return !report.isVisible() && top != &report && top->isVisible();
}

void ReportTable::applyScheme(const project::Scheme& scheme) {
    widthCache_.clear(); // the fonts may change
    const qt::SchemeFonts fonts(scheme);
    const QFont table = fonts.role(QStringLiteral("reportTable"));
    const QFont heading = fonts.role(QStringLiteral("reportHeading"));
    QPalette pal = palette();
    const QColor background = colourOf(scheme.role("dataBackground", 0xFFFFFFFFU));
    pal.setColor(QPalette::Base, background);
    pal.setColor(QPalette::AlternateBase, background);
    pal.setColor(QPalette::Text, colourOf(scheme.role("dataForeground", 0x202020FFU)));
    pal.setColor(QPalette::Window, colourOf(scheme.role("cptBackground", 0xFFFFFFFFU)));
    pal.setColor(QPalette::Button, colourOf(scheme.role("dataHeadingBackground", 0xE8E8E8FFU)));
    pal.setColor(QPalette::ButtonText, colourOf(scheme.role("dataHeadingForeground", 0x202020FFU)));
    pal.setColor(QPalette::Highlight, colourOf(scheme.role("dataHighlightBackground", 0x3080FFFFU)));
    pal.setColor(QPalette::HighlightedText, colourOf(scheme.role("dataHighlightForeground", 0xFFFFFFFFU)));
    for (QTableView* v : {frozen_, body_}) {
        v->setPalette(pal);
        v->setFont(table);
        v->horizontalHeader()->setFont(heading);
        v->setGridStyle(Qt::SolidLine);
    }
    QPalette cap = caption_->palette();
    cap.setColor(QPalette::Window, colourOf(scheme.role("cptCaptionBackground", 0xE8E8E8FFU)));
    cap.setColor(QPalette::WindowText, colourOf(scheme.role("cptCaptionForeground", 0x202020FFU)));
    caption_->setPalette(cap);
    caption_->setFont(fonts.role(QStringLiteral("caption")));
    proxy_->setTableBackground(background);
    proxy_->setBaseFont(table);
    relayoutColumns();
}

void ReportTable::setCaption(const QString& text, bool show) {
    caption_->setText(text);
    caption_->setVisible(show);
}

void ReportTable::setFocusFrame(bool shown, QColor colour) {
    focusFrame_ = shown;
    focusColour_ = colour;
    update();
}

void ReportTable::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    if (!focusFrame_) {
        return;
    }
    QPainter p(this);
    QPen pen(focusColour_);
    pen.setWidth(kFrameLogical);
    pen.setJoinStyle(Qt::MiterJoin);
    p.setPen(pen);
    p.drawRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0));
}

} // namespace esses::widgets::detail
