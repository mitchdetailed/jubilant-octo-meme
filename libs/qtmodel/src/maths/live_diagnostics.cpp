// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LiveDiagnostics (03 section 11 item 2): the debounced live check of the Maths Editor's expression text.

#include <esses/qtmodel/live_diagnostics.hpp>
#include <esses/qtmodel/maths_service.hpp>

#include <QTimer>
#include <algorithm>
#include <utility>

namespace esses::qt {

LiveDiagnostics::LiveDiagnostics(MathsService& maths, QObject* parent)
    : QObject(parent), maths_(maths), timer_(new QTimer(this)) {
    timer_->setSingleShot(true);
    timer_->setInterval(kDebounceMs);
    connect(timer_, &QTimer::timeout, this, &LiveDiagnostics::flush);
    // A new or changed maths output may define (or remove) a name the text reads.
    connect(&maths_, &MathsService::statusChanged, this, [this] {
        if (!text_.isEmpty() && !timer_->isActive()) {
            timer_->start();
        }
    });
}

LiveDiagnostics::~LiveDiagnostics() = default;

void LiveDiagnostics::setSource(std::optional<model::SourceId> source) {
    if (source == source_) {
        return;
    }
    source_ = std::move(source);
    timer_->start();
}

void LiveDiagnostics::setExpression(const QString& text) {
    text_ = text;
    timer_->start(); // restarts: one check 150 ms after the last keystroke
}

void LiveDiagnostics::flush() {
    timer_->stop();
    const QByteArray utf8 = text_.toUtf8();
    diags_ = maths_.check(std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())), source_);
    Q_EMIT diagnosticsReady(diags_);
}

bool LiveDiagnostics::hasError() const noexcept {
    return std::any_of(diags_.begin(), diags_.end(),
                       [](const maths::Diagnostic& d) { return d.level == maths::Diagnostic::Error; });
}

bool LiveDiagnostics::pending() const noexcept {
    return timer_->isActive();
}

} // namespace esses::qt
