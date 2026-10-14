// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ComponentWidget (04 section 7.1, 06 section 4.4): the factory, the context wiring, the 30 Hz Size
// coalescing, DPR changes, focus and the caption double-click (04 section 7.2 items 1 and 7).

#include <esses/maths/channel_key.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/components/placeholder_component.hpp>
#include <esses/widgets/properties_shell/properties_dialog.hpp>

#include <QEvent>
#include <QHash>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QTimer>
#include <utility>

namespace esses::widgets {

namespace {

QHash<QString, ComponentWidget::Factory>& factories() {
    static QHash<QString, ComponentWidget::Factory> table; // GUI thread only
    return table;
}

constexpr int kCaptionLogicalPx = 18; // 04 section 8.1 (scheme default)

} // namespace

ComponentWidget* ComponentWidget::create(const project::ComponentSpec& spec, WorksheetContext& context,
                                         QWidget* parent) {
    const QString type = QString::fromStdString(spec.type);
    const auto it = factories().constFind(type);
    if (spec.known && it != factories().constEnd()) {
        return it.value()(spec, context, parent);
    }
    return new PlaceholderComponent(spec, context, parent);
}

void ComponentWidget::registerType(const QString& type, Factory factory) {
    factories().insert(type, std::move(factory));
}

bool ComponentWidget::isRegistered(const QString& type) {
    return factories().contains(type);
}

ComponentWidget::ComponentWidget(const project::ComponentSpec& spec, WorksheetContext& context,
                                 QWidget* parent)
    : QWidget(parent), spec_(spec), context_(context), sizeTimer_(new QTimer(this)) {
    setFocusPolicy(Qt::StrongFocus);
    sizeTimer_->setSingleShot(true);
    sizeTimer_->setInterval(kSizeCoalesceMs);
    connect(sizeTimer_, &QTimer::timeout, this, [this] {
        if (sizePending_ != 0) {
            flushSizeChange();
            sizeTimer_->start(); // keep throttling while the burst lasts
        }
    });

    connect(&context_.view, &qt::WorksheetViewModel::changed, this, [this](plot::ChangeMask what) {
        // The worksheet's Size bit is its cursor-step width (WorksheetViewModel::setSize); a component's own
        // size reaches it through resizeEvent.
        what = static_cast<plot::ChangeMask>(what & ~plot::mask(plot::Change::Size));
        if (what != 0) {
            onViewChanged(context_.view.state(), what);
        }
    });
    connect(&context_.selection, &qt::DataSelectionModel::changed, this,
            [this](const plot::SelectionSnapshot& snapshot, qt::DataSelectionModel::Changes) {
                onSelectionChanged(snapshot);
            });
    const auto dataFromKey = [this](const maths::ChannelKey& key) { onDataChanged(key.src, 0); };
    connect(&context_.maths, &qt::MathsService::channelReady, this, dataFromKey);
    connect(&context_.maths, &qt::MathsService::statusChanged, this, dataFromKey);
    const auto dataFromSource = [this](const model::SourceId& source) { onDataChanged(source, 0); };
    qt::DistanceService& distance = context_.view.distanceService();
    connect(&distance, &qt::DistanceService::distanceReady, this, dataFromSource);
    connect(&distance, &qt::DistanceService::noDistance, this, dataFromSource);
}

ComponentWidget::~ComponentWidget() = default;

void ComponentWidget::applySpec(const project::ComponentSpec& spec) {
    spec_ = spec;
    update();
}

// Base-class behaviours (04 section 7.1): components override what applies to them.
void ComponentWidget::onViewChanged(const plot::ViewState&, plot::ChangeMask) {}
void ComponentWidget::onSelectionChanged(const plot::SelectionSnapshot&) {}
void ComponentWidget::onDataChanged(const model::SourceId&, std::uint64_t) {}
void ComponentWidget::onSchemeChanged(const project::Scheme&) {}

ComponentWidget::OverlayPolicy ComponentWidget::overlayPolicy() const {
    return OverlayPolicy::MainOnly;
}

QStringList ComponentWidget::channelNames() const {
    return {};
}

std::optional<QString> ComponentWidget::activeChannel() const {
    if (spec_.activeChannel.empty()) {
        return std::nullopt;
    }
    return QString::fromStdString(spec_.activeChannel);
}

bool ComponentWidget::acceptsChannelDrop() const {
    return false;
}

void ComponentWidget::addChannel(const QString&, bool) {}

QStringList ComponentWidget::contextCommands() const {
    return {};
}

bool ComponentWidget::exportData(ExportSink&, const ExportOptions&) const {
    return false; // CSV export arrives with the Component menu (M3)
}

QImage ComponentWidget::renderToImage(qreal dpr) const {
    dpr = dpr > 0.0 ? dpr : devicePixelRatioF();
    QImage img(size() * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    const_cast<ComponentWidget*>(this)->render(&img); // QWidget::render is non-const
    return img;
}

void ComponentWidget::openProperties(int tabIndex) { // F5 / CTRL+N (06 section 12, D-044)
    PropertiesDialog::openFor(*this, tabIndex);
}

qt::CommandContext ComponentWidget::commandContext() const {
    return qt::CommandContext::Component;
}

plot::HitResult ComponentWidget::hitAt(QPointF) const {
    return {};
}

void ComponentWidget::setFocusedComponent(bool focused) {
    if (focusedComponent_ == focused) {
        return;
    }
    focusedComponent_ = focused;
    onFocusChanged(focused);
}

void ComponentWidget::onFocusChanged(bool) {
    update();
}

int ComponentWidget::captionHeight() const {
    return spec_.caption.show ? kCaptionLogicalPx : 0;
}

bool ComponentWidget::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::DevicePixelRatioChange:
    case QEvent::ScreenChangeInternal:
        // A new DPR invalidates every layer and re-lays out the key column (06 section 17 item 3). Qt sends
        // the event to every widget of the window; one rebuild per real change.
        if (dpr_ > 0.0 && devicePixelRatioF() != dpr_) {
            queueSizeChange(plot::Change::Size | plot::Change::Scheme);
        }
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void ComponentWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    queueSizeChange(plot::mask(plot::Change::Size));
}

void ComponentWidget::queueSizeChange(plot::ChangeMask what) {
    sizePending_ = static_cast<plot::ChangeMask>(sizePending_ | what);
    if (!sizeTimer_->isActive()) {
        flushSizeChange(); // leading edge: the first change of a burst at once
        sizeTimer_->start();
    }
}

bool ComponentWidget::sizeThrottleActive() const {
    return sizeTimer_->isActive();
}

bool ComponentWidget::handleCommand(const QString&, const QVariantList&) {
    return false; // the shell's generic behaviour applies (M3)
}

bool ComponentWidget::cancelTransient() {
    return false;
}

void ComponentWidget::flushSizeChange() {
    const plot::ChangeMask what = sizePending_;
    sizePending_ = 0;
    if (what == 0) {
        return;
    }
    dpr_ = devicePixelRatioF();
    ++sizeDeliveries_;
    onViewChanged(context_.view.state(), what);
}

void ComponentWidget::mousePressEvent(QMouseEvent* event) {
    QWidget::mousePressEvent(event);
    Q_EMIT focusRequested();
}

void ComponentWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && event->position().y() < captionHeight()) {
        Q_EMIT maximiseRequested();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace esses::widgets
