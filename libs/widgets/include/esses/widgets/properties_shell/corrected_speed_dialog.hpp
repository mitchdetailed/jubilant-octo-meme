// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// CorrectedSpeedDialog (06 section 13.6; 03 section 9.1; D-016; 07 section 6.1): the Corrected Speed and
// Distance dialog.
//   1. method: Fastest Wheel (two wheel channels), Alternate Channel List (ordered, first existing wins),
//      G Correction (speed + longitudinal-G channel; analysis::CorrectedSpeed::gCorrection)
//   2. clamping: min/max in the speed display unit with "invalidate instead of clamp"
//   3. the distance unit follows the speed unit (hint pointing to the Channel Editor)
//   4. lap distance stretching: enable, tolerance % (default 5, per project), the per-lap stretch report
//      (read-only, shared with the Lap Editor)
//   5. Apply re-registers esses.corrected_speed_distance with the new settings for every source and
//      invalidates its cone (D-016, D-022); the module status line; the note that results are not
//      bit-identical to other tools
//   6. settings persist in project.json builtinMaths["esses.corrected_speed_distance"] {version, enabled,
//      settings}, validated by schemas/builtin-esses.corrected_speed_distance-1.schema.json; Apply is one
//      undo command (SetProjectOptionsCommand family)
// As built (M3): the pages bind the settings object through a FieldBinder against the module's settings
// schema (method, the fastest-wheel pair and the alternate list as channel lists, the gCorrection inputs,
// the clamp limits as optional {v, unit} values, the distance source and the output names), plus the module's
// enabled flag and options.lapStretch {enabled, tolerancePct}; Apply writes all three as one
// SetProjectOptionsCommand and ProjectSession::documentsChanged re-registers the module with MathsService
// (connectProjectSession). Fields left alone keep what project.json holds (absent members stay absent). The
// per-lap stretch report is the Lap Editor's table (the dialog points to it). The status line shows the
// module's state for the Main source's Corr Speed when there is one.
#pragma once

#include <esses/analysis/corrected_speed.hpp>

#include <QDialog>
#include <QJsonObject>
#include <memory>

class QCheckBox;
class QLabel;

namespace esses::qt {
class MathsService;
class ProjectSession;
} // namespace esses::qt

namespace esses::widgets {

class FieldBinder;
class NumberEdit;
class SchemaTree;

class CorrectedSpeedDialog final : public QDialog {
    Q_OBJECT
public:
    static constexpr const char* kModuleId = "esses.corrected_speed_distance";
    static constexpr const char* kGeometryKey = "correctedSpeed";

    CorrectedSpeedDialog(qt::ProjectSession& session, qt::MathsService& maths, QWidget* parent = nullptr);
    ~CorrectedSpeedDialog() override;

    const analysis::CorrectedSpeedSettings& settings() const noexcept { return settings_; }
    void setSettings(const analysis::CorrectedSpeedSettings& settings);
    // Writes the settings (one undo command) and re-registers the module.
    void apply();

    // Added in M3 (as built): the settings binder (pointers into the settings object), the module and
    // lap-stretch fields, the settings object the pages describe, the first validation failure ("" when
    // valid), reloading from project.json.
    FieldBinder& binder() const noexcept { return *binder_; }
    QCheckBox& enabledBox() const noexcept { return *enabled_; }
    QCheckBox& stretchBox() const noexcept { return *stretch_; }
    NumberEdit& toleranceEdit() const noexcept { return *tolerance_; }
    QJsonObject editedSettings() const;
    QString validationMessage() const;
    void reload();

private:
    void updateStatus();

    analysis::CorrectedSpeedSettings settings_;
    qt::ProjectSession& session_;
    qt::MathsService& maths_;
    std::unique_ptr<SchemaTree> schema_;
    FieldBinder* binder_ = nullptr;
    QCheckBox* enabled_ = nullptr;
    QCheckBox* stretch_ = nullptr;
    NumberEdit* tolerance_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* message_ = nullptr;
    QJsonObject loaded_;
};

} // namespace esses::widgets
