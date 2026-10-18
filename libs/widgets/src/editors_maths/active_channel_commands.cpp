// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The active-channel commands of 06 section 14.4 (component context): trace navigation and edits on the
// focused component's spec, the Maths Editor and Channel Status entry points, and the transient / local maths
// items of CTRL+SHIFT+F / O / Z (03 sections 4.2, 4.4 and 11 item 10).

#include <esses/base/casefold.hpp>
#include <esses/maths/channel_key.hpp>
#include <esses/maths/math_module_io.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/source.hpp>
#include <esses/plotcore/change_mask.hpp>
#include <esses/plotcore/time_distance_builder.hpp>
#include <esses/project/channel_overrides.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/qtmodel/values_model.hpp>
#include <esses/qtmodel/worksheet_view_model.hpp>
#include <esses/units/unit_registry.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/editors_maths/active_channel_commands.hpp>
#include <esses/widgets/editors_maths/channel_status_window.hpp>
#include <esses/widgets/editors_maths/maths_editor.hpp>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace esses::widgets {

namespace {

QString qs(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

std::string us(const QString& s) {
    return s.toStdString();
}

bool sameName(std::string_view a, std::string_view b) {
    return caseFold(a) == caseFold(b);
}

std::optional<plot::TimeDistanceBody> graphBody(const ComponentWidget& c) {
    if (c.spec().type != "time_distance") {
        return std::nullopt;
    }
    Result<plot::TimeDistanceBody> body = plot::TimeDistanceBody::parse(c.spec().body);
    if (!body) {
        return std::nullopt;
    }
    return std::move(body).value();
}

// The traces a user can make active: the visible traces of a graph in key order, else the channel list.
QStringList activeCandidates(const ComponentWidget& c) {
    QStringList out;
    if (const std::optional<plot::TimeDistanceBody> body = graphBody(c)) {
        for (const plot::TdGroup& g : body->groups) {
            for (const plot::TdTrace& t : g.traces) {
                if (t.visible) {
                    out << qs(t.channel);
                }
            }
        }
        return out;
    }
    return c.channelNames();
}

void requestSpec(ComponentWidget& c, const project::ComponentSpec& next, const QString& text) {
    // The component's own edit path: WorksheetCanvas turns specEditRequested into the undoable edit.
    Q_EMIT c.specEditRequested(next, text);
}

std::optional<ActiveChannelCommands::FilterChoice> askFilter(const QString& channel) {
    QDialog dialog;
    dialog.setWindowTitle(ActiveChannelCommands::tr("Filter %1").arg(channel));
    auto* form = new QFormLayout(&dialog);
    auto* mode = new QComboBox(&dialog);
    mode->addItems({ActiveChannelCommands::tr("Points"), ActiveChannelCommands::tr("Seconds")});
    auto* value = new QDoubleSpinBox(&dialog);
    value->setRange(0.001, 1.0e6);
    value->setDecimals(0);
    value->setValue(5.0);
    QObject::connect(mode, &QComboBox::currentIndexChanged, value,
                     [value](int index) { value->setDecimals(index == 1 ? 3 : 0); });
    form->addRow(ActiveChannelCommands::tr("Window in"), mode);
    form->addRow(ActiveChannelCommands::tr("Window"), value);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    ActiveChannelCommands::FilterChoice choice;
    if (mode->currentIndex() == 0) {
        choice.points = static_cast<int>(std::lround(value->value()));
    } else {
        choice.seconds = value->value();
    }
    return choice;
}

std::optional<ActiveChannelCommands::ScaleOffsetChoice> askScaleOffset(const QString& channel,
                                                                       const QString& displayUnit) {
    QDialog dialog;
    dialog.setWindowTitle(ActiveChannelCommands::tr("Scale and offset %1").arg(channel));
    auto* form = new QFormLayout(&dialog);
    auto* scale = new QDoubleSpinBox(&dialog);
    scale->setRange(-1.0e9, 1.0e9);
    scale->setDecimals(6);
    scale->setValue(1.0);
    auto* offset = new QDoubleSpinBox(&dialog);
    offset->setRange(-1.0e9, 1.0e9);
    offset->setDecimals(6);
    auto* unit = new QLineEdit(displayUnit, &dialog);
    form->addRow(ActiveChannelCommands::tr("Scale"), scale);
    form->addRow(ActiveChannelCommands::tr("Offset"), offset);
    form->addRow(ActiveChannelCommands::tr("Offset unit"), unit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return ActiveChannelCommands::ScaleOffsetChoice{scale->value(), offset->value(), unit->text().trimmed()};
}

} // namespace

struct ActiveChannelCommands::Target {
    ComponentWidget* component = nullptr;
    QString channel;
    std::shared_ptr<const model::DataSource> source; // the Main source, when it is open
    std::string layer{model::kNormalLayer};
    qt::ChannelRequest request; // the component's display unit and decimals for the channel
};

ActiveChannelCommands::ActiveChannelCommands(qt::ProjectSession& session, qt::SourceLoader& loader,
                                             qt::MathsService& maths, qt::DataSelectionModel& selection,
                                             Host host, QObject* parent)
    : QObject(parent), session_(session), loader_(loader), maths_(maths), selection_(selection),
      host_(std::move(host)), filterPrompt_(askFilter), scaleOffsetPrompt_(askScaleOffset) {
    // Transient maths lives as long as its source (03 section 4.2): closing or reloading drops it.
    connect(&loader_, &qt::SourceLoader::sourceClosed, this,
            [this](const model::SourceId& source) { transient_.erase(source); });
}

ActiveChannelCommands::~ActiveChannelCommands() = default;

QStringList ActiveChannelCommands::commandIds() {
    return {QStringLiteral("channel.next_active"),   QStringLiteral("channel.prev_active"),
            QStringLiteral("channel.hide"),          QStringLiteral("channel.remove"),
            QStringLiteral("channel.edit_maths"),    QStringLiteral("channel.status"),
            QStringLiteral("channel.filter"),        QStringLiteral("channel.scale_offset"),
            QStringLiteral("channel.zero_at_cursor")};
}

void ActiveChannelCommands::registerCommands(qt::CommandRegistry& registry) {
    const QPointer<ActiveChannelCommands> self(this);
    const auto add = [&registry, self](const char* id, const QString& title, const char* key,
                                       std::function<bool(ActiveChannelCommands&)> run, bool graphOnly) {
        qt::CommandSpec spec;
        spec.id = QString::fromLatin1(id);
        spec.title = title;
        spec.menuPath =
            QStringLiteral("Component/Active Channel"); // 06 section 2.2, as the shell's other rows
        spec.context = qt::CommandContext::Component;
        spec.defaultKeys = {QString::fromLatin1(key)};
        spec.enabled = [self, graphOnly] {
            if (self.isNull() || !self->target()) {
                return false;
            }
            return !graphOnly || graphBody(*self->target()->component).has_value();
        };
        spec.invoke = [self, run](const QVariantList&) {
            if (!self.isNull()) {
                static_cast<void>(run(*self));
            }
        };
        registry.add(std::move(spec));
    };
    add(
        "channel.next_active", tr("Next Active Channel"), "PgDown",
        [](ActiveChannelCommands& c) { return c.nextActive(1); }, false);
    add(
        "channel.prev_active", tr("Previous Active Channel"), "PgUp",
        [](ActiveChannelCommands& c) { return c.nextActive(-1); }, false);
    add(
        "channel.hide", tr("Hide Channel"), "Ctrl+H", [](ActiveChannelCommands& c) { return c.hideActive(); },
        true);
    add(
        "channel.remove", tr("Remove Channel"), "Ctrl+Delete",
        [](ActiveChannelCommands& c) { return c.removeActive(); }, true);
    add(
        "channel.edit_maths", tr("Edit Channel Maths"), "Ctrl+Shift+M",
        [](ActiveChannelCommands& c) { return c.editMaths(); }, false);
    add(
        "channel.status", tr("Channel Status of Active Channel"), "Ctrl+Shift+S",
        [](ActiveChannelCommands& c) { return c.showStatus(); }, false);
    add(
        "channel.filter", tr("Filter Channel..."), "Ctrl+Shift+F",
        [](ActiveChannelCommands& c) { return c.filterActive(); }, false);
    add(
        "channel.scale_offset", tr("Scale and Offset Channel..."), "Ctrl+Shift+O",
        [](ActiveChannelCommands& c) { return c.scaleOffsetActive(); }, false);
    add(
        "channel.zero_at_cursor", tr("Zero Channel at Cursor"), "Ctrl+Shift+Z",
        [](ActiveChannelCommands& c) { return c.zeroAtCursor(); }, false);
}

std::optional<QString> ActiveChannelCommands::activeChannel() const {
    ComponentWidget* c = host_.focusedComponent ? host_.focusedComponent() : nullptr;
    if (c == nullptr) {
        return std::nullopt;
    }
    if (const std::optional<QString> active = c->activeChannel()) {
        return active;
    }
    const QStringList names = activeCandidates(*c);
    if (names.isEmpty()) {
        return std::nullopt;
    }
    return names.front();
}

std::optional<ActiveChannelCommands::Target> ActiveChannelCommands::target() const {
    ComponentWidget* c = host_.focusedComponent ? host_.focusedComponent() : nullptr;
    const std::optional<QString> channel = activeChannel();
    if (c == nullptr || !channel) {
        return std::nullopt;
    }
    Target t;
    t.component = c;
    t.channel = *channel;
    if (selection_.hasMain()) {
        const model::RangeRef& main = selection_.snapshot().main;
        t.source = loader_.source(main.src);
        t.layer = std::string(model::canonicalLayerName(main.layer));
    }
    t.request.name = us(*channel);
    for (const qt::ChannelRequest& r : qt::componentChannels(c->spec())) {
        if (sameName(r.name, t.request.name)) {
            t.request = r;
            break;
        }
    }
    return t;
}

bool ActiveChannelCommands::fail(const QString& text) {
    lastHint_ = text;
    Q_EMIT hint(text);
    return false;
}

// ---- spec edits -------------------------------------------------------------------------------------------

bool ActiveChannelCommands::nextActive(int step) {
    lastHint_.clear();
    const std::optional<Target> t = target();
    if (!t || step == 0) {
        return false;
    }
    const QStringList names = activeCandidates(*t->component);
    if (names.isEmpty()) {
        return false;
    }
    qsizetype at = -1;
    for (qsizetype i = 0; i < names.size(); ++i) {
        if (sameName(us(names[i]), us(t->channel))) {
            at = i;
            break;
        }
    }
    const auto n = names.size();
    const qsizetype next = at < 0 ? 0 : ((at + step) % n + n) % n;
    if (sameName(us(names[next]), t->component->spec().activeChannel)) {
        return false;
    }
    project::ComponentSpec spec = t->component->spec();
    spec.activeChannel = us(names[next]);
    requestSpec(*t->component, spec, tr("Active channel %1").arg(names[next]));
    return true;
}

bool ActiveChannelCommands::hideActive() {
    lastHint_.clear();
    const std::optional<Target> t = target();
    if (!t) {
        return false;
    }
    std::optional<plot::TimeDistanceBody> body = graphBody(*t->component);
    if (!body) {
        return false;
    }
    bool hidden = false;
    for (plot::TdGroup& g : body->groups) {
        for (plot::TdTrace& trace : g.traces) {
            if (!hidden && trace.visible && sameName(trace.channel, us(t->channel))) {
                trace.visible = false;
                hidden = true;
            }
        }
    }
    if (!hidden) {
        return false;
    }
    project::ComponentSpec spec = t->component->spec();
    spec.body = body->toJson();
    // The square moves to the next visible trace (none: no active channel).
    std::string next;
    bool after = false;
    std::string first;
    for (const plot::TdGroup& g : body->groups) {
        for (const plot::TdTrace& trace : g.traces) {
            if (sameName(trace.channel, us(t->channel))) {
                after = true;
                continue;
            }
            if (!trace.visible) {
                continue;
            }
            if (first.empty()) {
                first = trace.channel;
            }
            if (after && next.empty()) {
                next = trace.channel;
            }
        }
    }
    spec.activeChannel = next.empty() ? first : next;
    requestSpec(*t->component, spec, tr("Hide %1").arg(t->channel));
    return true;
}

bool ActiveChannelCommands::removeActive() {
    lastHint_.clear();
    const std::optional<Target> t = target();
    if (!t) {
        return false;
    }
    std::optional<plot::TimeDistanceBody> body = graphBody(*t->component);
    if (!body) {
        return false;
    }
    // The visible trace after the removed one becomes active (or the one before it at the end).
    std::vector<std::string> order;
    for (const plot::TdGroup& g : body->groups) {
        for (const plot::TdTrace& trace : g.traces) {
            if (trace.visible || sameName(trace.channel, us(t->channel))) {
                order.push_back(trace.channel);
            }
        }
    }
    std::string replacement;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (sameName(order[i], us(t->channel))) {
            replacement = i + 1 < order.size() ? order[i + 1] : (i > 0 ? order[i - 1] : std::string());
            break;
        }
    }
    bool removed = false;
    for (auto g = body->groups.begin(); g != body->groups.end() && !removed;) {
        for (auto trace = g->traces.begin(); trace != g->traces.end(); ++trace) {
            if (sameName(trace->channel, us(t->channel))) {
                g->traces.erase(trace);
                removed = true;
                break;
            }
        }
        if (removed && g->traces.empty()) {
            g = body->groups.erase(g);
        } else {
            ++g;
        }
    }
    if (!removed) {
        return false;
    }
    project::ComponentSpec spec = t->component->spec();
    spec.body = body->toJson();
    spec.activeChannel = replacement;
    requestSpec(*t->component, spec, tr("Remove %1").arg(t->channel));
    return true;
}

// ---- the maths editors ------------------------------------------------------------------------------------

bool ActiveChannelCommands::editMaths() {
    lastHint_.clear();
    const std::optional<Target> t = target();
    if (!t) {
        return false;
    }
    if (t->source == nullptr) {
        return fail(tr("No data is selected: '%1' has no maths item to show.").arg(t->channel));
    }
    // The item that defines the channel: the head of its chain in the Main source.
    const std::string name = us(t->channel);
    std::optional<qt::MathsService::StatusRow> head;
    for (const qt::MathsService::StatusRow& r : maths_.statusRows()) {
        if (r.source == t->source->id && model::canonicalLayerName(r.layer) == t->layer &&
            sameName(r.name, name) && r.descriptor.has_value()) {
            head = r;
            break;
        }
    }
    if (!head) {
        return fail(tr("'%1' is a logged channel: it has no maths item.").arg(t->channel));
    }
    if (head->kind == maths::ItemKind::Builtin || head->moduleId.starts_with("esses.")) {
        return fail(tr("'%1' is calculated by the built-in module %2.").arg(t->channel, qs(head->moduleId)));
    }
    MathsEditor* editor = host_.mathsEditor ? host_.mathsEditor() : nullptr;
    if (editor == nullptr) {
        return false;
    }
    editor->setMainSource(t->source->id);
    MathsEditor::ModuleRef ref;
    if (head->moduleId == maths::MathModule::kLocalId) {
        ref.scope = MathsEditor::ModuleRef::Scope::Local;
        ref.source = t->source->id;
    } else if (head->moduleId == maths::MathModule::kTransientId) {
        ref.scope = MathsEditor::ModuleRef::Scope::Transient;
        ref.source = t->source->id;
    } else {
        ref.moduleId = qs(head->moduleId);
    }
    if (!editor->openAt(ref, qs(head->name))) {
        return fail(tr("The maths item of '%1' cannot be shown now.").arg(t->channel));
    }
    return true;
}

bool ActiveChannelCommands::showStatus() {
    lastHint_.clear();
    const std::optional<Target> t = target();
    if (!t) {
        return false;
    }
    ChannelStatusWindow* window = host_.channelStatus ? host_.channelStatus() : nullptr;
    if (window == nullptr) {
        return false;
    }
    if (t->source != nullptr &&
        window->focusChannel(maths::ChannelKey::make(t->source->id, t->layer, us(t->channel)))) {
        return true;
    }
    // Aliases and mappings have rows too (by name).
    if (window->selectRow(t->channel, QStringLiteral("Alias")) ||
        window->selectRow(t->channel, QStringLiteral("Mapping"))) {
        return true;
    }
    return fail(tr("'%1' is a logged channel: Channel Status has no row for it.").arg(t->channel));
}

// ---- transient and local maths ----------------------------------------------------------------------------

std::optional<maths::MathModule> ActiveChannelCommands::transientModule(const model::SourceId& source) const {
    const auto it = transient_.find(source);
    if (it == transient_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool ActiveChannelCommands::filterActive() {
    lastHint_.clear();
    const std::optional<Target> t = target();
    if (!t) {
        return false;
    }
    if (t->source == nullptr) {
        return fail(tr("No data is selected: the filter applies to the Main source."));
    }
    const std::optional<FilterChoice> choice = filterPrompt_ ? filterPrompt_(t->channel) : std::nullopt;
    if (!choice || (!choice->points && !choice->seconds) || (choice->points && *choice->points < 1) ||
        (choice->seconds && !(*choice->seconds > 0.0))) {
        return false;
    }
    maths::MathModule& m = transient_[t->source->id];
    m.id = std::string(maths::MathModule::kTransientId);
    m.origin = maths::MathModule::Origin::Transient;
    const std::string name = us(t->channel);
    auto it = std::find_if(m.items.begin(), m.items.end(), [&name](const maths::MathItem& i) {
        return i.kind == maths::ItemKind::Filter && sameName(i.name, name);
    });
    if (it == m.items.end()) {
        maths::MathItem item;
        item.kind = maths::ItemKind::Filter;
        item.name = name;
        item.comment = "filter from the channel label";
        m.items.push_back(std::move(item));
        it = std::prev(m.items.end());
    }
    it->window.points = choice->points;
    it->window.seconds = choice->points ? std::nullopt : choice->seconds;
    maths_.setTransientModule(t->source->id, m);
    return true;
}

bool ActiveChannelCommands::writeLocalItem(const Target& t,
                                           const std::function<void(maths::MathItem&, bool existed)>& edit,
                                           const QString& undoText) {
    const std::shared_ptr<const units::UnitRegistry>& reg = maths_.units();
    if (t.source == nullptr || reg == nullptr) {
        return fail(tr("No data is selected: local maths belongs to the Main source."));
    }
    maths::MathModule local;
    local.id = std::string(maths::MathModule::kLocalId);
    if (const std::optional<JsonDoc>& doc = t.source->sidecar.layer(t.layer).localMaths;
        doc && !doc->isNull()) {
        std::vector<std::string> log;
        Result<maths::MathModule> parsed = maths::MathModuleIo::fromJson(*doc, *reg, log);
        if (!parsed) {
            return fail(
                tr("The local maths of this file cannot be read: %1").arg(qs(parsed.error().toString())));
        }
        local = std::move(parsed).value();
    }
    local.origin = maths::MathModule::Origin::Local;
    const std::string name = us(t.channel);
    auto it = std::find_if(local.items.begin(), local.items.end(), [&name](const maths::MathItem& i) {
        return i.kind == maths::ItemKind::ScaleOffset && sameName(i.name, name);
    });
    const bool existed = it != local.items.end();
    if (!existed) {
        maths::MathItem item;
        item.kind = maths::ItemKind::ScaleOffset;
        item.name = name;
        local.items.push_back(std::move(item));
        it = std::prev(local.items.end());
    }
    edit(*it, existed);
    Result<model::OpaqueJson> json = maths::MathModuleIo::toJsonDoc(local, *reg);
    if (!json) {
        return fail(tr("The local maths cannot be written: %1").arg(qs(json.error().toString())));
    }
    session_.undoStack().push(
        new qt::SetLocalMathsCommand(loader_, *t.source, t.layer, std::move(json).value(), undoText));
    return true;
}

namespace {

// The decimals the channel shows: the component's, the override's, else the channel's own.
int channelDps(const qt::ProjectSession& session, const qt::MathsService& maths, const model::DataSource& src,
               const std::string& layer, const qt::ChannelRequest& request) {
    if (request.dps) {
        return *request.dps;
    }
    if (const project::ChannelOverrides::Entry* o = session.overrides().find(request.name);
        o != nullptr && o->dps) {
        return *o->dps;
    }
    if (const auto d = maths.descriptor(maths::ChannelKey::make(src.id, layer, request.name))) {
        return d->dps;
    }
    if (const model::DataLayer* l = src.layer(layer)) {
        if (const model::Channel* c = l->find(request.name)) {
            return c->desc.dps;
        }
    }
    return 2;
}

// The display unit the channel is shown in by the component (its trace's), else the stored unit's symbol.
QString displayUnitOf(const qt::MathsService& maths, const model::DataSource& src, const std::string& layer,
                      const qt::ChannelRequest& request) {
    if (!request.displayUnit.empty()) {
        return qs(request.displayUnit);
    }
    const units::UnitRegistry* reg = maths.units().get();
    units::UnitId unit = units::kNoUnit;
    if (const auto d = maths.descriptor(maths::ChannelKey::make(src.id, layer, request.name))) {
        unit = d->unit;
    } else if (const model::DataLayer* l = src.layer(layer)) {
        if (const model::Channel* c = l->find(request.name)) {
            unit = c->desc.unit;
        }
    }
    return reg != nullptr && unit != units::kNoUnit && reg->contains(unit) ? qs(reg->unit(unit).symbol)
                                                                           : QString();
}

} // namespace

bool ActiveChannelCommands::scaleOffsetActive() {
    lastHint_.clear();
    const std::optional<Target> t = target();
    if (!t) {
        return false;
    }
    if (t->source == nullptr) {
        return fail(tr("No data is selected: local maths belongs to the Main source."));
    }
    const QString unit = displayUnitOf(maths_, *t->source, t->layer, t->request);
    const std::optional<ScaleOffsetChoice> choice =
        scaleOffsetPrompt_ ? scaleOffsetPrompt_(t->channel, unit) : std::nullopt;
    if (!choice || !std::isfinite(choice->scale) || !std::isfinite(choice->offset)) {
        return false;
    }
    const units::UnitRegistry* reg = maths_.units().get();
    units::UnitId offsetUnit = units::kNoUnit;
    if (!choice->offsetUnit.trimmed().isEmpty()) {
        const std::optional<units::UnitId> u =
            reg != nullptr ? reg->find(us(choice->offsetUnit.trimmed())) : std::nullopt;
        if (!u) {
            return fail(tr("Unknown unit '%1'.").arg(choice->offsetUnit.trimmed()));
        }
        offsetUnit = *u;
    }
    const int dps = channelDps(session_, maths_, *t->source, t->layer, t->request);
    return writeLocalItem(
        *t,
        [&](maths::MathItem& item, bool) {
            item.scale = choice->scale;
            item.offset = maths::UnitValue{choice->offset, offsetUnit};
            item.display.dps = dps; // DPS preserved (03 section 4.2)
        },
        tr("Scale and offset %1").arg(t->channel));
}

bool ActiveChannelCommands::zeroAtCursor() {
    lastHint_.clear();
    const std::optional<Target> t = target();
    if (!t) {
        return false;
    }
    if (t->source == nullptr) {
        return fail(tr("No data is selected: local maths belongs to the Main source."));
    }
    qt::WorksheetViewModel* worksheet = host_.activeWorksheet ? host_.activeWorksheet() : nullptr;
    if (worksheet == nullptr) {
        return false;
    }
    // The displayed value at the cursor, through the Values window's single conversion rule (04 section 3.2).
    qt::ValuesModel values(selection_, maths_);
    values.setChannelRequests({t->request});
    values.setWorksheet(worksheet);
    values.refresh(
        static_cast<plot::ChangeMask>(plot::kDynamicOnly | plot::kStaticRebuild | plot::kFullRebuild));
    const QString text = values.cellText(0, qt::ValuesModel::MainCol);
    const QString unitText = values.cellText(0, qt::ValuesModel::UnitCol);
    bool ok = false;
    const double shown = QLocale::c().toDouble(text, &ok);
    if (!ok || !std::isfinite(shown)) {
        return fail(tr("The value of '%1' at the cursor is not available yet.").arg(t->channel));
    }
    const units::UnitRegistry* reg = maths_.units().get();
    const units::UnitId unit = reg != nullptr && !unitText.isEmpty()
                                   ? reg->find(us(unitText)).value_or(units::kNoUnit)
                                   : units::kNoUnit;
    const int dps = channelDps(session_, maths_, *t->source, t->layer, t->request);
    return writeLocalItem(
        *t,
        [&](maths::MathItem& item, bool existed) {
            double previous = 0.0;
            if (existed) {
                // The displayed value already includes the item's offset: keep it as a delta in this unit.
                previous = item.offset.v;
                if (reg != nullptr && item.offset.unit != unit) {
                    if (const Result<units::Converter> c = reg->converter(item.offset.unit, unit)) {
                        previous = c.value().a * item.offset.v;
                    }
                }
            } else {
                item.scale = 1.0;
            }
            item.offset = maths::UnitValue{previous - shown, unit};
            item.display.dps = dps; // DPS preserved
        },
        tr("Zero %1 at the cursor").arg(t->channel));
}

} // namespace esses::widgets
