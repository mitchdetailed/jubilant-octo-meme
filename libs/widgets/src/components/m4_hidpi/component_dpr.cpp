// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ComponentWidget::recheckDevicePixelRatio (component_widget.hpp, "M4 (hidpi package)"; D-209). Root of the
// M4 hidpi package in src/components/ (docs/milestones/M4.md section 3): component_widget.cpp belongs to the
// followups package, so the hidpi member of ComponentWidget is defined here; it uses the class's private
// members.

#include <esses/widgets/components/component_widget.hpp>

namespace esses::widgets {

// The rule of ComponentWidget::event for QEvent::DevicePixelRatioChange / ScreenChangeInternal (04
// section 6.6 item 3, 06 section 17 item 3), for the widgets Qt does not notify (DprWatcher): a component
// laid out at another DPR than its window's now queues the full Size | Scheme rebuild (the 30 Hz size
// throttle applies). A component never laid out has no DPR to compare (its first size change brings the
// window's); a rebuild already queued is not queued twice.
bool ComponentWidget::recheckDevicePixelRatio() {
    if (dpr_ <= 0.0 || devicePixelRatioF() == dpr_) {
        return false;
    }
    const plot::ChangeMask rebuild = plot::Change::Size | plot::Change::Scheme;
    if ((sizePending_ & rebuild) == rebuild) {
        return false;
    }
    queueSizeChange(rebuild);
    return true;
}

} // namespace esses::widgets
