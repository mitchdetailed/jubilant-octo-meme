// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// LiveDiagnostics (03 section 11 items 2, 3 and 12; D-021): the Maths Editor's live diagnostics feed. Every
// keystroke hands the expression text to setExpression(), which restarts a 150 ms debounce; when it expires
// MathsService::check parses and type checks the text against the scope of the Main source (setSource; a
// data-less scope when none, where unknown names are I-UNRESOLVED) with the project's compat switches, and
// diagnosticsReady() delivers the result (Errors, Warnings and Info with their spans). flush() checks at once
// (Apply, close). The text is checked again when the maths outputs change (a new item may define a name the
// expression reads). Never evaluates and never blocks; GUI thread only. Added to the outline in M2 (the
// Maths Editor's debounce of 03 section 11 item 2, kept out of the widget so that it is tested in Qt Test).
#pragma once

#include <esses/maths/diagnostics.hpp>
#include <esses/model/ids.hpp>

#include <QObject>
#include <QString>
#include <optional>

class QTimer;

namespace esses::qt {

class MathsService;

class LiveDiagnostics final : public QObject {
    Q_OBJECT
public:
    static constexpr int kDebounceMs = 150; // 03 section 11 item 2

    explicit LiveDiagnostics(MathsService& maths, QObject* parent = nullptr);
    ~LiveDiagnostics() override;

    // The Main source whose scope the text is checked against (nullopt: data-less); re-checks after the
    // debounce.
    void setSource(std::optional<model::SourceId> source);
    const std::optional<model::SourceId>& source() const noexcept { return source_; }
    // The edited text; restarts the debounce.
    void setExpression(const QString& text);
    const QString& expression() const noexcept { return text_; }
    // Checks the current text now (the debounce is cancelled); diagnosticsReady() fires.
    void flush();
    // The diagnostics of the last check; whether one is an Error (the item cannot be applied, 03 section 11
    // item 3; Warnings never block).
    const maths::Diagnostics& diagnostics() const noexcept { return diags_; }
    bool hasError() const noexcept;
    bool pending() const noexcept; // a check is scheduled

Q_SIGNALS:
    void diagnosticsReady(const esses::maths::Diagnostics& diagnostics);

private:
    MathsService& maths_;
    QTimer* timer_ = nullptr;
    QString text_;
    std::optional<model::SourceId> source_;
    maths::Diagnostics diags_;
};

} // namespace esses::qt
