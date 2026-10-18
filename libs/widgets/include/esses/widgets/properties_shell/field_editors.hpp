// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The editors a FieldBinder binds (06 section 12 items 4 and 6; D-027; AC-M3-08), shared by every
// PropertiesDialog page:
//   leaf kinds    Bool QCheckBox, Enum QComboBox, Integer QSpinBox, Number NumberEdit, Text QLineEdit,
//                 Colour ColourEdit, Channel an editable QComboBox (makeLeafEditor builds them);
//   subtree kinds a FieldEditor: UnitValueEdit ({v, unit}, D-010), FieldForm (an object as rows of its own
//                 FieldBinder: BandAxis, scale lines, the gauge font), RecordListEditor (an array of records
//                 or scalars with Add / Remove / Up / Down and a detail editor: groups and traces, the
//                 scatter Y list, colour bands, report channels, statistics, headers), FixedArrayEdit (edges,
//                 density bins) and OptionalEditor (a member that may be null: the gauge font, the bar
//                 colour, the histogram's colour axis).
// A FieldEditor reports its whole subtree as one QJsonValue (Undefined = the member is absent) and offers
// leafEditor(relative pointer): the leaf widget that edits a descendant, creating missing array records and
// switching an OptionalEditor on on the way (the AC-M3-08 test mutates every schema leaf through it).
// jsonptr: RFC 6901 pointers on QJsonValue. SchemaTree: a body schema with its local definitions resolved
// (the leaf pointers AC-M3-08 binds and the per-field constraints of the inline validation).
#pragma once

#include <esses/widgets/properties_shell/field_binder.hpp>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineEdit>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QCheckBox;
class QFormLayout;
class QListWidget;
class QPushButton;
class QStackedLayout;

namespace esses::widgets {

namespace jsonptr {
// "/a/0/b" -> {"a", "0", "b"} ("~1" -> "/", "~0" -> "~"); "" -> {}.
QStringList split(const QString& pointer);
QString join(const QStringList& tokens);
// The value at `pointer`, QJsonValue::Undefined when absent.
QJsonValue get(const QJsonValue& root, const QString& pointer);
// `root` with `value` at `pointer` (Undefined removes the member; missing objects are created; an array index
// equal to the size appends). Returns `root` unchanged when a parent is neither an object nor an array.
QJsonValue set(const QJsonValue& root, const QString& pointer, const QJsonValue& value);
// True when `ancestor` is `pointer` or one of its ancestors ("/a" of "/a/b", not of "/ab").
bool covers(const QString& ancestor, const QString& pointer);
// A JsonDoc as a QJsonValue (Undefined for the absent value) and back (canonical; absent for Undefined).
QJsonValue fromDoc(const JsonDoc& doc);
JsonDoc toDoc(const QJsonValue& value);
} // namespace jsonptr

// A component body schema (schemas/components/<type>-<version>.schema.json) with "#/definitions" resolved.
class SchemaTree {
public:
    SchemaTree() = default;
    explicit SchemaTree(const QJsonObject& schema);
    static SchemaTree fromDoc(const JsonDoc& schema);
    bool isNull() const noexcept { return root_.isEmpty(); }
    // The schema node of an instance pointer ({} when the schema does not allow it): $ref resolved; for a
    // oneOf/anyOf the first non-null alternative that allows the rest of the pointer.
    QJsonObject node(const QString& pointer) const;
    // The node with $ref resolved (one level, then again until no $ref is left).
    QJsonObject resolve(const QJsonObject& node) const;
    // Every leaf property pointer (objects descended, array items as "/0", alternatives merged, in schema
    // member order sorted by name).
    QStringList leafPointers() const;
    // The enum strings of a node ({} when it has none).
    static QStringList enumValues(const QJsonObject& node);
    // Validates `value` against `node` (type, enum, minimum/maximum and the exclusive forms, minLength,
    // pattern, minItems/maxItems/uniqueItems, required and closed members, alternatives). False with a
    // short reason ("must be at least 0.5", "must be one of ...", "/0: required").
    bool validate(const QJsonObject& node, const QJsonValue& value, QString* reason) const;

private:
    void collect(const QJsonObject& node, const QString& prefix, QStringList& out, int depth) const;
    QJsonObject root_;
    QJsonObject definitions_;
};

// A number field that keeps the exact value it was given until it is edited (shortest round-trip text).
class NumberEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit NumberEdit(QWidget* parent = nullptr);
    std::optional<double> value() const; // nullopt: empty or not a number
    void setValue(std::optional<double> v);
    bool hasText() const;

Q_SIGNALS:
    void valueEdited();

private:
    std::optional<double> exact_; // the value given to setValue while the text is unchanged
    QString exactText_;
};

// "#rrggbb" or "#rrggbbaa" (lower case on output).
class ColourEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit ColourEdit(QWidget* parent = nullptr);
    static bool isValidColour(const QString& text);
};

// The base of every subtree editor.
class FieldEditor : public QWidget {
    Q_OBJECT
public:
    explicit FieldEditor(QWidget* parent = nullptr);
    ~FieldEditor() override;
    // The subtree; QJsonValue::Undefined when the member is absent.
    virtual QJsonValue value() const = 0;
    virtual void setValue(const QJsonValue& value) = 0;
    // The leaf widget editing `relativePointer` ("" = this editor's own leaf, if it is one), creating missing
    // records and switching optional members on. nullptr when the pointer is not editable here.
    virtual QWidget* leafEditor(const QString& relativePointer);
    // The subtree's own checks beyond the schema (e.g. a number field holding text). True by default.
    virtual bool validate(QString* reason) const;

Q_SIGNALS:
    void edited();
};

// A persisted number with its unit: {"v": number, "unit": symbol} (D-010).
class UnitValueEdit final : public FieldEditor {
    Q_OBJECT
public:
    explicit UnitValueEdit(QWidget* parent = nullptr);
    QJsonValue value() const override;
    void setValue(const QJsonValue& value) override;
    QWidget* leafEditor(const QString& relativePointer) override;
    bool validate(QString* reason) const override;
    NumberEdit& number() const noexcept { return *number_; }
    QLineEdit& unit() const noexcept { return *unit_; }

private:
    NumberEdit* number_ = nullptr;
    QLineEdit* unit_ = nullptr;
    QJsonObject loaded_; // members other than v and unit are kept
};

// One row of a FieldForm: a pointer relative to the form's object, the kind, the label, and for a subtree
// kind the factory of its editor.
struct FieldRow {
    QString pointer;
    FieldKind kind = FieldKind::Text;
    QString label;
    QStringList enumValues;
    std::function<FieldEditor*(QWidget*)>
        make; // UnitValue (default UnitValueEdit), BandAxis, ColourBands, Custom
};

// An object as a form: its rows bound through its own FieldBinder.
class FieldForm final : public FieldEditor {
    Q_OBJECT
public:
    FieldForm(std::vector<FieldRow> rows, const SchemaTree* schema, const QString& schemaPrefix,
              QWidget* parent = nullptr);
    ~FieldForm() override;
    QJsonValue value() const override;
    void setValue(const QJsonValue& value) override;
    QWidget* leafEditor(const QString& relativePointer) override;
    bool validate(QString* reason) const override;
    FieldBinder& binder() const noexcept { return *binder_; }

private:
    FieldBinder* binder_ = nullptr;
    QJsonValue loaded_;
};

// An array of records (an item editor built per array) or of scalars (the item editor is a single leaf).
class RecordListEditor final : public FieldEditor {
    Q_OBJECT
public:
    using MakeItemEditor = std::function<FieldEditor*(QWidget*)>;
    // `newItem` is appended by Add; `title` names a row of the list (nullptr: the first string member
    // among name, channel, yChannel, else "Item n").
    RecordListEditor(MakeItemEditor makeItem, QJsonValue newItem,
                     std::function<QString(const QJsonValue&, int)> title = {}, QWidget* parent = nullptr);
    ~RecordListEditor() override;
    QJsonValue value() const override;
    void setValue(const QJsonValue& value) override;
    QWidget* leafEditor(const QString& relativePointer) override;
    bool validate(QString* reason) const override;

    int count() const;
    int currentIndex() const;
    void setCurrentIndex(int index);
    void addRecord();
    void removeCurrent();
    void moveCurrent(int delta);
    // The index of the first record whose `member` equals `text` (-1: none).
    int find(const QString& member, const QString& text) const;
    FieldEditor& itemEditor() const noexcept { return *item_; }
    QListWidget& list() const noexcept { return *list_; }

private:
    void flush() const; // the item editor's value into items_[current]
    void rebuildList();
    void loadCurrent();
    QString titleOf(int index) const;

    MakeItemEditor makeItem_;
    QJsonValue newItem_;
    std::function<QString(const QJsonValue&, int)> title_;
    QListWidget* list_ = nullptr;
    FieldEditor* item_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* up_ = nullptr;
    QPushButton* down_ = nullptr;
    mutable QJsonArray items_;
    bool present_ = false; // the array existed at load (or a record was added)
    int current_ = -1;
    int loading_ = 0;
};

// One scalar leaf as a FieldEditor (the item editor of a scalar list).
class LeafEditor final : public FieldEditor {
    Q_OBJECT
public:
    LeafEditor(FieldKind kind, const QStringList& enumValues, QWidget* parent = nullptr);
    QJsonValue value() const override;
    void setValue(const QJsonValue& value) override;
    QWidget* leafEditor(const QString& relativePointer) override;
    QWidget& widget() const noexcept { return *widget_; }

private:
    FieldKind kind_;
    QWidget* widget_ = nullptr;
};

// A fixed-length array of numbers or integers ([top, bottom] edges, density bins). A number field left empty
// takes its entry of `defaults` once another one is set (an absent array stays absent until one is).
class FixedArrayEdit final : public FieldEditor {
    Q_OBJECT
public:
    FixedArrayEdit(FieldKind kind, int count, const QStringList& labels, QWidget* parent = nullptr,
                   const QList<double>& defaults = {});
    QJsonValue value() const override;
    void setValue(const QJsonValue& value) override;
    QWidget* leafEditor(const QString& relativePointer) override;
    bool validate(QString* reason) const override;

private:
    FieldKind kind_;
    std::vector<QWidget*> fields_;
    QList<double> defaults_;
    bool present_ = false;
};

// A member that may be null (schema oneOf [null, X]): a check box "set" and the inner editor.
class OptionalEditor final : public FieldEditor {
    Q_OBJECT
public:
    OptionalEditor(const QString& setLabel, FieldEditor* inner, QJsonValue defaultValue,
                   QWidget* parent = nullptr);
    QJsonValue value() const override;
    void setValue(const QJsonValue& value) override;
    QWidget* leafEditor(const QString& relativePointer) override;
    bool validate(QString* reason) const override;
    QCheckBox& enabledBox() const noexcept { return *set_; }
    FieldEditor& inner() const noexcept { return *inner_; }

private:
    QCheckBox* set_ = nullptr;
    FieldEditor* inner_ = nullptr;
    QJsonValue default_;
    bool absent_ = true; // absent at load and not switched on since
};

// The leaf widget of a leaf kind (Bool, Enum, Integer, Number, Text, Colour, Channel); `node` gives the enum
// values and the integer range when the schema has them.
QWidget* makeLeafEditor(FieldKind kind, const QStringList& enumValues, const QJsonObject& node,
                        QWidget* parent);
// A leaf widget's value (Undefined: an empty Number, an Enum without a choice) and its setter.
QJsonValue leafValue(FieldKind kind, const QWidget* editor);
void setLeafValue(FieldKind kind, QWidget* editor, const QJsonValue& value);
// The names a Channel field offers (set by the PropertiesDialog for its component's sources).
// Replaces the suggestions of a heading picker (a Text leaf made with suggestions: report headings, range
// groups), keeping its text; range-group pickers keep only the "Outings..." paths (D-203 (y)).
void setHeadingChoices(QWidget* headingEditor, const QStringList& headings);
void setChannelChoices(QWidget* channelEditor, const QStringList& names);
// Marks a field invalid (red frame, the reason as tool tip) or valid again.
void markInvalid(QWidget* editor, const QString& reason);

} // namespace esses::widgets
