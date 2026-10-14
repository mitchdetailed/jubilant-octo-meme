// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The FieldBinder editors (06 section 12 items 4 and 6, AC-M3-08). Owner: src/properties_shell/.

#include <esses/widgets/properties_shell/field_editors.hpp>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <climits>
#include <cmath>
#include <utility>

namespace esses::widgets {

namespace {

bool isSubtreeKind(FieldKind kind) {
    return kind == FieldKind::UnitValue || kind == FieldKind::BandAxis || kind == FieldKind::ColourBands ||
           kind == FieldKind::Custom;
}

const QRegularExpression& colourPattern() {
    static const QRegularExpression re(QStringLiteral("^#[0-9A-Fa-f]{6}([0-9A-Fa-f]{2})?$"));
    return re;
}

int boundOr(const QJsonObject& node, const char* name, int fallback) {
    const QJsonValue v = node.value(QLatin1String(name));
    if (!v.isDouble()) {
        return fallback;
    }
    const double d = v.toDouble();
    if (d <= static_cast<double>(INT_MIN)) {
        return INT_MIN;
    }
    if (d >= static_cast<double>(INT_MAX)) {
        return INT_MAX;
    }
    return static_cast<int>(std::lround(d));
}

} // namespace

// ---- NumberEdit, ColourEdit -------------------------------------------------------------------------------

NumberEdit::NumberEdit(QWidget* parent) : QLineEdit(parent) {
    setObjectName(QStringLiteral("numberEdit"));
    connect(this, &QLineEdit::textEdited, this, &NumberEdit::valueEdited);
}

std::optional<double> NumberEdit::value() const {
    const QString t = text().trimmed();
    if (t == exactText_) {
        return exact_;
    }
    if (t.isEmpty()) {
        return std::nullopt;
    }
    bool ok = false;
    const double v = QLocale::c().toDouble(t, &ok);
    if (!ok || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

void NumberEdit::setValue(std::optional<double> v) {
    exact_ = v;
    exactText_ = v ? QString::number(*v, 'g', QLocale::FloatingPointShortest) : QString();
    setText(exactText_);
}

bool NumberEdit::hasText() const {
    return !text().trimmed().isEmpty();
}

ColourEdit::ColourEdit(QWidget* parent) : QLineEdit(parent) {
    setObjectName(QStringLiteral("colourEdit"));
    setMaxLength(9);
    setPlaceholderText(QStringLiteral("#rrggbb"));
}

bool ColourEdit::isValidColour(const QString& text) {
    return colourPattern().match(text).hasMatch();
}

// ---- leaf helpers -----------------------------------------------------------------------------------------

QWidget* makeLeafEditor(FieldKind kind, const QStringList& enumValues, const QJsonObject& node,
                        QWidget* parent) {
    switch (kind) {
    case FieldKind::Bool:
        return new QCheckBox(parent);
    case FieldKind::Enum: {
        auto* c = new QComboBox(parent);
        c->addItems(enumValues.isEmpty() ? SchemaTree::enumValues(node) : enumValues);
        c->setCurrentIndex(-1);
        return c;
    }
    case FieldKind::Integer: {
        auto* s = new QSpinBox(parent);
        const int lo = boundOr(node, "minimum", -1000000000);
        const int hi = boundOr(node, "maximum", 1000000000);
        s->setRange(lo, std::max(lo, hi));
        return s;
    }
    case FieldKind::Number:
        return new NumberEdit(parent);
    case FieldKind::Colour:
        return new ColourEdit(parent);
    case FieldKind::Channel: {
        auto* c = new QComboBox(parent);
        c->setEditable(true);
        c->setInsertPolicy(QComboBox::NoInsert);
        c->setProperty("esses.channelPicker", true);
        return c;
    }
    case FieldKind::Text:
    case FieldKind::UnitValue:
    case FieldKind::BandAxis:
    case FieldKind::ColourBands:
    case FieldKind::Custom:
        break;
    }
    return new QLineEdit(parent);
}

QJsonValue leafValue(FieldKind kind, const QWidget* editor) {
    switch (kind) {
    case FieldKind::Bool:
        if (const auto* b = qobject_cast<const QCheckBox*>(editor)) {
            return b->isChecked();
        }
        break;
    case FieldKind::Enum:
        if (const auto* c = qobject_cast<const QComboBox*>(editor)) {
            return c->currentIndex() < 0 ? QJsonValue(QJsonValue::Undefined) : QJsonValue(c->currentText());
        }
        break;
    case FieldKind::Integer:
        if (const auto* s = qobject_cast<const QSpinBox*>(editor)) {
            return s->value();
        }
        break;
    case FieldKind::Number:
        if (const auto* n = qobject_cast<const NumberEdit*>(editor)) {
            const std::optional<double> v = n->value();
            return v ? QJsonValue(*v) : QJsonValue(QJsonValue::Undefined);
        }
        break;
    case FieldKind::Colour:
        if (const auto* e = qobject_cast<const QLineEdit*>(editor)) {
            const QString t = e->text().trimmed();
            if (t.isEmpty()) {
                return QJsonValue(QJsonValue::Undefined);
            }
            return ColourEdit::isValidColour(t) ? t.toLower() : t;
        }
        break;
    case FieldKind::Channel:
        if (const auto* c = qobject_cast<const QComboBox*>(editor)) {
            return c->currentText();
        }
        break;
    case FieldKind::Text:
        if (const auto* e = qobject_cast<const QLineEdit*>(editor)) {
            return e->text();
        }
        break;
    case FieldKind::UnitValue:
    case FieldKind::BandAxis:
    case FieldKind::ColourBands:
    case FieldKind::Custom:
        if (const auto* fe = qobject_cast<const FieldEditor*>(editor)) {
            return fe->value();
        }
        break;
    }
    return QJsonValue(QJsonValue::Undefined);
}

void setLeafValue(FieldKind kind, QWidget* editor, const QJsonValue& value) {
    switch (kind) {
    case FieldKind::Bool:
        if (auto* b = qobject_cast<QCheckBox*>(editor)) {
            b->setChecked(value.toBool(false));
        }
        return;
    case FieldKind::Enum:
        if (auto* c = qobject_cast<QComboBox*>(editor)) {
            c->setCurrentIndex(value.isString() ? c->findText(value.toString()) : -1);
        }
        return;
    case FieldKind::Integer:
        if (auto* s = qobject_cast<QSpinBox*>(editor)) {
            s->setValue(value.isDouble() ? static_cast<int>(std::lround(value.toDouble()))
                                         : std::clamp(0, s->minimum(), s->maximum()));
        }
        return;
    case FieldKind::Number:
        if (auto* n = qobject_cast<NumberEdit*>(editor)) {
            n->setValue(value.isDouble() ? std::optional<double>(value.toDouble()) : std::nullopt);
        }
        return;
    case FieldKind::Channel:
        if (auto* c = qobject_cast<QComboBox*>(editor)) {
            c->setEditText(value.toString());
        }
        return;
    case FieldKind::Colour:
    case FieldKind::Text:
        if (auto* e = qobject_cast<QLineEdit*>(editor)) {
            e->setText(value.toString());
        }
        return;
    case FieldKind::UnitValue:
    case FieldKind::BandAxis:
    case FieldKind::ColourBands:
    case FieldKind::Custom:
        if (auto* fe = qobject_cast<FieldEditor*>(editor)) {
            fe->setValue(value);
        }
        return;
    }
}

void setChannelChoices(QWidget* channelEditor, const QStringList& names) {
    auto* c = qobject_cast<QComboBox*>(channelEditor);
    if (c == nullptr || !c->isEditable() || !c->property("esses.channelPicker").toBool()) {
        return;
    }
    const QString text = c->currentText();
    const QSignalBlocker block(c);
    c->clear();
    c->addItems(names);
    c->setEditText(text);
}

void markInvalid(QWidget* editor, const QString& reason) {
    if (editor == nullptr) {
        return;
    }
    const bool invalid = !reason.isEmpty();
    if (editor->property("esses.invalid").toBool() == invalid && editor->toolTip() == reason) {
        return;
    }
    editor->setProperty("esses.invalid", invalid);
    editor->setToolTip(reason);
    // Only the leaf widgets get the red frame: a style sheet on a container would cascade to its children.
    const bool leaf =
        qobject_cast<QLineEdit*>(editor) != nullptr || qobject_cast<QComboBox*>(editor) != nullptr ||
        qobject_cast<QSpinBox*>(editor) != nullptr || qobject_cast<QCheckBox*>(editor) != nullptr;
    if (leaf) {
        editor->setStyleSheet(invalid ? QStringLiteral("border: 1px solid #d03030;") : QString());
    }
}

// ---- FieldEditor ------------------------------------------------------------------------------------------

FieldEditor::FieldEditor(QWidget* parent) : QWidget(parent) {}

FieldEditor::~FieldEditor() = default;

QWidget* FieldEditor::leafEditor(const QString&) {
    return nullptr;
}

bool FieldEditor::validate(QString*) const {
    return true;
}

// ---- UnitValueEdit ----------------------------------------------------------------------------------------

UnitValueEdit::UnitValueEdit(QWidget* parent)
    : FieldEditor(parent), number_(new NumberEdit(this)), unit_(new QLineEdit(this)) {
    setObjectName(QStringLiteral("unitValueEdit"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(number_, 2);
    row->addWidget(unit_, 1);
    unit_->setPlaceholderText(tr("unit"));
    unit_->setObjectName(QStringLiteral("unitEdit"));
    connect(number_, &QLineEdit::textChanged, this, &FieldEditor::edited);
    connect(unit_, &QLineEdit::textChanged, this, &FieldEditor::edited);
}

QJsonValue UnitValueEdit::value() const {
    const std::optional<double> v = number_->value();
    if (!v && !number_->hasText() && unit_->text().isEmpty() && loaded_.isEmpty()) {
        return QJsonValue(QJsonValue::Undefined);
    }
    QJsonObject o = loaded_;
    if (v) {
        o.insert(QStringLiteral("v"), *v);
    } else {
        o.remove(QStringLiteral("v"));
    }
    o.insert(QStringLiteral("unit"), unit_->text());
    return o;
}

void UnitValueEdit::setValue(const QJsonValue& value) {
    const QSignalBlocker b1(number_);
    const QSignalBlocker b2(unit_);
    loaded_ = value.toObject();
    const QJsonValue v = loaded_.value(QStringLiteral("v"));
    number_->setValue(v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt);
    unit_->setText(loaded_.value(QStringLiteral("unit")).toString());
}

QWidget* UnitValueEdit::leafEditor(const QString& relativePointer) {
    if (relativePointer.isEmpty() || relativePointer == QLatin1String("/v")) {
        return number_;
    }
    if (relativePointer == QLatin1String("/unit")) {
        return unit_;
    }
    return nullptr;
}

bool UnitValueEdit::validate(QString* reason) const {
    const bool bad = number_->hasText() && !number_->value();
    markInvalid(number_, bad ? tr("must be a number") : QString());
    if (bad && reason != nullptr) {
        *reason = tr("the value must be a number");
    }
    return !bad;
}

// ---- FieldForm --------------------------------------------------------------------------------------------

FieldForm::FieldForm(std::vector<FieldRow> rows, const SchemaTree* schema, const QString& schemaPrefix,
                     QWidget* parent)
    : FieldEditor(parent), binder_(new FieldBinder(this)) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    binder_->setSchema(schema, schemaPrefix);
    for (FieldRow& r : rows) {
        const QJsonObject node = schema != nullptr ? schema->node(schemaPrefix + r.pointer) : QJsonObject();
        QWidget* editor = nullptr;
        if (isSubtreeKind(r.kind)) {
            editor =
                r.make ? r.make(this) : (r.kind == FieldKind::UnitValue ? new UnitValueEdit(this) : nullptr);
        } else {
            editor = makeLeafEditor(r.kind, r.enumValues, node, this);
        }
        if (editor == nullptr) {
            continue;
        }
        editor->setObjectName(r.pointer);
        form->addRow(r.label, editor);
        const QStringList values = r.enumValues.isEmpty() ? SchemaTree::enumValues(node) : r.enumValues;
        binder_->bind(r.pointer, r.kind, editor, r.label, values);
    }
    connect(binder_, &FieldBinder::edited, this, &FieldEditor::edited);
}

FieldForm::~FieldForm() = default;

QJsonValue FieldForm::value() const {
    if (loaded_.isUndefined() && !binder_->isModified()) {
        return QJsonValue(QJsonValue::Undefined);
    }
    return binder_->storeJson(loaded_.isObject() ? loaded_ : QJsonValue(QJsonObject()));
}

void FieldForm::setValue(const QJsonValue& value) {
    loaded_ = value.isObject() ? value : QJsonValue(QJsonValue::Undefined);
    binder_->loadJson(value.isObject() ? value : QJsonValue(QJsonObject()));
}

QWidget* FieldForm::leafEditor(const QString& relativePointer) {
    return binder_->editorFor(relativePointer);
}

bool FieldForm::validate(QString* reason) const {
    return binder_->validate(reason);
}

// ---- LeafEditor -------------------------------------------------------------------------------------------

LeafEditor::LeafEditor(FieldKind kind, const QStringList& enumValues, QWidget* parent)
    : FieldEditor(parent), kind_(kind), widget_(makeLeafEditor(kind, enumValues, {}, this)) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(widget_);
    if (auto* b = qobject_cast<QCheckBox*>(widget_)) {
        connect(b, &QCheckBox::toggled, this, &FieldEditor::edited);
    } else if (auto* c = qobject_cast<QComboBox*>(widget_)) {
        connect(c, &QComboBox::currentIndexChanged, this, &FieldEditor::edited);
        connect(c, &QComboBox::editTextChanged, this, &FieldEditor::edited);
    } else if (auto* s = qobject_cast<QSpinBox*>(widget_)) {
        connect(s, &QSpinBox::valueChanged, this, &FieldEditor::edited);
    } else if (auto* e = qobject_cast<QLineEdit*>(widget_)) {
        connect(e, &QLineEdit::textChanged, this, &FieldEditor::edited);
    }
}

QJsonValue LeafEditor::value() const {
    return leafValue(kind_, widget_);
}

void LeafEditor::setValue(const QJsonValue& value) {
    const QSignalBlocker block(widget_);
    setLeafValue(kind_, widget_, value);
}

QWidget* LeafEditor::leafEditor(const QString& relativePointer) {
    return relativePointer.isEmpty() ? widget_ : nullptr;
}

// ---- FixedArrayEdit ---------------------------------------------------------------------------------------

FixedArrayEdit::FixedArrayEdit(FieldKind kind, int count, const QStringList& labels, QWidget* parent,
                               const QList<double>& defaults)
    : FieldEditor(parent), kind_(kind), defaults_(defaults) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < count; ++i) {
        if (i < labels.size() && !labels[i].isEmpty()) {
            row->addWidget(new QLabel(labels[i], this));
        }
        QWidget* f = nullptr;
        if (kind == FieldKind::Integer) {
            auto* s = new QSpinBox(this);
            s->setRange(-1000000000, 1000000000);
            connect(s, &QSpinBox::valueChanged, this, &FieldEditor::edited);
            f = s;
        } else {
            auto* n = new NumberEdit(this);
            if (i < defaults_.size()) {
                n->setPlaceholderText(QString::number(defaults_[i], 'g', QLocale::FloatingPointShortest));
            }
            connect(n, &QLineEdit::textChanged, this, &FieldEditor::edited);
            f = n;
        }
        f->setObjectName(QStringLiteral("item%1").arg(i));
        row->addWidget(f, 1);
        fields_.push_back(f);
    }
}

QJsonValue FixedArrayEdit::value() const {
    QJsonArray a;
    bool any = present_;
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        QWidget* f = fields_[i];
        if (auto* s = qobject_cast<QSpinBox*>(f)) {
            a.append(s->value());
        } else if (auto* n = qobject_cast<NumberEdit*>(f)) {
            std::optional<double> v = n->value();
            any = any || n->hasText();
            if (!v && !n->hasText() && static_cast<qsizetype>(i) < defaults_.size()) {
                v = defaults_[static_cast<qsizetype>(i)];
            }
            a.append(v ? QJsonValue(*v) : QJsonValue(QJsonValue::Null));
        }
    }
    if (!any && kind_ != FieldKind::Integer) {
        return QJsonValue(QJsonValue::Undefined);
    }
    return a;
}

void FixedArrayEdit::setValue(const QJsonValue& value) {
    const QJsonArray a = value.toArray();
    present_ = value.isArray();
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        const QSignalBlocker block(fields_[i]);
        const QJsonValue v = static_cast<qsizetype>(i) < a.size() ? a.at(static_cast<qsizetype>(i))
                                                                  : QJsonValue(QJsonValue::Undefined);
        if (auto* s = qobject_cast<QSpinBox*>(fields_[i])) {
            const double fallback =
                static_cast<qsizetype>(i) < defaults_.size() ? defaults_[static_cast<qsizetype>(i)] : 0.0;
            s->setValue(static_cast<int>(std::lround(v.isDouble() ? v.toDouble() : fallback)));
        } else if (auto* n = qobject_cast<NumberEdit*>(fields_[i])) {
            n->setValue(v.isDouble() ? std::optional<double>(v.toDouble()) : std::nullopt);
        }
    }
}

QWidget* FixedArrayEdit::leafEditor(const QString& relativePointer) {
    const QStringList t = jsonptr::split(relativePointer);
    if (t.size() != 1) {
        return nullptr;
    }
    bool ok = false;
    const int i = t.front().toInt(&ok);
    return ok && i >= 0 && i < static_cast<int>(fields_.size()) ? fields_[static_cast<std::size_t>(i)]
                                                                : nullptr;
}

bool FixedArrayEdit::validate(QString* reason) const {
    for (QWidget* f : fields_) {
        if (auto* n = qobject_cast<NumberEdit*>(f); n != nullptr && n->hasText() && !n->value()) {
            if (reason != nullptr) {
                *reason = tr("must be a number");
            }
            return false;
        }
    }
    return true;
}

// ---- OptionalEditor ---------------------------------------------------------------------------------------

OptionalEditor::OptionalEditor(const QString& setLabel, FieldEditor* inner, QJsonValue defaultValue,
                               QWidget* parent)
    : FieldEditor(parent), set_(new QCheckBox(setLabel, this)), inner_(inner),
      default_(std::move(defaultValue)) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->addWidget(set_);
    inner_->setParent(this);
    column->addWidget(inner_);
    inner_->setEnabled(false);
    set_->setObjectName(QStringLiteral("optionalSet"));
    connect(set_, &QCheckBox::toggled, this, [this](bool on) {
        inner_->setEnabled(on);
        if (on && inner_->value().isUndefined()) {
            const QSignalBlocker block(inner_);
            inner_->setValue(default_);
        }
        Q_EMIT edited();
    });
    connect(inner_, &FieldEditor::edited, this, &FieldEditor::edited);
}

QJsonValue OptionalEditor::value() const {
    if (!set_->isChecked()) {
        return absent_ ? QJsonValue(QJsonValue::Undefined) : QJsonValue(QJsonValue::Null);
    }
    const QJsonValue v = inner_->value();
    return v.isUndefined() ? default_ : v;
}

void OptionalEditor::setValue(const QJsonValue& value) {
    absent_ = value.isUndefined();
    const bool on = !(value.isUndefined() || value.isNull());
    {
        const QSignalBlocker block(set_);
        set_->setChecked(on);
    }
    inner_->setEnabled(on);
    const QSignalBlocker block(inner_);
    inner_->setValue(on ? value : QJsonValue(QJsonValue::Undefined));
}

QWidget* OptionalEditor::leafEditor(const QString& relativePointer) {
    if (!set_->isChecked()) {
        set_->setChecked(true);
    }
    return inner_->leafEditor(relativePointer);
}

bool OptionalEditor::validate(QString* reason) const {
    return !set_->isChecked() || inner_->validate(reason);
}

// ---- RecordListEditor -------------------------------------------------------------------------------------

RecordListEditor::RecordListEditor(MakeItemEditor makeItem, QJsonValue newItem,
                                   std::function<QString(const QJsonValue&, int)> title, QWidget* parent)
    : FieldEditor(parent), makeItem_(std::move(makeItem)), newItem_(std::move(newItem)),
      title_(std::move(title)) {
    setObjectName(QStringLiteral("recordList"));
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("records"));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    add_ = new QPushButton(tr("Add"), this);
    remove_ = new QPushButton(tr("Remove"), this);
    up_ = new QPushButton(tr("Up"), this);
    down_ = new QPushButton(tr("Down"), this);
    add_->setObjectName(QStringLiteral("add"));
    remove_->setObjectName(QStringLiteral("remove"));
    up_->setObjectName(QStringLiteral("up"));
    down_->setObjectName(QStringLiteral("down"));
    item_ = makeItem_(this);
    auto* left = new QVBoxLayout;
    left->addWidget(list_, 1);
    auto* buttons = new QHBoxLayout;
    for (QPushButton* b : {add_, remove_, up_, down_}) {
        b->setAutoDefault(false);
        buttons->addWidget(b);
    }
    left->addLayout(buttons);
    auto* columns = new QHBoxLayout(this);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->addLayout(left, 1);
    columns->addWidget(item_, 2);
    list_->setMinimumHeight(60);
    connect(add_, &QPushButton::clicked, this, &RecordListEditor::addRecord);
    connect(remove_, &QPushButton::clicked, this, &RecordListEditor::removeCurrent);
    connect(up_, &QPushButton::clicked, this, [this] { moveCurrent(-1); });
    connect(down_, &QPushButton::clicked, this, [this] { moveCurrent(1); });
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (loading_ > 0 || row == current_) {
            return;
        }
        flush();
        current_ = row;
        loadCurrent();
    });
    connect(item_, &FieldEditor::edited, this, [this] {
        if (loading_ > 0 || current_ < 0 || current_ >= items_.size()) {
            return;
        }
        const QJsonValue v = item_->value();
        if (v.isUndefined()) {
            return;
        }
        items_.replace(current_, v);
        if (QListWidgetItem* it = list_->item(current_)) {
            it->setText(titleOf(current_));
        }
        Q_EMIT edited();
    });
    loadCurrent();
}

RecordListEditor::~RecordListEditor() = default;

QString RecordListEditor::titleOf(int index) const {
    const QJsonValue v = items_.at(index);
    if (title_) {
        return title_(v, index);
    }
    if (v.isString()) {
        return v.toString().isEmpty() ? tr("(none)") : v.toString();
    }
    if (v.isDouble()) {
        return QString::number(v.toDouble(), 'g', QLocale::FloatingPointShortest);
    }
    const QJsonObject o = v.toObject();
    for (const char* key : {"name", "channel", "yChannel"}) {
        const QString s = o.value(QLatin1String(key)).toString();
        if (!s.isEmpty()) {
            return s;
        }
    }
    return tr("Item %1").arg(index + 1);
}

void RecordListEditor::flush() const {
    if (current_ >= 0 && current_ < items_.size()) {
        const QJsonValue v = item_->value();
        if (!v.isUndefined()) {
            items_.replace(current_, v);
        }
    }
}

void RecordListEditor::rebuildList() {
    ++loading_;
    list_->clear();
    for (int i = 0; i < items_.size(); ++i) {
        list_->addItem(titleOf(i));
    }
    --loading_;
}

void RecordListEditor::loadCurrent() {
    ++loading_;
    const bool any = current_ >= 0 && current_ < items_.size();
    item_->setEnabled(any);
    item_->setValue(any ? items_.at(current_) : QJsonValue(QJsonValue::Undefined));
    list_->setCurrentRow(any ? current_ : -1);
    remove_->setEnabled(any);
    up_->setEnabled(any && current_ > 0);
    down_->setEnabled(any && current_ + 1 < items_.size());
    --loading_;
}

QJsonValue RecordListEditor::value() const {
    flush();
    if (!present_ && items_.isEmpty()) {
        return QJsonValue(QJsonValue::Undefined);
    }
    return items_;
}

void RecordListEditor::setValue(const QJsonValue& value) {
    items_ = value.toArray();
    present_ = value.isArray();
    current_ = items_.isEmpty() ? -1 : 0;
    rebuildList();
    loadCurrent();
}

int RecordListEditor::count() const {
    return static_cast<int>(items_.size());
}

int RecordListEditor::currentIndex() const {
    return current_;
}

void RecordListEditor::setCurrentIndex(int index) {
    if (index < -1 || index >= items_.size() || index == current_) {
        return;
    }
    flush();
    current_ = index;
    loadCurrent();
}

void RecordListEditor::addRecord() {
    flush();
    items_.append(newItem_);
    present_ = true;
    rebuildList();
    current_ = static_cast<int>(items_.size()) - 1;
    loadCurrent();
    Q_EMIT edited();
}

void RecordListEditor::removeCurrent() {
    if (current_ < 0 || current_ >= items_.size()) {
        return;
    }
    items_.removeAt(current_);
    rebuildList();
    current_ = std::min(current_, static_cast<int>(items_.size()) - 1);
    loadCurrent();
    Q_EMIT edited();
}

void RecordListEditor::moveCurrent(int delta) {
    flush();
    const int to = current_ + delta;
    if (current_ < 0 || to < 0 || to >= items_.size()) {
        return;
    }
    const QJsonValue v = items_.at(current_);
    items_.removeAt(current_);
    items_.insert(to, v);
    rebuildList();
    current_ = to;
    loadCurrent();
    Q_EMIT edited();
}

int RecordListEditor::find(const QString& member, const QString& text) const {
    flush();
    for (int i = 0; i < items_.size(); ++i) {
        const QJsonValue v = items_.at(i);
        if ((member.isEmpty() && v.toString() == text) || v.toObject().value(member).toString() == text) {
            return i;
        }
    }
    return -1;
}

QWidget* RecordListEditor::leafEditor(const QString& relativePointer) {
    const QStringList tokens = jsonptr::split(relativePointer);
    if (tokens.isEmpty()) {
        return nullptr;
    }
    bool ok = false;
    const int index = tokens.front().toInt(&ok);
    if (!ok || index < 0 || index > 1000) {
        return nullptr;
    }
    while (count() <= index) {
        addRecord();
    }
    setCurrentIndex(index);
    return item_->leafEditor(jsonptr::join(tokens.mid(1)));
}

bool RecordListEditor::validate(QString* reason) const {
    QString why;
    if (!item_->validate(&why)) {
        if (reason != nullptr) {
            *reason = tr("%1: %2").arg(titleOf(current_), why);
        }
        return false;
    }
    return true;
}

} // namespace esses::widgets
