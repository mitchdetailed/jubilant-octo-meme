// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/widgets/components/placeholder_component.hpp>

#include <QPainter>

namespace esses::widgets {

PlaceholderComponent::PlaceholderComponent(const project::ComponentSpec& spec, WorksheetContext& context,
                                           QWidget* parent)
    : ComponentWidget(spec, context, parent), typeUtf8_(QByteArray::fromStdString(spec.type)) {}

PlaceholderComponent::~PlaceholderComponent() = default;

void PlaceholderComponent::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), palette().window());
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    p.drawText(rect(), Qt::AlignCenter,
               tr("%1\nunsupported in this version").arg(QString::fromUtf8(typeUtf8_)));
}

} // namespace esses::widgets
