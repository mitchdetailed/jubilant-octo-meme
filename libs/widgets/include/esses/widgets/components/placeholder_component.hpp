// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// PlaceholderComponent (06 section 4.3 item 12, D-019): a component of a type this build does not implement:
// shows the type name, the caption and "unsupported in this version"; can be moved, resized, cut and pasted;
// its body is preserved verbatim (ComponentSpec::known == false).
#pragma once

#include <esses/widgets/components/component_widget.hpp>

#include <QByteArray>

namespace esses::widgets {

class PlaceholderComponent final : public ComponentWidget {
    Q_OBJECT
public:
    PlaceholderComponent(const project::ComponentSpec& spec, WorksheetContext& context,
                         QWidget* parent = nullptr);
    ~PlaceholderComponent() override;

    const char* type() const override { return typeUtf8_.constData(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QByteArray typeUtf8_;
};

} // namespace esses::widgets
