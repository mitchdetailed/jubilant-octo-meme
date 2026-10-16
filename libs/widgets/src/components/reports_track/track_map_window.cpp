// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TrackMapWindow (06 section 9). Owner: src/components/reports_track/.

#include <esses/base/hash.hpp>
#include <esses/base/uuid.hpp>
#include <esses/plotcore/track_geometry.hpp>
#include <esses/widgets/components/track_map_window.hpp>
#include <esses/widgets/components/track_position.hpp>

#include <QAction>
#include <QContextMenuEvent>
#include <QLabel>
#include <QMenu>
#include <QVBoxLayout>

namespace esses::widgets {

namespace {

project::ComponentSpec dockSpec() {
    project::ComponentSpec spec;
    spec.id = uuidFromHash(hash128(std::string_view("esses.trackmap.dock"))); // one stable build key
    spec.type = TrackPosition::kType;
    spec.caption.show = false; // the dock's title bar names it
    spec.links = project::Links{true, true, true};
    plot::TrackPositionBody body;
    body.showSectionLabels = false;
    spec.body = body.toJson();
    return spec;
}

plot::TrackPositionBody bodyOf(const project::ComponentSpec& spec) {
    Result<plot::TrackPositionBody> b = plot::TrackPositionBody::parse(spec.body);
    return b ? std::move(b).value() : plot::TrackPositionBody{};
}

} // namespace

TrackMapWindow::TrackMapWindow(QWidget* parent)
    : QWidget(parent), spec_(dockSpec()), note_(new QLabel(this)), layout_(new QVBoxLayout(this)) {
    setObjectName(QStringLiteral("trackMapWindow"));
    layout_->setContentsMargins(0, 0, 0, 0);
    note_->setText(tr("No worksheet"));
    note_->setAlignment(Qt::AlignCenter);
    layout_->addWidget(note_);

    fit_ = new QAction(tr("Fit"), this);
    fit_->setObjectName(QStringLiteral("trackMapFit"));
    connect(fit_, &QAction::triggered, this, &TrackMapWindow::fit);
    sections_ = new QAction(tr("Show Sections"), this);
    sections_->setObjectName(QStringLiteral("trackMapSections"));
    sections_->setCheckable(true);
    connect(sections_, &QAction::toggled, this, &TrackMapWindow::setShowSections);
    labels_ = new QAction(tr("Show Section Labels"), this);
    labels_->setObjectName(QStringLiteral("trackMapSectionLabels"));
    labels_->setCheckable(true);
    connect(labels_, &QAction::toggled, this, &TrackMapWindow::setShowSectionLabels);
    editor_ = new QAction(tr("Track Editor..."), this);
    editor_->setObjectName(QStringLiteral("trackMapEditor"));
    connect(editor_, &QAction::triggered, this, &TrackMapWindow::editorRequested);
    regenerate_ = new QAction(tr("Regenerate Track..."), this);
    regenerate_->setObjectName(QStringLiteral("trackMapRegenerate"));
    connect(regenerate_, &QAction::triggered, this, &TrackMapWindow::regenerateRequested);
    addActions({fit_, sections_, labels_, editor_, regenerate_});
    syncActions();
}

TrackMapWindow::~TrackMapWindow() = default;

void TrackMapWindow::setContext(WorksheetContext* context) {
    if (context == context_ && (context == nullptr || position_ != nullptr)) {
        return;
    }
    context_ = context;
    if (position_ != nullptr) {
        // The spec it may still be committing (a zoom gesture) is the dock's already (applySpec below).
        delete position_;
        position_ = nullptr;
    }
    note_->setVisible(context_ == nullptr);
    if (context_ == nullptr) {
        return;
    }
    position_ = new TrackPosition(spec_, *context_, this);
    position_->setObjectName(QStringLiteral("trackMapPosition"));
    // The dock's settings are not a document: an edit applies at once (no undo command).
    connect(position_, &ComponentWidget::specEditRequested, this,
            [this](const project::ComponentSpec& next, const QString&) { applySpec(next); });
    layout_->addWidget(position_, 1);
    setFocusProxy(position_);
    position_->show();
}

void TrackMapWindow::applySpec(const project::ComponentSpec& spec) {
    spec_ = spec;
    if (position_ != nullptr) {
        position_->applySpec(spec_);
    }
    syncActions();
}

void TrackMapWindow::syncActions() {
    const plot::TrackPositionBody body = bodyOf(spec_);
    const QSignalBlocker a(sections_);
    const QSignalBlocker b(labels_);
    sections_->setChecked(body.showSectionColours || body.showSectionMarks);
    labels_->setChecked(body.showSectionLabels);
}

void TrackMapWindow::applyScheme(const project::Scheme& scheme) {
    if (position_ != nullptr) {
        position_->onSchemeChanged(scheme);
    }
}

void TrackMapWindow::fit() {
    if (position_ != nullptr) {
        position_->fit();
        return;
    }
    plot::TrackPositionBody body = bodyOf(spec_);
    body.scaleMode = "fit";
    body.zoom = 1.0;
    body.panX = 0.0;
    body.panY = 0.0;
    project::ComponentSpec next = spec_;
    next.body = body.toJson();
    applySpec(next);
}

void TrackMapWindow::setShowSections(bool shown) {
    plot::TrackPositionBody body = bodyOf(spec_);
    body.showSectionColours = shown;
    body.showSectionMarks = shown;
    project::ComponentSpec next = spec_;
    next.body = body.toJson();
    applySpec(next);
}

void TrackMapWindow::setShowSectionLabels(bool shown) {
    plot::TrackPositionBody body = bodyOf(spec_);
    body.showSectionLabels = shown;
    project::ComponentSpec next = spec_;
    next.body = body.toJson();
    applySpec(next);
}

void TrackMapWindow::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    menu.addAction(fit_);
    menu.addSeparator();
    menu.addAction(sections_);
    menu.addAction(labels_);
    menu.addSeparator();
    menu.addAction(editor_);
    menu.addAction(regenerate_);
    menu.exec(event->globalPos());
    event->accept();
}

} // namespace esses::widgets
