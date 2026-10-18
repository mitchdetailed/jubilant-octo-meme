// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// FieldBinder and its editors (06 section 12 items 4 and 6, AC-M3-08): the RFC 6901 pointer helpers;
// SchemaTree (definitions resolved, leaf pointers with array items as /0 and the non-null alternatives,
// validation reasons); load/store with every leaf kind (members no row binds survive, an untouched row
// leaves the body's member as it was - absent stays absent, a number keeps its exact value, so store()
// returns the very same document); validation against the schema (a red field with the reason, the first
// failing row's label); the subtree editors (UnitValueEdit, FieldForm, RecordListEditor with Add / Remove /
// Up / Down and leafEditor creating records, OptionalEditor writing null when switched off, FixedArrayEdit's
// defaults); edited() and validityChanged().

#include <esses/widgets/properties_shell/field_binder.hpp>
#include <esses/widgets/properties_shell/field_editors.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>
#include <memory>

using namespace esses;
using namespace esses::widgets;

namespace {

JsonDoc doc(const char* json) {
    return JsonDoc::parse(json).value();
}

const char* kSchema = R"({
  "type": "object", "additionalProperties": false,
  "properties": {
    "flag": {"type": "boolean"},
    "mode": {"enum": ["a", "b", "c"]},
    "count": {"type": "integer", "minimum": 1, "maximum": 10},
    "width": {"type": "number", "exclusiveMinimum": 0, "maximum": 5},
    "name": {"type": "string", "minLength": 1},
    "colour": {"$ref": "#/definitions/colour"},
    "limit": {"$ref": "#/definitions/unitValue"},
    "font": {"oneOf": [{"type": "null"}, {"type": "object", "additionalProperties": false, "required": ["family"],
             "properties": {"family": {"type": "string", "minLength": 1}, "size": {"type": "number"}}}]},
    "list": {"type": "array", "items": {"type": "object", "additionalProperties": false, "required": ["channel"],
             "properties": {"channel": {"type": "string"}, "on": {"type": "boolean"}}}},
    "pair": {"type": "array", "minItems": 2, "maxItems": 2, "items": {"type": "number"}},
    "tags": {"type": "array", "uniqueItems": true, "items": {"enum": ["x", "y", "z"]}}
  },
  "definitions": {
    "colour": {"type": "string", "pattern": "^#[0-9A-Fa-f]{6}([0-9A-Fa-f]{2})?$"},
    "unitValue": {"type": "object", "required": ["v", "unit"], "additionalProperties": false,
                  "properties": {"v": {"type": "number"}, "unit": {"type": "string"}}}
  }
})";

struct Page {
    SchemaTree schema = SchemaTree::fromDoc(doc(kSchema));
    QWidget host;
    FieldBinder binder;
    QCheckBox* flag = nullptr;
    QComboBox* mode = nullptr;
    QSpinBox* count = nullptr;
    NumberEdit* width = nullptr;
    QLineEdit* name = nullptr;
    ColourEdit* colour = nullptr;
    UnitValueEdit* limit = nullptr;
    OptionalEditor* font = nullptr;
    RecordListEditor* list = nullptr;
    FixedArrayEdit* pair = nullptr;
    RecordListEditor* tags = nullptr;

    Page() {
        binder.setSchema(&schema);
        const auto leaf = [&](const char* p, FieldKind k) {
            QWidget* w = makeLeafEditor(k, {}, schema.node(QString::fromLatin1(p)), &host);
            binder.bind(QString::fromLatin1(p), k, w, QString::fromLatin1(p + 1),
                        SchemaTree::enumValues(schema.node(QString::fromLatin1(p))));
            return w;
        };
        flag = qobject_cast<QCheckBox*>(leaf("/flag", FieldKind::Bool));
        mode = qobject_cast<QComboBox*>(leaf("/mode", FieldKind::Enum));
        count = qobject_cast<QSpinBox*>(leaf("/count", FieldKind::Integer));
        width = qobject_cast<NumberEdit*>(leaf("/width", FieldKind::Number));
        name = qobject_cast<QLineEdit*>(leaf("/name", FieldKind::Text));
        colour = qobject_cast<ColourEdit*>(leaf("/colour", FieldKind::Colour));
        limit = new UnitValueEdit(&host);
        binder.bind(QStringLiteral("/limit"), FieldKind::UnitValue, limit, QStringLiteral("limit"));
        font = new OptionalEditor(
            QStringLiteral("own font"),
            new FieldForm(
                {FieldRow{QStringLiteral("/family"), FieldKind::Text, QStringLiteral("family"), {}, {}},
                 FieldRow{QStringLiteral("/size"), FieldKind::Number, QStringLiteral("size"), {}, {}}},
                &schema, QStringLiteral("/font"), &host),
            QJsonObject{{QStringLiteral("family"), QStringLiteral("Noto Sans")}}, &host);
        binder.bind(QStringLiteral("/font"), FieldKind::Custom, font, QStringLiteral("font"));
        list = new RecordListEditor(
            [this](QWidget* parent) -> FieldEditor* {
                return new FieldForm(
                    {FieldRow{
                         QStringLiteral("/channel"), FieldKind::Channel, QStringLiteral("channel"), {}, {}},
                     FieldRow{QStringLiteral("/on"), FieldKind::Bool, QStringLiteral("on"), {}, {}}},
                    &schema, QStringLiteral("/list/0"), parent);
            },
            QJsonObject{{QStringLiteral("channel"), QString()}}, {}, &host);
        binder.bind(QStringLiteral("/list"), FieldKind::Custom, list, QStringLiteral("list"));
        pair = new FixedArrayEdit(FieldKind::Number, 2, {QStringLiteral("a"), QStringLiteral("b")}, &host,
                                  {0.0, 1.0});
        binder.bind(QStringLiteral("/pair"), FieldKind::Custom, pair, QStringLiteral("pair"));
        tags = new RecordListEditor(
            [](QWidget* parent) -> FieldEditor* {
                return new LeafEditor(
                    FieldKind::Enum, {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")}, parent);
            },
            QStringLiteral("x"), {}, &host);
        binder.bind(QStringLiteral("/tags"), FieldKind::Custom, tags, QStringLiteral("tags"));
    }
};

} // namespace

class TstFieldBinder : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void pointers() {
        QCOMPARE(jsonptr::split(QStringLiteral("/a/0/b~1c~0d")),
                 (QStringList{QStringLiteral("a"), QStringLiteral("0"), QStringLiteral("b/c~d")}));
        QCOMPARE(jsonptr::join({QStringLiteral("a"), QStringLiteral("b/c")}), QStringLiteral("/a/b~1c"));
        const QJsonValue v = jsonptr::fromDoc(doc(R"({"a":{"b":[1,{"c":2}]}})"));
        QCOMPARE(jsonptr::get(v, QStringLiteral("/a/b/1/c")).toInt(), 2);
        QVERIFY(jsonptr::get(v, QStringLiteral("/a/x")).isUndefined());
        QVERIFY(jsonptr::get(v, QStringLiteral("/a/b/7")).isUndefined());
        QJsonValue w = jsonptr::set(v, QStringLiteral("/a/new/deep"), 3);
        QCOMPARE(jsonptr::get(w, QStringLiteral("/a/new/deep")).toInt(), 3);
        w = jsonptr::set(w, QStringLiteral("/a/b/2"), QStringLiteral("appended"));
        QCOMPARE(jsonptr::get(w, QStringLiteral("/a/b")).toArray().size(), 3);
        w = jsonptr::set(w, QStringLiteral("/a/new"), QJsonValue(QJsonValue::Undefined));
        QVERIFY(jsonptr::get(w, QStringLiteral("/a/new")).isUndefined());
        QVERIFY(jsonptr::covers(QStringLiteral("/a"), QStringLiteral("/a/b")));
        QVERIFY(!jsonptr::covers(QStringLiteral("/a"), QStringLiteral("/ab")));
        QCOMPARE(jsonptr::toDoc(jsonptr::fromDoc(doc(R"({"z":1,"a":[1.5,true,null]})"))),
                 doc(R"({"a":[1.5,true,null],"z":1})"));
    }

    void schemaTree() {
        const SchemaTree s = SchemaTree::fromDoc(doc(kSchema));
        const QStringList leaves = s.leafPointers();
        QCOMPARE(leaves,
                 (QStringList{QStringLiteral("/colour"), QStringLiteral("/count"), QStringLiteral("/flag"),
                              QStringLiteral("/font/family"), QStringLiteral("/font/size"),
                              QStringLiteral("/limit/unit"), QStringLiteral("/limit/v"),
                              QStringLiteral("/list/0/channel"), QStringLiteral("/list/0/on"),
                              QStringLiteral("/mode"), QStringLiteral("/name"), QStringLiteral("/pair/0"),
                              QStringLiteral("/tags/0"), QStringLiteral("/width")}));
        QCOMPARE(FieldBinder::schemaPointers(doc(kSchema)), leaves);
        QCOMPARE(s.node(QStringLiteral("/list/3/channel")).value(QStringLiteral("type")).toString(),
                 QStringLiteral("string"));
        QCOMPARE(s.node(QStringLiteral("/font/family")).value(QStringLiteral("minLength")).toInt(), 1);
        QVERIFY(s.node(QStringLiteral("/nope")).isEmpty());
        QCOMPARE(SchemaTree::enumValues(s.node(QStringLiteral("/mode"))),
                 (QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}));
        QString why;
        QVERIFY(!s.validate(s.node(QStringLiteral("/count")), 11, &why));
        QVERIFY(why.contains(QStringLiteral("at most 10")));
        QVERIFY(!s.validate(s.node(QStringLiteral("/count")), 2.5, &why));
        QVERIFY(why.contains(QStringLiteral("whole number")));
        QVERIFY(!s.validate(s.node(QStringLiteral("/width")), 0, &why));
        QVERIFY(why.contains(QStringLiteral("more than 0")));
        QVERIFY(!s.validate(s.node(QStringLiteral("/colour")), QStringLiteral("red"), &why));
        QVERIFY(!s.validate(s.node(QStringLiteral("/mode")), QStringLiteral("d"), &why));
        QVERIFY(why.contains(QStringLiteral("a, b, c")));
        QVERIFY(!s.validate(s.node(QStringLiteral("/tags")),
                            QJsonArray{QStringLiteral("x"), QStringLiteral("x")}, &why));
        QVERIFY(!s.validate(s.node(QStringLiteral("/pair")), QJsonArray{1}, &why));
        QVERIFY(!s.validate(s.node(QStringLiteral("/list")),
                            QJsonArray{QJsonObject{{QStringLiteral("on"), true}}}, &why));
        QVERIFY(why.contains(QStringLiteral("channel is required")));
        QVERIFY(s.validate(s.node(QStringLiteral("/font")), QJsonValue(QJsonValue::Null), &why));
        QVERIFY(!s.validate(s.node(QStringLiteral("/font")), QJsonObject{{QStringLiteral("size"), 3}}, &why));
        QVERIFY(s.validate(s.node(QString()),
                           jsonptr::fromDoc(doc(R"({"flag":true,"limit":{"v":1,"unit":"s"}})")), &why));
        QVERIFY(!s.validate(s.node(QString()), jsonptr::fromDoc(doc(R"({"unknown":1})")), &why));
    }

    void untouchedStoreIsTheSameDocument() {
        Page p;
        const JsonDoc body =
            doc(R"({"flag":true,"width":0.30000000000000004,"keep":{"me":[1,2]},"list":[{"channel":"A"}]})");
        p.binder.load(body);
        QVERIFY(!p.binder.isModified());
        QCOMPARE(p.binder.store(body), body); // absent stays absent, the exact number stays, "keep" survives
        QVERIFY(p.binder.validate());
        // An edit writes only its row.
        p.count->setValue(7);
        const JsonDoc stored = p.binder.store(body);
        const QJsonValue v = jsonptr::fromDoc(stored);
        QCOMPARE(jsonptr::get(v, QStringLiteral("/count")).toInt(), 7);
        QCOMPARE(jsonptr::get(v, QStringLiteral("/width")).toDouble(), 0.30000000000000004);
        QVERIFY(jsonptr::get(v, QStringLiteral("/mode")).isUndefined());
        QCOMPARE(jsonptr::get(v, QStringLiteral("/keep/me/1")).toInt(), 2);
    }

    void everyKindStores() {
        Page p;
        QSignalSpy edited(&p.binder, &FieldBinder::edited);
        p.binder.load(doc("{}"));
        QCOMPARE(edited.count(), 0); // loading is not an edit
        p.flag->setChecked(true);
        p.mode->setCurrentIndex(2);
        p.count->setValue(3);
        p.width->setText(QStringLiteral("2.5"));
        p.name->setText(QStringLiteral("N"));
        p.colour->setText(QStringLiteral("#AABBCC"));
        p.limit->number().setText(QStringLiteral("4"));
        p.limit->unit().setText(QStringLiteral("km/h"));
        QWidget* family = p.binder.editorFor(QStringLiteral("/font/family")); // switches the optional on
        QVERIFY(family != nullptr);
        QVERIFY(p.font->enabledBox().isChecked());
        qobject_cast<QLineEdit*>(family)->setText(QStringLiteral("Mono"));
        auto* channel =
            qobject_cast<QComboBox*>(p.binder.editorFor(QStringLiteral("/list/1/channel"))); // two records
        QVERIFY(channel != nullptr);
        QCOMPARE(p.list->count(), 2);
        channel->setEditText(QStringLiteral("B"));
        qobject_cast<NumberEdit*>(p.binder.editorFor(QStringLiteral("/pair/1")))
            ->setText(QStringLiteral("0.5"));
        auto* tag = qobject_cast<QComboBox*>(p.binder.editorFor(QStringLiteral("/tags/0")));
        QVERIFY(tag != nullptr);
        tag->setCurrentIndex(2);
        QVERIFY(edited.count() > 10);
        QVERIFY2(p.binder.validate(), qPrintable([&] {
                     QString r;
                     p.binder.validate(&r);
                     return r;
                 }()));
        const QJsonValue v = jsonptr::fromDoc(p.binder.store(doc("{}")));
        QCOMPARE(jsonptr::get(v, QStringLiteral("/flag")).toBool(), true);
        QCOMPARE(jsonptr::get(v, QStringLiteral("/mode")).toString(), QStringLiteral("c"));
        QCOMPARE(jsonptr::get(v, QStringLiteral("/count")).toInt(), 3);
        QCOMPARE(jsonptr::get(v, QStringLiteral("/width")).toDouble(), 2.5);
        QCOMPARE(jsonptr::get(v, QStringLiteral("/name")).toString(), QStringLiteral("N"));
        QCOMPARE(jsonptr::get(v, QStringLiteral("/colour")).toString(),
                 QStringLiteral("#aabbcc")); // lower case
        QCOMPARE(jsonptr::get(v, QStringLiteral("/limit")).toObject(),
                 (QJsonObject{{QStringLiteral("v"), 4}, {QStringLiteral("unit"), QStringLiteral("km/h")}}));
        QCOMPARE(jsonptr::get(v, QStringLiteral("/font/family")).toString(), QStringLiteral("Mono"));
        QCOMPARE(jsonptr::get(v, QStringLiteral("/list")).toArray().size(), 2);
        QCOMPARE(jsonptr::get(v, QStringLiteral("/list/1/channel")).toString(), QStringLiteral("B"));
        QCOMPARE(jsonptr::get(v, QStringLiteral("/pair")).toArray(),
                 (QJsonArray{0.0, 0.5})); // the default fills /0
        QCOMPARE(jsonptr::get(v, QStringLiteral("/tags")).toArray(), QJsonArray{QStringLiteral("z")});
        // Switching the optional off again leaves the member absent (it was absent); the record list removes
        // and reorders.
        p.font->enabledBox().setChecked(false);
        p.list->setCurrentIndex(0);
        p.list->moveCurrent(1);
        const QJsonValue w = jsonptr::fromDoc(p.binder.store(doc("{}")));
        QVERIFY(jsonptr::get(w, QStringLiteral("/font")).isUndefined());
        QCOMPARE(jsonptr::get(w, QStringLiteral("/list/0/channel")).toString(), QStringLiteral("B"));
        p.list->removeCurrent();
        QCOMPARE(jsonptr::get(jsonptr::fromDoc(p.binder.store(doc("{}"))), QStringLiteral("/list"))
                     .toArray()
                     .size(),
                 1);
        // A member that was set and is switched off becomes null.
        const JsonDoc withFont = doc(R"({"font":{"family":"Serif"}})");
        p.binder.load(withFont);
        QVERIFY(p.font->enabledBox().isChecked());
        p.font->enabledBox().setChecked(false);
        QVERIFY(jsonptr::get(jsonptr::fromDoc(p.binder.store(withFont)), QStringLiteral("/font")).isNull());
    }

    void validation() {
        Page p;
        QSignalSpy validity(&p.binder, &FieldBinder::validityChanged);
        p.binder.load(doc(R"({"count":2})"));
        QVERIFY(p.binder.validate());
        p.width->setText(QStringLiteral("9")); // maximum 5
        QString reason;
        QVERIFY(!p.binder.validate(&reason));
        QCOMPARE(reason, QStringLiteral("width: must be at most 5"));
        QVERIFY(p.width->property("esses.invalid").toBool());
        QCOMPARE(p.width->toolTip(), QStringLiteral("must be at most 5"));
        QCOMPARE(validity.count(), 1);
        QCOMPARE(validity.back().front().toBool(), false);
        p.width->setText(QStringLiteral("x"));
        QVERIFY(!p.binder.validate(&reason));
        QVERIFY(reason.contains(QStringLiteral("number")));
        p.width->setText(QStringLiteral("1"));
        QVERIFY(p.binder.validate());
        QVERIFY(!p.width->property("esses.invalid").toBool());
        QCOMPARE(validity.back().front().toBool(), true);
        // A duplicate entry of a unique list, a required member in a record, a bad colour.
        qobject_cast<QComboBox*>(p.binder.editorFor(QStringLiteral("/tags/0")))->setCurrentIndex(0);
        qobject_cast<QComboBox*>(p.binder.editorFor(QStringLiteral("/tags/1")))->setCurrentIndex(0);
        QVERIFY(!p.binder.validate(&reason));
        QVERIFY(reason.contains(QStringLiteral("twice")));
        qobject_cast<QComboBox*>(p.binder.editorFor(QStringLiteral("/tags/1")))->setCurrentIndex(1);
        QVERIFY(p.binder.validate());
        p.colour->setText(QStringLiteral("#12"));
        QVERIFY(!p.binder.validate(&reason));
        QVERIFY(reason.startsWith(QStringLiteral("colour: ")));
    }
};

QTEST_MAIN(TstFieldBinder)
#include "tst_field_binder.moc"
