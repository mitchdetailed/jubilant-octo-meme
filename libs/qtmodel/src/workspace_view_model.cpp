// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// WorkspaceViewModel (04 section 2.1 item 3, 06 section 4.5, D-133): the one shared zoom/cursor/datum.

#include <esses/qtmodel/workspace_view_model.hpp>

#include <algorithm>
#include <utility>

namespace esses::qt {

namespace {

ZoomFraction fractionOf(plot::XSpan span, double mainExtent, ZoomFraction previous) {
    if (!(mainExtent > 0.0)) {
        return previous;
    }
    return ZoomFraction{span.a / mainExtent, span.b / mainExtent};
}

} // namespace

WorkspaceViewModel::WorkspaceViewModel(QObject* parent) : QObject(parent) {}

WorkspaceViewModel::~WorkspaceViewModel() = default;

void WorkspaceViewModel::setZoom(plot::XSpan span, plot::Domain domain, double mainExtent) {
    const ZoomFraction frac = fractionOf(span, mainExtent, zoomFrac_);
    if (span == zoom_ && domain == zoomDomain_ && frac.fa == zoomFrac_.fa && frac.fb == zoomFrac_.fb) {
        return;
    }
    zoom_ = span;
    zoomDomain_ = domain;
    zoomFrac_ = frac;
    ++version_;
    Q_EMIT changed(plot::mask(plot::Change::Zoom));
}

void WorkspaceViewModel::setCursor(Micros tMainLap) {
    if (tMainLap == cursorT_) {
        return;
    }
    cursorT_ = tMainLap;
    ++version_;
    Q_EMIT changed(plot::mask(plot::Change::Cursor));
}

void WorkspaceViewModel::setDatum(std::optional<Micros> tMainLap) {
    const std::optional<Micros> before = datumT();
    datumT_ = tMainLap;
    if (tMainLap) {
        datumVisible_ = true;
    }
    if (datumT() == before) {
        return;
    }
    ++version_;
    Q_EMIT changed(plot::mask(plot::Change::Datum));
}

void WorkspaceViewModel::setDatumVisible(bool visible) {
    const std::optional<Micros> before = datumT();
    datumVisible_ = visible;
    if (datumT() == before) {
        return;
    }
    ++version_;
    Q_EMIT changed(plot::mask(plot::Change::Datum));
}

void WorkspaceViewModel::setCursorDatum(Micros cursorT, std::optional<Micros> datumT) {
    plot::ChangeMask what = 0;
    if (cursorT != cursorT_) {
        what = what | plot::Change::Cursor;
    }
    if (datumT != datumT_ && (datumVisible_ || !datumT_)) {
        what = what | plot::Change::Datum;
    }
    cursorT_ = cursorT;
    datumT_ = datumT;
    if (what == 0) {
        return;
    }
    ++version_;
    Q_EMIT changed(what);
}

void WorkspaceViewModel::swapCursorDatum() {
    if (!datumT_ || *datumT_ == cursorT_) {
        return;
    }
    std::swap(cursorT_, *datumT_);
    ++version_;
    Q_EMIT changed(plot::Change::Cursor | plot::Change::Datum);
}

void WorkspaceViewModel::convertZoom(plot::Domain domain, double mainExtentInDomain,
                                     std::optional<plot::XSpan> converted,
                                     std::optional<plot::XSpan> clampTo) {
    plot::XSpan span =
        converted ? *converted
                  : plot::XSpan{zoomFrac_.fa * mainExtentInDomain, zoomFrac_.fb * mainExtentInDomain};
    if (clampTo && clampTo->length() > 0.0) {
        const double len = std::min(span.length(), clampTo->length());
        double a = std::clamp(span.a, clampTo->a, clampTo->b - len);
        span = plot::XSpan{a, a + len};
        if (!(span.length() > 0.0)) {
            span = *clampTo;
        }
    }
    zoom_ = span;
    zoomDomain_ = domain;
    zoomFrac_ = fractionOf(span, mainExtentInDomain, zoomFrac_);
    ++version_;
    Q_EMIT changed(plot::mask(plot::Change::Zoom));
}

} // namespace esses::qt
