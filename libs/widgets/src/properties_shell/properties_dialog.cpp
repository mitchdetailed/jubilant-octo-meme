// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// PropertiesDialog (06 section 12, AC-M3-08). Owner: src/properties_shell/.

#include "property_pages.hpp"

#include <esses/maths/channel_key.hpp>
#include <esses/project/schema_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/properties_shell/field_binder.hpp>
#include <esses/widgets/properties_shell/field_editors.hpp>
#include <esses/widgets/properties_shell/properties_dialog.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>
#include <map>
#include <set>
#include <utility>

namespace esses::widgets {

namespace {

PropertiesDialog::HeadingSource& headingSource() {
    static PropertiesDialog::HeadingSource source; // GUI thread only
    return source;
}

QHash<const ComponentWidget*, QPointer<PropertiesDialog>>& openDialogs() {
    static QHash<const ComponentWidget*, QPointer<PropertiesDialog>> table; // GUI thread only
    return table;
}

PropertiesDialog::GeometryLoad& geometryLoad() {
    static PropertiesDialog::GeometryLoad load;
    return load;
}

PropertiesDialog::GeometrySave& geometrySave() {
    static PropertiesDialog::GeometrySave save;
    return save;
}

QJsonObject headerJson(const project::ComponentSpec& s) {
    return QJsonObject{{QStringLiteral("caption"),
                        QJsonObject{{QStringLiteral("show"), s.caption.show},
                                    {QStringLiteral("auto"), s.caption.autoText},
                                    {QStringLiteral("text"), QString::fromStdString(s.caption.text)}}},
                       {QStringLiteral("links"), QJsonObject{{QStringLiteral("zoom"), s.links.zoom},
                                                             {QStringLiteral("cursor"), s.links.cursor},
                                                             {QStringLiteral("datum"), s.links.datum}}},
                       {QStringLiteral("legend"), QJsonObject{{QStringLiteral("channels"), s.legend.channels},
                                                              {QStringLiteral("colour"), s.legend.colour}}}};
}

void applyHeader(project::ComponentSpec& s, const QJsonObject& h) {
    const QJsonObject caption = h.value(QStringLiteral("caption")).toObject();
    const QJsonObject links = h.value(QStringLiteral("links")).toObject();
    const QJsonObject legend = h.value(QStringLiteral("legend")).toObject();
    s.caption.show = caption.value(QStringLiteral("show")).toBool(s.caption.show);
    s.caption.autoText = caption.value(QStringLiteral("auto")).toBool(s.caption.autoText);
    s.caption.text =
        caption.value(QStringLiteral("text")).toString(QString::fromStdString(s.caption.text)).toStdString();
    s.links.zoom = links.value(QStringLiteral("zoom")).toBool(s.links.zoom);
    s.links.cursor = links.value(QStringLiteral("cursor")).toBool(s.links.cursor);
    s.links.datum = links.value(QStringLiteral("datum")).toBool(s.links.datum);
    s.legend.channels = legend.value(QStringLiteral("channels")).toBool(s.legend.channels);
    s.legend.colour = legend.value(QStringLiteral("colour")).toBool(s.legend.colour);
}

WorksheetCanvas* canvasOf(const QWidget* w) {
    for (QWidget* p = w == nullptr ? nullptr : w->parentWidget(); p != nullptr; p = p->parentWidget()) {
        if (auto* c = qobject_cast<WorksheetCanvas*>(p)) {
            return c;
        }
    }
    return nullptr;
}

QString typeOf(const ComponentWidget& c) {
    return QString::fromLatin1(c.type());
}

// The channel names of the open sources (their Normal layers), sorted, plus the names the body uses.
// The names a channel picker offers: the logged channels and the maths outputs (Corr Speed, Corr Dist, the
// user's maths) of every open source, and the component's own (D-203 (l)).
QStringList channelChoices(const ComponentWidget& c) {
    std::set<QString> names;
    WorksheetContext& context = const_cast<ComponentWidget&>(c).context();
    const qt::DataSelectionModel& sel = context.selection;
    for (const auto& src : sel.sources()) {
        if (src == nullptr) {
            continue;
        }
        if (const model::DataLayer* layer = src->layer("Normal")) {
            for (const model::Channel& ch : layer->channels) {
                names.insert(QString::fromStdString(ch.desc.name));
            }
        }
        for (const maths::ChannelKey& key : context.maths.outputs(src->id, "Normal")) {
            if (const std::optional<model::ChannelDescriptor> d = context.maths.descriptor(key)) {
                names.insert(QString::fromStdString(d->name));
            }
        }
    }
    for (const QString& n : c.channelNames()) {
        names.insert(n);
    }
    QStringList out;
    for (const QString& n : names) {
        if (!n.isEmpty()) {
            out.push_back(n);
        }
    }
    return out;
}

// Selects the record that names `channel` in a list, descending into nested lists (groups -> traces).
bool selectRecord(RecordListEditor& list, const QString& channel) {
    for (const char* member : {"channel", "yChannel", "name"}) {
        const int i = list.find(QLatin1String(member), channel);
        if (i >= 0) {
            list.setCurrentIndex(i);
            return true;
        }
    }
    const QJsonArray items = list.value().toArray();
    const QByteArray needle = QJsonDocument(QJsonArray{channel}).toJson(QJsonDocument::Compact);
    for (int i = 0; i < items.size(); ++i) {
        const QByteArray text = items.at(i).isObject()
                                    ? QJsonDocument(items.at(i).toObject()).toJson(QJsonDocument::Compact)
                                    : QByteArray();
        if (!text.contains(needle.mid(1, needle.size() - 2))) {
            continue;
        }
        list.setCurrentIndex(i);
        for (RecordListEditor* nested : list.itemEditor().findChildren<RecordListEditor*>()) {
            if (selectRecord(*nested, channel)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

struct PropertiesDialog::Impl {
    QPointer<ComponentWidget> guard;
    SchemaTree schema;
    std::vector<pages::Page> pages;
    QTabWidget* tabs = nullptr;
    QLabel* message = nullptr;
    QDialogButtonBox* buttons = nullptr;
    FieldBinder* header = nullptr;
    project::ComponentSpec openSpec;
    project::ComponentSpec lastApplied; // the spec the last Apply emitted
    bool applied = false;
    bool closing = false;
    QString type;
};

void PropertiesDialog::setGeometryStore(GeometryLoad load, GeometrySave save) {
    geometryLoad() = std::move(load);
    geometrySave() = std::move(save);
}

JsonDoc PropertiesDialog::bodySchema(const QString& type, int version) {
    static std::map<QString, JsonDoc> cache; // GUI thread only
    const QString key = type + QLatin1Char('-') + QString::number(version);
    if (const auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    JsonDoc doc;
    const std::filesystem::path dir = project::SchemaRegistry::findSchemasDir();
    if (!dir.empty()) {
        Result<JsonDoc> r = JsonDoc::load(dir / "components" / (key.toStdString() + ".schema.json"));
        if (r) {
            doc = std::move(r).value();
        }
    }
    cache.emplace(key, doc);
    return doc;
}

PropertiesDialog* PropertiesDialog::openFor(ComponentWidget& component, int tabIndex) {
    PropertiesDialog* d = find(component);
    if (d == nullptr) {
        d = new PropertiesDialog(component, component.window());
        d->setAttribute(Qt::WA_DeleteOnClose);
    }
    if (tabIndex >= 0 && tabIndex < d->d_->tabs->count()) {
        d->d_->tabs->setCurrentIndex(tabIndex);
    }
    d->show();
    d->raise();
    d->activateWindow();
    return d;
}

PropertiesDialog* PropertiesDialog::find(const ComponentWidget& component) {
    const auto it = openDialogs().constFind(&component);
    return it == openDialogs().constEnd() ? nullptr : it.value().data();
}

PropertiesDialog::PropertiesDialog(ComponentWidget& component, QWidget* parent)
    : QDialog(parent), component_(component), binder_(new FieldBinder(this)), d_(std::make_unique<Impl>()) {
    setModal(false);
    setWindowFlag(Qt::Dialog, true);
    d_->guard = &component;
    d_->type = typeOf(component);
    d_->openSpec = component.spec();
    d_->header = new FieldBinder(this);
    setObjectName(QStringLiteral("propertiesDialog"));
    setWindowTitle(tr("%1 Properties").arg(d_->type));
    d_->schema = SchemaTree::fromDoc(bodySchema(d_->type, component.spec().version));
    binder_->setSchema(&d_->schema);
    build();
    refresh();
    openDialogs().insert(&component, this);
    // The component is deleted (cut, delete, another workbook): the dialog goes with it.
    connect(&component, &QObject::destroyed, this, [this] {
        d_->guard.clear();
        d_->closing = true;
        openDialogs().remove(&component_);
        hide();
        deleteLater();
    });
    if (WorksheetCanvas* canvas = canvasOf(&component)) {
        connect(canvas, &WorksheetCanvas::componentSpecApplied, this, [this](ComponentWidget* c) {
            if (c == d_->guard.data()) {
                // Undo, redo or another edit of this component while the dialog is open: the rows the user
                // has not changed follow it, an unapplied edit stays (D-203 (l)).
                refreshUntouched();
            }
        });
    }
    // Sources opened or closed, maths outputs appearing: the channel pickers offer what exists now.
    connect(&component.context().selection, &qt::DataSelectionModel::sourcesChanged, this,
            &PropertiesDialog::refreshChannelChoices);
    connect(&component.context().maths, &qt::MathsService::statusChanged, this,
            &PropertiesDialog::refreshChannelChoices);
    connect(binder_, &FieldBinder::edited, this, &PropertiesDialog::updateButtons);
    connect(d_->header, &FieldBinder::edited, this, &PropertiesDialog::updateButtons);
    if (geometryLoad()) {
        const QByteArray g = geometryLoad()(d_->type);
        if (!g.isEmpty()) {
            restoreGeometry(g);
        }
    }
    if (geometry().width() < 420) {
        resize(560, 520);
    }
}

PropertiesDialog::~PropertiesDialog() {
    if (const auto it = openDialogs().find(&component_); it != openDialogs().end() && it.value() == this) {
        openDialogs().erase(it);
    }
}

void PropertiesDialog::build() {
    auto* column = new QVBoxLayout(this);
    d_->tabs = new QTabWidget(this);
    d_->tabs->setObjectName(QStringLiteral("tabs"));
    column->addWidget(d_->tabs, 1);
    d_->message = new QLabel(this);
    d_->message->setObjectName(QStringLiteral("validation"));
    d_->message->setStyleSheet(QStringLiteral("color: #c02020;"));
    d_->message->setWordWrap(true);
    column->addWidget(d_->message);
    d_->buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    column->addWidget(d_->buttons);
    connect(d_->buttons, &QDialogButtonBox::accepted, this, &PropertiesDialog::accept);
    connect(d_->buttons, &QDialogButtonBox::rejected, this, &PropertiesDialog::reject);
    connect(d_->buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            &PropertiesDialog::apply);

    d_->pages = pages::pagesFor(d_->type, &d_->schema);
    // A type without pages (an unknown component) still shows its header.
    bool hasDisplay = false;
    for (const pages::Page& p : d_->pages) {
        hasDisplay = hasDisplay || p.title == tr("Display");
    }
    if (!hasDisplay) {
        d_->pages.push_back(pages::Page{tr("Display"), {}});
    }
    for (const pages::Page& p : d_->pages) {
        auto* scroll = new QScrollArea(d_->tabs);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* page = new QWidget(scroll);
        page->setObjectName(QStringLiteral("page.") + p.title);
        auto* pageColumn = new QVBoxLayout(page);
        auto* form = new QFormLayout;
        pageColumn->addLayout(form);
        for (const FieldRow& r : p.rows) {
            const QJsonObject node = d_->schema.node(r.pointer);
            QWidget* editor = nullptr;
            const bool subtree = r.kind == FieldKind::UnitValue || r.kind == FieldKind::BandAxis ||
                                 r.kind == FieldKind::ColourBands || r.kind == FieldKind::Custom;
            if (subtree) {
                editor = r.make ? r.make(page) : new UnitValueEdit(page);
            } else {
                editor = makeLeafEditor(r.kind, r.enumValues, node, page);
            }
            editor->setObjectName(r.pointer);
            form->addRow(r.label, editor);
            binder_->bind(r.pointer, r.kind, editor, r.label,
                          r.enumValues.isEmpty() ? SchemaTree::enumValues(node) : r.enumValues);
        }
        if (p.title == tr("Display")) {
            auto* group = new QGroupBox(tr("Component"), page);
            group->setObjectName(QStringLiteral("header"));
            auto* headerForm = new QFormLayout(group);
            for (const FieldRow& r : pages::headerRows()) {
                QWidget* editor = makeLeafEditor(r.kind, r.enumValues, {}, group);
                editor->setObjectName(r.pointer);
                if (r.pointer.startsWith(QLatin1String("/links/"))) {
                    // Kept with the component; honouring them waits for M4 (D-203 (m)).
                    editor->setToolTip(tr("Kept with the component; every component follows the worksheet's "
                                          "zoom, cursor and datum until component links are built"));
                }
                headerForm->addRow(r.label, editor);
                d_->header->bind(r.pointer, r.kind, editor, r.label);
            }
            pageColumn->addWidget(group);
        }
        pageColumn->addStretch(1);
        scroll->setWidget(page);
        d_->tabs->addTab(scroll, p.title);
    }
    refreshChannelChoices();
}

void PropertiesDialog::refreshChannelChoices() {
    if (d_->guard.isNull()) {
        return;
    }
    const QStringList choices = channelChoices(component_);
    const HeadingSource& headings = headingSource();
    const QStringList headingList = headings ? headings() : QStringList();
    for (QComboBox* c : findChildren<QComboBox*>()) {
        setChannelChoices(c, choices);
        if (!headingList.isEmpty()) {
            setHeadingChoices(c, headingList);
        }
    }
}

void PropertiesDialog::setHeadingSource(HeadingSource source) {
    headingSource() = std::move(source);
}

void PropertiesDialog::refreshUntouched() {
    if (d_->guard.isNull()) {
        return;
    }
    const project::ComponentSpec& s = component_.spec();
    binder_->loadUntouchedJson(jsonptr::fromDoc(s.body));
    d_->header->loadUntouchedJson(headerJson(s));
    updateButtons();
}

QStringList PropertiesDialog::tabTitles() const {
    QStringList out;
    for (int i = 0; i < d_->tabs->count(); ++i) {
        out.push_back(d_->tabs->tabText(i));
    }
    return out;
}

FieldBinder& PropertiesDialog::binder() {
    return *binder_;
}

FieldBinder& PropertiesDialog::headerBinder() {
    return *d_->header;
}

QTabWidget& PropertiesDialog::tabs() const {
    return *d_->tabs;
}

bool PropertiesDialog::wasApplied() const noexcept {
    return d_->applied;
}

int PropertiesDialog::channelsTabIndex() const {
    return pages::channelsTab(d_->pages);
}

project::ComponentSpec PropertiesDialog::editedSpec() const {
    project::ComponentSpec s = d_->guard.isNull() ? d_->openSpec : component_.spec();
    s.body = binder_->store(s.body);
    applyHeader(s, d_->header->storeJson(headerJson(s)).toObject());
    return s;
}

bool PropertiesDialog::isValid() const {
    return binder_->validate(nullptr) && d_->header->validate(nullptr);
}

QString PropertiesDialog::validationMessage() const {
    QString reason;
    if (!binder_->validate(&reason) || !d_->header->validate(&reason)) {
        return reason;
    }
    return {};
}

void PropertiesDialog::updateButtons() {
    const QString reason = validationMessage();
    d_->message->setText(reason);
    const bool valid = reason.isEmpty();
    d_->buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
    d_->buttons->button(QDialogButtonBox::Apply)->setEnabled(valid);
}

void PropertiesDialog::apply() {
    if (d_->guard.isNull() || !isValid()) {
        return;
    }
    const project::ComponentSpec next = editedSpec();
    if (next == component_.spec()) {
        return;
    }
    d_->applied = true;
    d_->lastApplied = next;
    Q_EMIT component_.specEditRequested(next, tr("Edit %1 properties").arg(d_->type));
    if (!d_->guard.isNull() && component_.spec() == next) {
        refresh(); // the pages now describe the applied spec
    }
}

void PropertiesDialog::refresh() {
    if (d_->guard.isNull()) {
        return;
    }
    const project::ComponentSpec& s = component_.spec();
    binder_->load(s.body);
    d_->header->loadJson(headerJson(s));
    updateButtons();
}

bool PropertiesDialog::focusChannel(const QString& channel) {
    const int tab = channelsTabIndex();
    d_->tabs->setCurrentIndex(tab);
    QWidget* page = d_->tabs->widget(tab);
    if (page == nullptr || channel.isEmpty()) {
        return false;
    }
    for (RecordListEditor* list : page->findChildren<RecordListEditor*>()) {
        // Top-level lists only: a nested one (the traces of a group) is reached through its parent record.
        const QWidget* grand =
            list->parentWidget() == nullptr ? nullptr : list->parentWidget()->parentWidget();
        if (qobject_cast<const RecordListEditor*>(grand) == nullptr && selectRecord(*list, channel)) {
            return true;
        }
    }
    // Single-channel types (gauges): the channel field is the record.
    for (QComboBox* c : page->findChildren<QComboBox*>()) {
        if (c->property("esses.channelPicker").toBool() && c->currentText() == channel) {
            c->setFocus(Qt::OtherFocusReason);
            return true;
        }
    }
    return false;
}

void PropertiesDialog::saveGeometryNow() {
    if (geometrySave()) {
        geometrySave()(d_->type, saveGeometry());
    }
}

void PropertiesDialog::accept() {
    apply();
    if (!isValid()) {
        return;
    }
    saveGeometryNow();
    d_->closing = true;
    QDialog::accept();
}

void PropertiesDialog::reject() {
    if (!d_->closing && !d_->guard.isNull() && d_->applied) {
        // Back to the open-time values of what the dialog applied, on top of the component's current spec:
        // a move, a resize, the z order, the active channel or a hotkey toggle made meanwhile stays
        // (D-203 (l)).
        const project::ComponentSpec current = component_.spec();
        project::ComponentSpec revert = current;
        QJsonValue body = jsonptr::fromDoc(current.body);
        if (body.isUndefined()) {
            body = QJsonObject();
        }
        const QJsonValue open = jsonptr::fromDoc(d_->openSpec.body);
        const QJsonValue applied = jsonptr::fromDoc(d_->lastApplied.body);
        for (const QString& pointer : binder_->boundPointers()) {
            const QJsonValue was = jsonptr::get(open, pointer);
            if (jsonptr::get(applied, pointer) != was) {
                body = jsonptr::set(body, pointer, was);
            }
        }
        revert.body = jsonptr::toDoc(body);
        const QJsonValue openHeader = headerJson(d_->openSpec);
        const QJsonValue appliedHeader = headerJson(d_->lastApplied);
        QJsonValue header = headerJson(current);
        for (const QString& pointer : d_->header->boundPointers()) {
            const QJsonValue was = jsonptr::get(openHeader, pointer);
            if (jsonptr::get(appliedHeader, pointer) != was) {
                header = jsonptr::set(header, pointer, was);
            }
        }
        applyHeader(revert, header.toObject());
        if (!(revert == current)) {
            Q_EMIT component_.specEditRequested(revert, tr("Edit %1 properties").arg(d_->type));
        }
    }
    saveGeometryNow();
    d_->closing = true;
    QDialog::reject();
}

void PropertiesDialog::closeEvent(QCloseEvent* event) {
    if (!d_->closing) {
        reject(); // the title bar's close button is Cancel
    }
    event->accept();
}

} // namespace esses::widgets
