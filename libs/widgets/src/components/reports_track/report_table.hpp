// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Private to the report components (channel_report, time_report; 04 sections 12 item 1 and 13): the table
// they show. Two QTableViews over one ReportStyleProxy share a selection model: the left one shows column 0
// only (the frozen row labels), the right one the other columns; their vertical scrolling follows each other.
// ReportStyleProxy turns qt::ReportModel's plain styling roles into Qt::FontRole (italic untrusted rows,
// bold fastest times) and Qt::BackgroundRole (the min/max highlight and the time-report bands, tinted over
// the table background), which esses_qtmodel cannot (D-161). The caption bar above shows the component's
// caption (04 section 7.3) and the focus border is painted around the table (04 section 7.2 item 1). Both
// views paint their cells through one ReportCellDelegate, which leaves out the keyboard-focus mark while the
// table is painted for Print to Clipboard.
#pragma once

#include <esses/project/scheme.hpp>

#include <QColor>
#include <QFont>
#include <QHash>
#include <QIdentityProxyModel>
#include <QWidget>
#include <cstdint>

class QLabel;
class QTableView;

namespace esses::widgets::detail {

class ReportCellDelegate;

class ReportStyleProxy final : public QIdentityProxyModel {
    Q_OBJECT
public:
    explicit ReportStyleProxy(QObject* parent = nullptr);
    void setBaseFont(const QFont& font);
    void setTableBackground(QColor colour);
    // The tint of a highlight or band colour over the table background (the colour at 45 %).
    static QColor tint(std::uint32_t rgba, QColor background);
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

private:
    QFont font_;
    QColor background_ = Qt::white;
};

class ReportTable final : public QWidget {
    Q_OBJECT
public:
    ReportTable(QAbstractItemModel* model, QWidget* parent = nullptr);
    ~ReportTable() override;

    void applyScheme(const project::Scheme& scheme);
    // Applies a pending layout change at once (before Print to Clipboard).
    void settleLayout();
    // While on, the current cell carries no keyboard-focus mark (Print to Clipboard, 04 section 7.2 item 9):
    // Qt draws it only when the view has the focus and its window took the focus by the keyboard, or by being
    // activated with nothing focused (Qt then focuses the first child with Qt::TabFocusReason), so the same
    // table gave other pictures depending on whether and how it had the focus (PR #5, the ubuntu qt-app run
    // of 750eae4). On screen the mark stays.
    void setExporting(bool on);
    void setCaption(const QString& text, bool show);
    void setFocusFrame(bool shown, QColor colour);

    QTableView* frozenView() const noexcept { return frozen_; }
    QTableView* bodyView() const noexcept { return body_; }
    QLabel* captionLabel() const noexcept { return caption_; }
    ReportStyleProxy* proxy() const noexcept { return proxy_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void relayoutColumns();
    int contentWidth(int column) const;
    // The advance of each (font, text) pair measured so far (D-203 (aa)): a delivery of the same report
    // measures its cells from here. Cleared with the scheme and when it grows past a bound.
    mutable QHash<QString, int> widthCache_;

    ReportStyleProxy* proxy_ = nullptr;
    QLabel* caption_ = nullptr;
    QTableView* frozen_ = nullptr;
    QTableView* body_ = nullptr;
    ReportCellDelegate* delegate_ = nullptr;
    bool focusFrame_ = false;
    QColor focusColour_;
};

// Whether a report waits for its next show before re-running: it sits on a hidden page of a shown window (a
// kept worksheet, D-203 (z)). A standalone widget (tests, Print to Clipboard) runs at once.
bool deferWhileHidden(const QWidget& report);

// 0xRRGGBBAA of a "#rrggbb" or "#rrggbbaa" body colour; `fallback` for anything else.
std::uint32_t parseBodyColour(const QString& text, std::uint32_t fallback);
QColor colourOf(std::uint32_t rgba);

} // namespace esses::widgets::detail
