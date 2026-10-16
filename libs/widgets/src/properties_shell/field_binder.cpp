// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// FieldBinder (06 section 12 item 6, AC-M3-08), the RFC 6901 pointer helpers and SchemaTree. Owner:
// src/properties_shell/.

#include <esses/widgets/properties_shell/field_binder.hpp>
#include <esses/widgets/properties_shell/field_editors.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLocale>
#include <QRegularExpression>
#include <QSpinBox>
#include <algorithm>
#include <cmath>

namespace esses::widgets {

// ---- JSON pointers ----------------------------------------------------------------------------------------

namespace jsonptr {

QStringList split(const QString& pointer) {
    QStringList out;
    if (pointer.isEmpty()) {
        return out;
    }
    const QStringList parts =
        pointer.mid(pointer.startsWith(QLatin1Char('/')) ? 1 : 0).split(QLatin1Char('/'));
    out.reserve(parts.size());
    for (QString p : parts) {
        p.replace(QStringLiteral("~1"), QStringLiteral("/"));
        p.replace(QStringLiteral("~0"), QStringLiteral("~"));
        out.push_back(p);
    }
    return out;
}

QString join(const QStringList& tokens) {
    QString out;
    for (QString t : tokens) {
        t.replace(QStringLiteral("~"), QStringLiteral("~0"));
        t.replace(QStringLiteral("/"), QStringLiteral("~1"));
        out += QLatin1Char('/') + t;
    }
    return out;
}

QJsonValue get(const QJsonValue& root, const QString& pointer) {
    QJsonValue node = root;
    for (const QString& t : split(pointer)) {
        if (node.isObject()) {
            const QJsonObject o = node.toObject();
            if (!o.contains(t)) {
                return QJsonValue(QJsonValue::Undefined);
            }
            node = o.value(t);
        } else if (node.isArray()) {
            bool ok = false;
            const int i = t.toInt(&ok);
            const QJsonArray a = node.toArray();
            if (!ok || i < 0 || i >= a.size()) {
                return QJsonValue(QJsonValue::Undefined);
            }
            node = a.at(i);
        } else {
            return QJsonValue(QJsonValue::Undefined);
        }
    }
    return node;
}

namespace {

QJsonValue setAt(const QJsonValue& node, const QStringList& tokens, qsizetype i, const QJsonValue& value) {
    if (i == tokens.size()) {
        return value;
    }
    const QString& t = tokens[i];
    const bool last = i + 1 == tokens.size();
    if (node.isArray()) {
        QJsonArray a = node.toArray();
        bool ok = false;
        const int idx = t.toInt(&ok);
        if (!ok || idx < 0 || idx > a.size()) {
            return node;
        }
        if (idx == a.size()) {
            if (value.isUndefined() && last) {
                return node;
            }
            a.append(setAt(QJsonValue(QJsonValue::Undefined), tokens, i + 1, value));
            return a;
        }
        const QJsonValue child = setAt(a.at(idx), tokens, i + 1, value);
        if (child.isUndefined()) {
            a.removeAt(idx);
        } else {
            a.replace(idx, child);
        }
        return a;
    }
    if (node.isObject() || node.isUndefined() || node.isNull()) {
        QJsonObject o = node.toObject();
        const QJsonValue child =
            setAt(o.contains(t) ? o.value(t) : QJsonValue(QJsonValue::Undefined), tokens, i + 1, value);
        if (child.isUndefined()) {
            o.remove(t);
        } else {
            o.insert(t, child);
        }
        return o;
    }
    return node;
}

} // namespace

QJsonValue set(const QJsonValue& root, const QString& pointer, const QJsonValue& value) {
    return setAt(root, split(pointer), 0, value);
}

bool covers(const QString& ancestor, const QString& pointer) {
    return pointer == ancestor || ancestor.isEmpty() || pointer.startsWith(ancestor + QLatin1Char('/'));
}

QJsonValue fromDoc(const JsonDoc& doc) {
    if (doc.isNull()) {
        return QJsonValue(QJsonValue::Undefined);
    }
    const QByteArray text = QByteArray::fromStdString(doc.compact());
    if (doc.isObject()) {
        return QJsonDocument::fromJson(text).object();
    }
    if (doc.isArray()) {
        return QJsonDocument::fromJson(text).array();
    }
    const QJsonArray wrapped = QJsonDocument::fromJson("[" + text + "]").array();
    return wrapped.isEmpty() ? QJsonValue(QJsonValue::Undefined) : wrapped.first();
}

JsonDoc toDoc(const QJsonValue& v) {
    QByteArray text;
    if (v.isObject()) {
        text = QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
    } else if (v.isArray()) {
        text = QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
    } else if (v.isUndefined()) {
        return {};
    } else {
        text = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
        text = text.mid(1, text.size() - 2);
    }
    Result<JsonDoc> doc = JsonDoc::parse(text.toStdString());
    return doc ? std::move(doc).value() : JsonDoc();
}

} // namespace jsonptr

// ---- SchemaTree -------------------------------------------------------------------------------------------

namespace {

constexpr int kMaxDepth = 32;

const QStringList& alternativeKeys() {
    static const QStringList keys{QStringLiteral("oneOf"), QStringLiteral("anyOf")};
    return keys;
}

bool isNullOnly(const QJsonObject& node) {
    return node.value(QStringLiteral("type")).toString() == QLatin1String("null");
}

QJsonObject nodeAt(const SchemaTree& tree, const QJsonObject& raw, const QStringList& tokens, qsizetype i,
                   int depth) {
    if (depth > kMaxDepth) {
        return {};
    }
    const QJsonObject node = tree.resolve(raw);
    if (i == tokens.size()) {
        return node;
    }
    for (const QString& key : alternativeKeys()) {
        if (node.contains(key)) {
            for (const QJsonValue& alt : node.value(key).toArray()) {
                const QJsonObject a = tree.resolve(alt.toObject());
                if (isNullOnly(a)) {
                    continue;
                }
                const QJsonObject r = nodeAt(tree, a, tokens, i, depth + 1);
                if (!r.isEmpty()) {
                    return r;
                }
            }
            return {};
        }
    }
    const QString& t = tokens[i];
    const QJsonObject props = node.value(QStringLiteral("properties")).toObject();
    if (props.contains(t)) {
        return nodeAt(tree, props.value(t).toObject(), tokens, i + 1, depth + 1);
    }
    bool ok = false;
    static_cast<void>(t.toInt(&ok));
    if (ok && node.value(QStringLiteral("items")).isObject()) {
        return nodeAt(tree, node.value(QStringLiteral("items")).toObject(), tokens, i + 1, depth + 1);
    }
    return {};
}

QString typeName(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::Null:
        return QStringLiteral("null");
    case QJsonValue::Bool:
        return QStringLiteral("boolean");
    case QJsonValue::Double:
        return QStringLiteral("number");
    case QJsonValue::String:
        return QStringLiteral("string");
    case QJsonValue::Array:
        return QStringLiteral("array");
    case QJsonValue::Object:
        return QStringLiteral("object");
    case QJsonValue::Undefined:
        break;
    }
    return QStringLiteral("absent");
}

bool typeMatches(const QString& type, const QJsonValue& v) {
    if (type == QLatin1String("integer")) {
        return v.isDouble() && std::isfinite(v.toDouble()) && std::floor(v.toDouble()) == v.toDouble();
    }
    if (type == QLatin1String("number")) {
        return v.isDouble();
    }
    return typeName(v) == type;
}

QString numberText(double v) {
    return QString::number(v, 'g', QLocale::FloatingPointShortest);
}

QString withPrefix(const QString& prefix, const QString& reason) {
    return prefix.isEmpty() ? reason : prefix + QStringLiteral(": ") + reason;
}

QString valueText(const QJsonValue& v) {
    if (v.isString()) {
        return v.toString();
    }
    const QByteArray text = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(text.mid(1, text.size() - 2));
}

} // namespace

SchemaTree::SchemaTree(const QJsonObject& schema)
    : root_(schema), definitions_(schema.value(QStringLiteral("definitions")).toObject()) {}

SchemaTree SchemaTree::fromDoc(const JsonDoc& schema) {
    if (!schema.isObject()) {
        return {};
    }
    return SchemaTree(QJsonDocument::fromJson(QByteArray::fromStdString(schema.compact())).object());
}

QJsonObject SchemaTree::resolve(const QJsonObject& node) const {
    QJsonObject out = node;
    for (int guard = 0; guard < kMaxDepth && out.contains(QStringLiteral("$ref")); ++guard) {
        const QString ref = out.value(QStringLiteral("$ref")).toString();
        const QString prefix = QStringLiteral("#/definitions/");
        if (!ref.startsWith(prefix)) {
            return {};
        }
        // Sibling members of a $ref (a description) are ignored, as draft-07 does.
        out = definitions_.value(ref.mid(prefix.size())).toObject();
    }
    return out;
}

QJsonObject SchemaTree::node(const QString& pointer) const {
    if (root_.isEmpty()) {
        return {};
    }
    return nodeAt(*this, root_, jsonptr::split(pointer), 0, 0);
}

QStringList SchemaTree::enumValues(const QJsonObject& node) {
    QStringList out;
    for (const QJsonValue& v : node.value(QStringLiteral("enum")).toArray()) {
        if (v.isString()) {
            out.push_back(v.toString());
        }
    }
    return out;
}

void SchemaTree::collect(const QJsonObject& raw, const QString& prefix, QStringList& out, int depth) const {
    if (depth > kMaxDepth) {
        return;
    }
    const QJsonObject node = resolve(raw);
    for (const QString& key : alternativeKeys()) {
        if (node.contains(key)) {
            for (const QJsonValue& alt : node.value(key).toArray()) {
                const QJsonObject a = resolve(alt.toObject());
                if (!isNullOnly(a)) {
                    collect(a, prefix, out, depth + 1);
                }
            }
            return;
        }
    }
    const QJsonObject props = node.value(QStringLiteral("properties")).toObject();
    if (!props.isEmpty()) {
        for (auto it = props.begin(); it != props.end(); ++it) {
            collect(it.value().toObject(), prefix + jsonptr::join({it.key()}), out, depth + 1);
        }
        return;
    }
    if (node.value(QStringLiteral("items")).isObject()) {
        collect(node.value(QStringLiteral("items")).toObject(), prefix + QStringLiteral("/0"), out,
                depth + 1);
        return;
    }
    if (!out.contains(prefix)) {
        out.push_back(prefix);
    }
}

QStringList SchemaTree::leafPointers() const {
    QStringList out;
    if (!root_.isEmpty()) {
        collect(root_, QString(), out, 0);
    }
    out.removeAll(QString()); // the body itself is never a property
    return out;
}

bool SchemaTree::validate(const QJsonObject& raw, const QJsonValue& value, QString* reason) const {
    const QJsonObject node = resolve(raw);
    const auto fail = [reason](const QString& why) {
        if (reason != nullptr) {
            *reason = why;
        }
        return false;
    };
    if (node.isEmpty() || value.isUndefined()) {
        return true;
    }
    for (const QString& key : alternativeKeys()) {
        if (node.contains(key)) {
            QString first;
            for (const QJsonValue& alt : node.value(key).toArray()) {
                QString why;
                if (validate(alt.toObject(), value, &why)) {
                    return true;
                }
                if (first.isEmpty() && !isNullOnly(resolve(alt.toObject()))) {
                    first = why;
                }
            }
            return fail(first.isEmpty() ? QStringLiteral("is not an allowed value") : first);
        }
    }
    if (node.contains(QStringLiteral("type"))) {
        const QJsonValue t = node.value(QStringLiteral("type"));
        QStringList types;
        if (t.isArray()) {
            for (const QJsonValue& x : t.toArray()) {
                types.push_back(x.toString());
            }
        } else {
            types.push_back(t.toString());
        }
        if (std::none_of(types.begin(), types.end(),
                         [&](const QString& ty) { return typeMatches(ty, value); })) {
            return fail(types.contains(QStringLiteral("integer")) && value.isDouble()
                            ? QStringLiteral("must be a whole number")
                            : QStringLiteral("must be a %1").arg(types.join(QStringLiteral(" or "))));
        }
    }
    if (node.contains(QStringLiteral("enum"))) {
        const QJsonArray allowed = node.value(QStringLiteral("enum")).toArray();
        if (!allowed.contains(value)) {
            QStringList names;
            for (const QJsonValue& a : allowed) {
                names.push_back(valueText(a));
            }
            return fail(QStringLiteral("must be one of %1").arg(names.join(QStringLiteral(", "))));
        }
    }
    if (value.isDouble()) {
        const double v = value.toDouble();
        const auto bound = [&node](const char* name) { return node.value(QLatin1String(name)).toDouble(); };
        if (node.contains(QStringLiteral("minimum")) && v < bound("minimum")) {
            return fail(QStringLiteral("must be at least %1").arg(numberText(bound("minimum"))));
        }
        if (node.contains(QStringLiteral("maximum")) && v > bound("maximum")) {
            return fail(QStringLiteral("must be at most %1").arg(numberText(bound("maximum"))));
        }
        if (node.contains(QStringLiteral("exclusiveMinimum")) && v <= bound("exclusiveMinimum")) {
            return fail(QStringLiteral("must be more than %1").arg(numberText(bound("exclusiveMinimum"))));
        }
        if (node.contains(QStringLiteral("exclusiveMaximum")) && v >= bound("exclusiveMaximum")) {
            return fail(QStringLiteral("must be less than %1").arg(numberText(bound("exclusiveMaximum"))));
        }
    }
    if (value.isString()) {
        const QString s = value.toString();
        if (node.contains(QStringLiteral("minLength")) &&
            s.size() < node.value(QStringLiteral("minLength")).toInt()) {
            return fail(QStringLiteral("must not be empty"));
        }
        if (node.contains(QStringLiteral("pattern"))) {
            const QString pattern = node.value(QStringLiteral("pattern")).toString();
            const QRegularExpression re(pattern);
            if (re.isValid() && !re.match(s).hasMatch()) {
                return fail(pattern.contains(QLatin1Char('#'))
                                ? QStringLiteral("must be a colour #rrggbb or #rrggbbaa")
                                : QStringLiteral("must not be blank or start or end with a space"));
            }
        }
    }
    if (value.isArray()) {
        const QJsonArray a = value.toArray();
        if (node.contains(QStringLiteral("minItems")) &&
            a.size() < node.value(QStringLiteral("minItems")).toInt()) {
            return fail(QStringLiteral("needs at least %1 entries")
                            .arg(node.value(QStringLiteral("minItems")).toInt()));
        }
        if (node.contains(QStringLiteral("maxItems")) &&
            a.size() > node.value(QStringLiteral("maxItems")).toInt()) {
            return fail(QStringLiteral("takes at most %1 entries")
                            .arg(node.value(QStringLiteral("maxItems")).toInt()));
        }
        if (node.value(QStringLiteral("uniqueItems")).toBool()) {
            for (qsizetype i = 0; i < a.size(); ++i) {
                for (qsizetype j = i + 1; j < a.size(); ++j) {
                    if (a.at(i) == a.at(j)) {
                        return fail(QStringLiteral("lists %1 twice").arg(valueText(a.at(i))));
                    }
                }
            }
        }
        if (node.value(QStringLiteral("items")).isObject()) {
            const QJsonObject items = node.value(QStringLiteral("items")).toObject();
            for (qsizetype i = 0; i < a.size(); ++i) {
                QString why;
                if (!validate(items, a.at(i), &why)) {
                    return fail(withPrefix(QStringLiteral("entry %1").arg(i + 1), why));
                }
            }
        }
    }
    if (value.isObject()) {
        const QJsonObject o = value.toObject();
        for (const QJsonValue& r : node.value(QStringLiteral("required")).toArray()) {
            if (!o.contains(r.toString())) {
                return fail(QStringLiteral("%1 is required").arg(r.toString()));
            }
        }
        const QJsonObject props = node.value(QStringLiteral("properties")).toObject();
        const bool closed = node.value(QStringLiteral("additionalProperties")) == QJsonValue(false);
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (props.contains(it.key())) {
                QString why;
                if (!validate(props.value(it.key()).toObject(), it.value(), &why)) {
                    return fail(withPrefix(it.key(), why));
                }
            } else if (closed) {
                return fail(QStringLiteral("%1 is not a known member").arg(it.key()));
            }
        }
    }
    return true;
}

// ---- FieldBinder ------------------------------------------------------------------------------------------

namespace {

bool isSubtreeKind(FieldKind kind) {
    return kind == FieldKind::UnitValue || kind == FieldKind::BandAxis || kind == FieldKind::ColourBands ||
           kind == FieldKind::Custom;
}

} // namespace

FieldBinder::FieldBinder(QObject* parent) : QObject(parent) {}

FieldBinder::~FieldBinder() = default;

void FieldBinder::bind(const QString& pointer, FieldKind kind, QWidget* editor, const QString& label,
                       const QStringList& enumValues) {
    bindings_.push_back(FieldBinding{pointer, kind, label, enumValues, editor});
    initial_.emplace_back(QJsonValue::Undefined);
    loaded_.push_back(false);
    watch(bindings_.size() - 1);
}

QStringList FieldBinder::boundPointers() const {
    QStringList out;
    out.reserve(static_cast<qsizetype>(bindings_.size()));
    for (const FieldBinding& b : bindings_) {
        out.push_back(b.pointer);
    }
    return out;
}

bool FieldBinder::isBound(const QString& pointer) const {
    return std::any_of(bindings_.begin(), bindings_.end(), [&](const FieldBinding& b) {
        return pointer == b.pointer || pointer.startsWith(b.pointer + QLatin1Char('/'));
    });
}

void FieldBinder::watch(std::size_t index) {
    QWidget* e = bindings_[index].editor.data();
    if (e == nullptr) {
        return;
    }
    if (auto* fe = qobject_cast<FieldEditor*>(e)) {
        connect(fe, &FieldEditor::edited, this, &FieldBinder::onEdited);
    } else if (auto* box = qobject_cast<QCheckBox*>(e)) {
        connect(box, &QCheckBox::toggled, this, &FieldBinder::onEdited);
    } else if (auto* combo = qobject_cast<QComboBox*>(e)) {
        connect(combo, &QComboBox::currentIndexChanged, this, &FieldBinder::onEdited);
        connect(combo, &QComboBox::editTextChanged, this, &FieldBinder::onEdited);
    } else if (auto* spin = qobject_cast<QSpinBox*>(e)) {
        connect(spin, &QSpinBox::valueChanged, this, &FieldBinder::onEdited);
    } else if (auto* line = qobject_cast<QLineEdit*>(e)) {
        connect(line, &QLineEdit::textChanged, this, &FieldBinder::onEdited);
    }
}

void FieldBinder::onEdited() {
    if (loading_ > 0) {
        return;
    }
    Q_EMIT edited();
    const bool valid = validate(nullptr);
    if (valid != lastValid_) {
        lastValid_ = valid;
        Q_EMIT validityChanged(valid);
    }
}

QJsonValue FieldBinder::editorValue(const FieldBinding& b) const {
    QWidget* e = b.editor.data();
    if (e == nullptr) {
        return QJsonValue(QJsonValue::Undefined);
    }
    if (isSubtreeKind(b.kind)) {
        const auto* fe = qobject_cast<const FieldEditor*>(e);
        return fe == nullptr ? QJsonValue(QJsonValue::Undefined) : fe->value();
    }
    return leafValue(b.kind, e);
}

void FieldBinder::load(const JsonDoc& body) {
    loadJson(jsonptr::fromDoc(body));
}

void FieldBinder::loadJson(const QJsonValue& body) {
    ++loading_;
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        const FieldBinding& b = bindings_[i];
        QWidget* e = b.editor.data();
        if (e == nullptr) {
            continue;
        }
        const QJsonValue v = jsonptr::get(body, b.pointer);
        if (isSubtreeKind(b.kind)) {
            if (auto* fe = qobject_cast<FieldEditor*>(e)) {
                fe->setValue(v);
            }
        } else {
            setLeafValue(b.kind, e, v);
        }
        initial_[i] = editorValue(b);
        loaded_[i] = true;
    }
    --loading_;
    const bool valid = validate(nullptr);
    if (valid != lastValid_) {
        lastValid_ = valid;
        Q_EMIT validityChanged(valid);
    }
}

void FieldBinder::loadUntouchedJson(const QJsonValue& body) {
    ++loading_;
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        const FieldBinding& b = bindings_[i];
        QWidget* e = b.editor.data();
        if (e == nullptr || (loaded_[i] && editorValue(b) != initial_[i])) {
            continue; // the user's edit stays
        }
        const QJsonValue v = jsonptr::get(body, b.pointer);
        if (isSubtreeKind(b.kind)) {
            if (auto* fe = qobject_cast<FieldEditor*>(e)) {
                fe->setValue(v);
            }
        } else {
            setLeafValue(b.kind, e, v);
        }
        initial_[i] = editorValue(b);
        loaded_[i] = true;
    }
    --loading_;
    const bool valid = validate(nullptr);
    if (valid != lastValid_) {
        lastValid_ = valid;
        Q_EMIT validityChanged(valid);
    }
}

JsonDoc FieldBinder::store(const JsonDoc& body) const {
    const QJsonValue base = jsonptr::fromDoc(body);
    const QJsonValue out = storeJson(base);
    if (out == base) {
        return body; // nothing changed: the very same document
    }
    return jsonptr::toDoc(out);
}

QJsonValue FieldBinder::storeJson(const QJsonValue& body) const {
    QJsonValue out = body;
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        const FieldBinding& b = bindings_[i];
        if (b.editor.isNull()) {
            continue;
        }
        const QJsonValue v = editorValue(b);
        if (loaded_[i] && v == initial_[i]) {
            continue; // untouched: the body keeps what it had
        }
        out = jsonptr::set(out.isUndefined() || out.isNull() ? QJsonValue(QJsonObject()) : out, b.pointer, v);
    }
    return out;
}

const FieldBinding* FieldBinder::binding(const QString& pointer) const {
    for (const FieldBinding& b : bindings_) {
        if (b.pointer == pointer) {
            return &b;
        }
    }
    return nullptr;
}

QWidget* FieldBinder::editorFor(const QString& pointer) const {
    const FieldBinding* best = nullptr;
    for (const FieldBinding& b : bindings_) {
        if (jsonptr::covers(b.pointer, pointer) &&
            (best == nullptr || b.pointer.size() > best->pointer.size())) {
            best = &b;
        }
    }
    if (best == nullptr || best->editor.isNull()) {
        return nullptr;
    }
    if (best->pointer == pointer && !isSubtreeKind(best->kind)) {
        return best->editor.data();
    }
    auto* fe = qobject_cast<FieldEditor*>(best->editor.data());
    if (fe == nullptr) {
        return best->pointer == pointer ? best->editor.data() : nullptr;
    }
    return fe->leafEditor(pointer.mid(best->pointer.size()));
}

bool FieldBinder::isModified() const {
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        if (!bindings_[i].editor.isNull() && (!loaded_[i] || editorValue(bindings_[i]) != initial_[i])) {
            return true;
        }
    }
    return false;
}

void FieldBinder::setSchema(const SchemaTree* schema, const QString& prefix) {
    schema_ = schema;
    schemaPrefix_ = prefix;
}

bool FieldBinder::validateRow(std::size_t index, QString* reason) const {
    const FieldBinding& b = bindings_[index];
    QWidget* e = b.editor.data();
    if (e == nullptr) {
        return true;
    }
    if (const auto* fe = qobject_cast<const FieldEditor*>(e); fe != nullptr && !fe->validate(reason)) {
        return false;
    }
    if (const auto* n = qobject_cast<const NumberEdit*>(e); n != nullptr && n->hasText() && !n->value()) {
        if (reason != nullptr) {
            *reason = QStringLiteral("must be a number");
        }
        return false;
    }
    const QJsonValue v = editorValue(b);
    if (loaded_[index] && v == initial_[index]) {
        return true; // what the body already holds is not this edit's to judge
    }
    if (schema_ != nullptr && !schema_->isNull()) {
        const QJsonObject node = schema_->node(schemaPrefix_ + b.pointer);
        if (!node.isEmpty() && !schema_->validate(node, v, reason)) {
            return false;
        }
    }
    return true;
}

bool FieldBinder::validate(QString* reason) const {
    bool valid = true;
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        QString why;
        const bool ok = validateRow(i, &why);
        if (QWidget* e = bindings_[i].editor.data(); e != nullptr) {
            markInvalid(e, ok ? QString() : why);
        }
        if (!ok && valid) {
            valid = false;
            if (reason != nullptr) {
                *reason = bindings_[i].label + QStringLiteral(": ") + why;
            }
        }
    }
    return valid;
}

QStringList FieldBinder::schemaPointers(const JsonDoc& schema) {
    return SchemaTree::fromDoc(schema).leafPointers();
}

} // namespace esses::widgets
