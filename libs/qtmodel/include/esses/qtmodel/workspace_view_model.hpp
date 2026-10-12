// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// WorkspaceViewModel (04 section 2.1 item 3, 06 section 4.5, D-133): the workspace-shared part of the view:
// the zoom span with the domain it was last set in (zoomDomain) and its fractions of the Main extent
// (zoomFrac, maintained on every zoom change for the F9 fallback rule of 04 section 3.4), the cursor and the
// datum (both Main-lap time). One per workspace, beside DataSelectionModel; each WorksheetViewModel composes
// its ViewState from this state and its own domain/offsetAxis. Switching worksheets changes neither the
// selection nor this state; when the newly active worksheet's domain differs from zoomDomain, convertZoom()
// applies the F9 rule. GUI thread only.
// Additions (M1): the datum keeps its value while hidden (D, 04 section 8.5 item 4: datumT() is the visible
// datum, storedDatumT() the value); setCursorDatum moves both in one Cursor|Datum change (N/P and Q keep the
// lap distance, 04 section 2.1 item 1); the active worksheet is the one whose domain the shared cursor and
// zoom conversions follow (06 section 4.1 item 4); zoomFrac is recomputed on convertZoom too.
#pragma once

#include <esses/base/time.hpp>
#include <esses/plotcore/change_mask.hpp>
#include <esses/plotcore/view_state.hpp>

#include <QObject>
#include <cstdint>
#include <optional>

namespace esses::qt {

struct ZoomFraction { // fractions of the Main extent in zoomDomain; < 0 or > 1 outside the Main range
    double fa = 0.0;
    double fb = 1.0;
};

class WorkspaceViewModel final : public QObject {
    Q_OBJECT
public:
    explicit WorkspaceViewModel(QObject* parent = nullptr);
    ~WorkspaceViewModel() override;

    plot::XSpan zoom() const noexcept { return zoom_; }
    plot::Domain zoomDomain() const noexcept { return zoomDomain_; }
    ZoomFraction zoomFrac() const noexcept { return zoomFrac_; }
    Micros cursorT() const noexcept { return cursorT_; }
    // The visible datum (nullopt while hidden or never placed).
    std::optional<Micros> datumT() const noexcept { return datumVisible_ ? datumT_ : std::nullopt; }
    std::optional<Micros> storedDatumT() const noexcept { return datumT_; }
    bool datumVisible() const noexcept { return datumVisible_ && datumT_.has_value(); }
    std::uint64_t version() const noexcept { return version_; }
    // The worksheet whose domain the shared conversions follow (WorksheetViewModel::activate()).
    const QObject* activeWorksheet() const noexcept { return active_; }
    void setActiveWorksheet(const QObject* worksheet) noexcept { active_ = worksheet; }

    // `mainExtent` is |Main| in `domain` (for zoomFrac).
    void setZoom(plot::XSpan span, plot::Domain domain, double mainExtent);
    void setCursor(Micros tMainLap);
    // Places (value) or clears (nullopt) the datum; a placed datum is visible.
    void setDatum(std::optional<Micros> tMainLap);
    // D: hides or shows the datum, keeping its value.
    void setDatumVisible(bool visible);
    // Cursor and stored datum in one Cursor|Datum change (visibility unchanged).
    void setCursorDatum(Micros cursorT, std::optional<Micros> datumT);
    void swapCursorDatum(); // X
    // Expresses the shared zoom in `domain` (F9 rule, 04 section 3.4): `converted` when the caller could map
    // the span through the Main range's DistanceChain (or converted it with plot::convertZoom), else zoomFrac
    // times `mainExtentInDomain`, clamped to `clampTo` when given. zoomFrac is recomputed in the new domain.
    void convertZoom(plot::Domain domain, double mainExtentInDomain, std::optional<plot::XSpan> converted,
                     std::optional<plot::XSpan> clampTo = std::nullopt);
    // Added in the M1 integration: the shared distance zoom is in the fraction rule's normalised units
    // because the Main lap's distance index was still pending when the active worksheet set it (F9, W, F2
    // before the index arrived); that worksheet re-expresses it in metres, keeping zoomFrac, when the index
    // arrives. Not part of changed(): a bookkeeping bit of the active WorksheetViewModel.
    bool zoomNormalised() const noexcept { return zoomNormalised_; }
    void setZoomNormalised(bool on) noexcept { zoomNormalised_ = on; }

Q_SIGNALS:
    void changed(esses::plot::ChangeMask what);

private:
    plot::XSpan zoom_;
    plot::Domain zoomDomain_ = plot::Domain::Time;
    ZoomFraction zoomFrac_;
    Micros cursorT_ = 0;
    std::optional<Micros> datumT_;
    bool datumVisible_ = true;
    bool zoomNormalised_ = false;
    const QObject* active_ = nullptr;
    std::uint64_t version_ = 0;
};

} // namespace esses::qt
