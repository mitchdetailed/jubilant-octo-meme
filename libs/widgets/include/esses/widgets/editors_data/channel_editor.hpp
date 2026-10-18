// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ChannelEditor (CTRL+E; 06 section 13.1, D-010, D-013, D-032): every channel of every open source in one
// table (Source, Name, Logged name, Category, Rate, Type, Unit, Min, Max, DPS, Colour, Interp, Override),
// sortable by every column (numbers by value), filtered with the Channels-window search semantics
// (qt::ChannelsModel::filterAccepts, loose with options.general.looseChannelSearch). With a MathsService the
// maths outputs of each source are rows too (Category "Maths"), so the Corr Dist unit is editable here (it
// changes the distance axis units). One channel is edited at a time (multi-select edit is v1.0): the Unit
// (a combo of the quantity's units; compat units excluded), Min and Max (numbers in the display unit, stored
// as {v, unit}), DPS, Colour (-1 = automatic) and Interp cells write channels/overrides.json through
// SetOverrideCommand, "Clear override" through ClearOverrideCommand; each edit is one undo step. A unit
// override that cannot apply to every open file carrying the channel (another quantity family there) is
// refused with the list of conflicting files (editRefused, lastRefusal). Aliases... and Mappings... open the
// two small editors (channel_documents_dialogs.hpp), which push EditAliasesCommand / EditMappingsCommand.
// Implemented in src/editors_maths/channel_editor.cpp (M2: the maths-editors work owns the Channel Editor).
#pragma once

#include <QDialog>
#include <QString>
#include <memory>
#include <optional>

class QAbstractItemModel;
class QLineEdit;
class QTableView;

namespace esses::qt {
class MathsService;
class ProjectSession;
class SourceLoader;
} // namespace esses::qt

namespace esses::widgets {

class AliasesDialog;
class MappingsDialog;

class ChannelEditor final : public QDialog {
    Q_OBJECT
public:
    static constexpr const char* kGeometryKey = "channelEditor";
    enum Column : int {
        SourceCol,
        NameCol,
        LoggedCol,
        CategoryCol,
        RateCol,
        TypeCol,
        UnitCol,
        MinCol,
        MaxCol,
        DpsCol,
        ColourCol,
        InterpCol,
        OverrideCol,
        ColumnCount
    };

    ChannelEditor(qt::ProjectSession& session, qt::SourceLoader& loader, qt::MathsService* maths = nullptr,
                  QWidget* parent = nullptr);
    ~ChannelEditor() override;

    // The sorted and filtered view; its model is the filter proxy over channelModel().
    QTableView& table() noexcept { return *table_; }
    QLineEdit& filterEdit() noexcept;
    // The rows in source order (one per channel and open source).
    QAbstractItemModel& channelModel() noexcept;
    // Rebuilds the rows from the open sources and the overrides.
    void refresh();
    // Shows the row of `channel` (first source that has it), clearing a filter that hides it; false when
    // none.
    bool focusChannel(const QString& channel);
    // The name of the current row's channel ("" when none).
    QString currentChannel() const;

    // The edits behind the cells (each one SetOverrideCommand; false with the reason when refused).
    bool setDisplayUnit(const QString& channel, const QString& unit, QString* refusal = nullptr);
    bool setLimit(const QString& channel, bool max, std::optional<double> valueInDisplayUnit,
                  QString* refusal = nullptr);
    bool setDps(const QString& channel, std::optional<int> dps);
    bool setColourIndex(const QString& channel, std::optional<int> colour);
    bool setInterp(const QString& channel, const QString& interp, QString* refusal = nullptr);
    // "Clear override" (ClearOverrideCommand); false when the channel has none.
    bool clearOverride(const QString& channel);
    // The reason of the last refused edit ("" when the last edit was applied).
    QString lastRefusal() const;

    // The Aliases and Mappings sub-editors (06 section 13.1 item 7), created on first use and shown modeless.
    void openAliases();
    void openMappings();
    AliasesDialog* aliasesDialog() const;
    MappingsDialog* mappingsDialog() const;

Q_SIGNALS:
    void editRefused(const QString& reason);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    qt::ProjectSession& session_;
    qt::SourceLoader& loader_;
    qt::MathsService* maths_ = nullptr;
    QTableView* table_ = nullptr;
};

} // namespace esses::widgets
