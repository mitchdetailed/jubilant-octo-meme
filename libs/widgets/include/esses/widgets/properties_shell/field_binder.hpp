// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// FieldBinder (06 section 12 item 6; D-027; AC-M3-08): the one generic binding between a component body and
// the editors of a PropertiesDialog page. A binding row is {JSON pointer into the body, editor kind, label};
// load() fills every bound editor from a body, store() writes the editors back over a copy of the body
// (members no row binds survive unchanged), validate() runs the inline validation (red field + reason, 06
// section 12 item 4). The editors, validation and undo wiring are shared by every component type, so a page
// is a layout plus a binding table; only the time_distance/outing groups tree and the scatter Y-trace list
// are custom widgets, which bind their subtree with kind Custom. AC-M3-08 (test properties_roundtrip): for
// each component type every property of its body schema (schemas/components/<type>-<version>.schema.json,
// schemaPointers()) has a binding; an unbound property fails.
// As built (M3): a row whose editor still shows what load() put there leaves the body's member as it was
// (absent stays absent, a number keeps its exact value), so opening and applying changes nothing; a changed
// row writes its editor's value (an empty Number or an Enum without a choice removes the member). With
// setSchema() the rows are validated against the schema node of their pointer (type, enum, ranges, pattern,
// lengths, required members) and an editor that fails is marked red with the reason as tool tip. The leaf
// kinds use plain Qt widgets (field_editors.hpp: Bool QCheckBox, Enum QComboBox, Integer QSpinBox, Number
// NumberEdit, Text QLineEdit, Colour ColourEdit, Channel an editable QComboBox); UnitValue, BandAxis,
// ColourBands and Custom rows take a FieldEditor. loadJson/storeJson are the same on QJsonValue (nested
// forms bind a sub-object).
#pragma once

#include <esses/base/json_doc.hpp>

#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <cstdint>
#include <memory>
#include <vector>

namespace esses::widgets {

class SchemaTree;

enum class FieldKind : std::uint8_t {
    Bool,
    Enum, // a string from enumValues
    Integer,
    Number,
    UnitValue, // {v, unit} (D-010)
    Text,
    Colour,      // "#rrggbb[aa]"
    Channel,     // channel picker (a name)
    BandAxis,    // the BandAxis editor
    ColourBands, // the ColourBands editor
    Custom       // a custom widget that owns the subtree (groups tree, scatter Y list)
};

struct FieldBinding {
    QString pointer; // RFC 6901 pointer into the body ("/display/variance/heightFrac")
    FieldKind kind = FieldKind::Text;
    QString label;
    QStringList enumValues; // Enum
    QPointer<QWidget> editor;
};

class FieldBinder final : public QObject {
    Q_OBJECT
public:
    explicit FieldBinder(QObject* parent = nullptr);
    ~FieldBinder() override;

    void bind(const QString& pointer, FieldKind kind, QWidget* editor, const QString& label,
              const QStringList& enumValues = {});
    const std::vector<FieldBinding>& bindings() const noexcept { return bindings_; }
    QStringList boundPointers() const;
    // True when `pointer` or one of its ancestors is bound (a Custom subtree binds its descendants).
    bool isBound(const QString& pointer) const;

    void load(const JsonDoc& body);
    JsonDoc store(const JsonDoc& body) const;
    // False with the first failing row's label and reason.
    bool validate(QString* reason = nullptr) const;

    // The pointer of every leaf property a body schema allows (objects are descended, array items as "/0";
    // shared definitions resolved), the list AC-M3-08 checks bindings against.
    static QStringList schemaPointers(const JsonDoc& schema);

    // Added in M3 (as built): the schema the rows validate against (`prefix`: the pointer of the bound
    // object inside it, for a nested form; not owned, must outlive the binder); the QJsonValue forms of
    // load/store; the binding of a pointer (nullptr when none binds it exactly); the leaf widget that edits
    // `pointer` (a bound leaf, else the bound ancestor's FieldEditor::leafEditor); whether any row differs
    // from what load() put there.
    void setSchema(const SchemaTree* schema, const QString& prefix = {});
    void loadJson(const QJsonValue& body);
    QJsonValue storeJson(const QJsonValue& body) const;
    const FieldBinding* binding(const QString& pointer) const;
    QWidget* editorFor(const QString& pointer) const;
    bool isModified() const;

Q_SIGNALS:
    void edited(); // any bound editor changed
    void validityChanged(bool valid);

private:
    void watch(std::size_t index);
    void onEdited();
    QJsonValue editorValue(const FieldBinding& binding) const;
    bool validateRow(std::size_t index, QString* reason) const;

    std::vector<FieldBinding> bindings_;
    std::vector<QJsonValue> initial_; // per row: the editor's value right after load()
    std::vector<bool> loaded_;        // per row: load() ran since the row was bound
    const SchemaTree* schema_ = nullptr;
    QString schemaPrefix_;
    int loading_ = 0;
    bool lastValid_ = true;
};

} // namespace esses::widgets
