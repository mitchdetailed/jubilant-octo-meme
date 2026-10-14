// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// AnimationController (04 section 8.5 item 7, 06 section 4.5, AC-M3-10). Owner: src/properties_shell/.

#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/properties_shell/animation_controller.hpp>

#include <QTimer>

namespace esses::widgets {

AnimationController::AnimationController(QObject* parent) : QObject(parent), poll_(new QTimer(this)) {
    poll_->setInterval(250);
    connect(poll_, &QTimer::timeout, this, &AnimationController::syncWithWorksheet);
}

AnimationController::~AnimationController() {
    if (worksheet_ != nullptr && animating_) {
        worksheet_->setAnimating(false);
    }
}

void AnimationController::syncWithWorksheet() {
    if (animating_ && worksheet_ != nullptr && !worksheet_->animating()) {
        animating_ = false; // the end of the outing
        poll_->stop();
        Q_EMIT animatingChanged(false);
    }
}

void AnimationController::setWorksheet(qt::WorksheetViewModel* worksheet) {
    if (worksheet == worksheet_) {
        return;
    }
    if (worksheet_ != nullptr) {
        QObject::disconnect(worksheet_, nullptr, this, nullptr);
        worksheet_->setAnimating(false);
    }
    worksheet_ = worksheet;
    if (worksheet_ == nullptr) {
        if (animating_) {
            animating_ = false;
            poll_->stop();
            Q_EMIT animatingChanged(false);
        }
        return;
    }
    connect(worksheet_, &QObject::destroyed, this, [this] {
        worksheet_ = nullptr;
        if (animating_) {
            animating_ = false;
            poll_->stop();
            Q_EMIT animatingChanged(false);
        }
    });
    connect(worksheet_, &qt::WorksheetViewModel::changed, this, &AnimationController::syncWithWorksheet);
    worksheet_->setAnimationRate(rate_);
    if (animating_) {
        worksheet_->setAnimating(true); // a running animation moves to the new worksheet
    }
}

void AnimationController::setAnimating(bool on) {
    // Without a worksheet the state is kept and the animation starts on the next setWorksheet().
    if (worksheet_ != nullptr) {
        worksheet_->setAnimationRate(rate_);
        worksheet_->setAnimating(on);
    }
    if (on) {
        poll_->start();
    } else {
        poll_->stop();
    }
    if (animating_ != on) {
        animating_ = on;
        Q_EMIT animatingChanged(on);
    }
}

void AnimationController::toggle() {
    setAnimating(!animating_);
}

void AnimationController::setRate(double rate) {
    if (rate > 0.0 && rate != rate_) {
        rate_ = rate;
        if (worksheet_ != nullptr) {
            worksheet_->setAnimationRate(rate);
        }
        Q_EMIT rateChanged(rate);
    }
}

} // namespace esses::widgets
