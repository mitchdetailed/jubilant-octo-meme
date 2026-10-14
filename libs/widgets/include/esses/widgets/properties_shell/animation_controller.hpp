// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AnimationController (04 section 8.5 item 7; 06 sections 2.1, 2.2 and 4.5; D-027 "animation (A, basic)"; 07
// section 6.1; AC-M3-10): the shell side of cursor animation. data.animate (A, the toolbar toggle) starts and
// stops it on the active worksheet; the Data -> Animation Rate menu sets the rate (x0.25, x0.5, x1, x2, x4;
// options.animation.rate); switching worksheets moves a running animation to the new worksheet; "reduce
// motion" (QStyleHints) starts it paused and Options -> General "Reduce CPU during animation" lowers the tick
// rate. The ticks themselves are WorksheetViewModel's (setAnimating / setAnimationRate: a QTimer advancing
// cursorT by rate * dt, a Cursor-only change, so components repaint Dynamic layers only; AC-M3-10: <= 4 ms
// CPU per frame at x10). M3 contract: rates() and the accessors are final.
// As built (M3): the state survives without a worksheet (the animation starts on the next setWorksheet);
// setWorksheet(nullptr) stops it; the worksheet stopping by itself at the end of the outing turns the state
// off (checked on every change of the worksheet and every 250 ms while animating). Qt 6 has no reduce-motion
// hint and WorksheetViewModel's tick interval is fixed (33 ms), so neither "reduce motion" nor "Reduce CPU
// during animation" changes anything yet (D-200).
#pragma once

#include <QObject>
#include <array>

class QTimer;

namespace esses::qt {
class WorksheetViewModel;
} // namespace esses::qt

namespace esses::widgets {

class AnimationController final : public QObject {
    Q_OBJECT
public:
    static constexpr std::array<double, 5> kRates{0.25, 0.5, 1.0, 2.0, 4.0}; // Data -> Animation Rate

    explicit AnimationController(QObject* parent = nullptr);
    ~AnimationController() override;

    // The active worksheet (null: none; a running animation stops). Not owned.
    void setWorksheet(qt::WorksheetViewModel* worksheet);
    qt::WorksheetViewModel* worksheet() const noexcept { return worksheet_; }
    bool animating() const noexcept { return animating_; }
    double rate() const noexcept { return rate_; }

    void setAnimating(bool on);
    void toggle(); // data.animate
    void setRate(double rate);

Q_SIGNALS:
    void animatingChanged(bool on);
    void rateChanged(double rate);

private:
    void syncWithWorksheet();

    qt::WorksheetViewModel* worksheet_ = nullptr;
    bool animating_ = false;
    double rate_ = 1.0;
    QTimer* poll_ = nullptr;
};

} // namespace esses::widgets
