// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathsEditor (03 section 11, 06 section 13.2): panes, item pages, live diagnostics, the unit popup, Insert
// Range / Insert Channel, hover help, the apply / switch / close rules and the auto-save on close.

#include "expression_assist.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/utf8.hpp>
#include <esses/maths/function_registry.hpp>
#include <esses/maths/math_module.hpp>
#include <esses/maths/math_module_io.hpp>
#include <esses/maths/math_status.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/resolver.hpp>
#include <esses/model/source.hpp>
#include <esses/qtmodel/live_diagnostics.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/project_session.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/units/unit_registry.hpp>
#include <esses/widgets/editors_maths/maths_editor.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace esses::widgets {

namespace {

constexpr int kStatusRefreshMs = 100; // the status column follows statusChanged at 10 Hz (06 section 10)
constexpr const char* kLevelProperty = "esses.level";
constexpr const char* kTextEntryProperty = "esses.textEntry";

QString qs(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

std::string us(const QString& s) {
    return s.toStdString();
}

QString kindName(maths::ItemKind k) {
    switch (k) {
    case maths::ItemKind::Expression:
        return MathsEditor::tr("Expression");
    case maths::ItemKind::Filter:
        return MathsEditor::tr("Filter");
    case maths::ItemKind::ScaleOffset:
        return MathsEditor::tr("Scale & Offset");
    case maths::ItemKind::Table2D:
        return MathsEditor::tr("2D Table");
    case maths::ItemKind::Table3D:
        return MathsEditor::tr("3D Table");
    case maths::ItemKind::Builtin:
        return MathsEditor::tr("Built-in");
    case maths::ItemKind::Plugin:
        return MathsEditor::tr("Plugin");
    case maths::ItemKind::Regression:
        return MathsEditor::tr("Regression");
    }
    return MathsEditor::tr("Expression");
}

QString levelName(maths::Diagnostic::Level level) {
    switch (level) {
    case maths::Diagnostic::Error:
        return QStringLiteral("error");
    case maths::Diagnostic::Warning:
        return QStringLiteral("warning");
    case maths::Diagnostic::Info:
        return QStringLiteral("info");
    case maths::Diagnostic::Style:
        return QStringLiteral("style");
    }
    return QStringLiteral("info");
}

// Errors red, Warnings amber, Info grey (03 section 11 item 2); readable on light and dark backgrounds.
QColor levelColour(maths::Diagnostic::Level level) {
    switch (level) {
    case maths::Diagnostic::Error:
        return QColor(0xD3, 0x2F, 0x2F);
    case maths::Diagnostic::Warning:
        return QColor(0xC7, 0x7C, 0x02);
    case maths::Diagnostic::Info:
    case maths::Diagnostic::Style:
        return QColor(0x80, 0x80, 0x80);
    }
    return QColor(0x80, 0x80, 0x80);
}

QString statusText(const maths::MathStatus& st) {
    switch (st.state) {
    case maths::MathState::Pending:
        return MathsEditor::tr("Pending");
    case maths::MathState::Evaluating:
        return MathsEditor::tr("Calculating");
    case maths::MathState::Ok:
        return MathsEditor::tr("OK");
    case maths::MathState::Warning:
        return MathsEditor::tr("Warning: %1").arg(qs(st.summary));
    case maths::MathState::Error:
        return MathsEditor::tr("Error: %1").arg(qs(st.summary));
    case maths::MathState::Disabled:
        return st.summary.starts_with("Disabled") ? qs(st.summary) : MathsEditor::tr("Disabled");
    }
    return {};
}

maths::Diagnostic makeDiag(const char* code, maths::Diagnostic::Level level, const QString& message) {
    maths::Diagnostic d;
    d.code = code;
    d.level = level;
    d.message = us(message);
    return d;
}

const maths::Diagnostic* firstOfLevel(const maths::Diagnostics& diags, maths::Diagnostic::Level level) {
    const auto it = std::find_if(diags.begin(), diags.end(),
                                 [level](const maths::Diagnostic& d) { return d.level == level; });
    return it == diags.end() ? nullptr : &*it;
}

// The most severe diagnostic (Errors first), or nullptr.
const maths::Diagnostic* headline(const maths::Diagnostics& diags) {
    for (const maths::Diagnostic::Level level : {maths::Diagnostic::Error, maths::Diagnostic::Warning,
                                                 maths::Diagnostic::Info, maths::Diagnostic::Style}) {
        if (const maths::Diagnostic* d = firstOfLevel(diags, level)) {
            return d;
        }
    }
    return nullptr;
}

QString sourceLabel(const model::DataSource& src) {
    const std::string name = pathToUtf8(src.prov.path.filename());
    return name.empty() ? qs(src.id.hex().substr(0, 8)) : qs(name);
}

// A module id is the file stem of maths/<id>.maths.json: letters, digits, '.', '_', '-' and inner blanks;
// "local" and ids starting with '~' or '.' are reserved.
bool validModuleId(const QString& id) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_. -]*$"));
    return !id.isEmpty() && id == id.trimmed() && re.match(id).hasMatch() &&
           id.compare(QString::fromLatin1(maths::MathModule::kLocalId.data(),
                                          static_cast<qsizetype>(maths::MathModule::kLocalId.size())),
                      Qt::CaseInsensitive) != 0;
}

template <class Names> QString uniqueName(const QString& base, const Names& taken) {
    const auto used = [&taken](const QString& n) {
        const std::string cf = caseFold(us(n));
        return std::any_of(taken.begin(), taken.end(),
                           [&cf](const std::string& t) { return caseFold(t) == cf; });
    };
    if (!used(base)) {
        return base;
    }
    for (int i = 2;; ++i) {
        const QString candidate = QStringLiteral("%1 %2").arg(base).arg(i);
        if (!used(candidate)) {
            return candidate;
        }
    }
}

Rate rateOfHz(double hz) {
    if (hz <= 0.0) {
        return Rate{};
    }
    const double rounded = std::round(hz);
    if (std::abs(hz - rounded) < 1e-9 && rounded <= 4.0e9) {
        return Rate::fromHz(static_cast<std::uint32_t>(rounded));
    }
    return Rate{static_cast<std::uint32_t>(std::llround(hz * 1000.0)), 1000}.normalised();
}

} // namespace

// ---- Impl -------------------------------------------------------------------------------------------------

struct MathsEditor::Impl {
    explicit Impl(MathsEditor& editor) : q(editor) {}

    struct Entry {
        bool constant = false;
        int index = -1; // -1: a new item or constant not applied yet
        friend bool operator==(const Entry&, const Entry&) = default;
    };
    enum TreeRole : int { ScopeRole = Qt::UserRole + 1, ModuleIdRole, SourceLoRole, SourceHiRole };
    enum Column : int { EnabledCol, KindCol, NameCol, RateCol, UnitCol, StatusCol, CommentCol, ColumnCount };

    MathsEditor& q;

    // widgets
    QFrame* bar = nullptr;
    QLabel* barText = nullptr;
    QPushButton* barRevert = nullptr;
    QTreeWidget* modulesTree = nullptr;
    QLineEdit* conditionEdit = nullptr;
    QTableWidget* itemsTable = nullptr;
    QLabel* kindLabel = nullptr;
    QLabel* nameLabel = nullptr;
    QLineEdit* nameEdit = nullptr;
    QLineEdit* commentEdit = nullptr;
    QStackedWidget* pages = nullptr;
    QWidget* emptyPage = nullptr;
    QWidget* exprPage = nullptr;
    QComboBox* quantityBox = nullptr;
    QComboBox* resultUnitBox = nullptr;
    QComboBox* displayUnitBox = nullptr;
    QCheckBox* rateAutoBox = nullptr;
    QDoubleSpinBox* rateHzBox = nullptr;
    QSpinBox* dpsBox = nullptr;
    QSpinBox* colourBox = nullptr;
    QComboBox* interpBox = nullptr;
    QPlainTextEdit* exprEdit = nullptr;
    QLabel* diagLine = nullptr;
    QListWidget* diagList = nullptr;
    QToolButton* insertRangeButton = nullptr;
    QToolButton* insertChannelButton = nullptr;
    QWidget* constPage = nullptr;
    QLineEdit* constValueEdit = nullptr;
    QComboBox* constUnitBox = nullptr;
    QWidget* filterPage = nullptr;
    QComboBox* filterModeBox = nullptr;
    QDoubleSpinBox* filterValueBox = nullptr;
    QWidget* soPage = nullptr;
    QDoubleSpinBox* soScaleBox = nullptr;
    QDoubleSpinBox* soOffsetBox = nullptr;
    QComboBox* soOffsetUnitBox = nullptr;
    QWidget* otherPage = nullptr;
    QLabel* otherText = nullptr;
    QLabel* statusLabel = nullptr;
    QPushButton* applyButton = nullptr;
    QPushButton* revertButton = nullptr;
    QListWidget* unitPopup = nullptr;
    QTimer* statusTimer = nullptr;
    std::unique_ptr<qt::LiveDiagnostics> live;

    // state
    std::optional<model::SourceId> main;
    std::optional<ModuleRef> module;
    std::vector<Entry> rows;
    std::optional<Entry> edited;
    maths::ItemKind newKind = maths::ItemKind::Expression; // kind of a new (index -1) item
    QString editedName;
    bool dirty = false;
    int loading = 0;
    bool modulesEdited = false; // a global module edit was pushed: auto-save on close
    std::vector<model::SourceId> listedTransient;
    maths::Diagnostics exprDiags;

    static QString tr(const char* text) { return MathsEditor::tr(text); }

    qt::ProjectSession& session() const { return q.session_; }
    qt::MathsService& maths() const { return q.maths_; }
    qt::SourceLoader* loader() const { return q.loader_; }
    const units::UnitRegistry* units() const { return maths().units().get(); }

    // ---- sources and scope --------------------------------------------------------------------------------

    std::vector<std::shared_ptr<const model::DataSource>> sources() const {
        if (loader() != nullptr) {
            return loader()->sources();
        }
        std::vector<std::shared_ptr<const model::DataSource>> out;
        for (const model::SourceId& id : maths().sources()) {
            if (auto src = maths().source(id)) {
                out.push_back(std::move(src));
            }
        }
        return out;
    }

    std::shared_ptr<const model::DataSource> sourceOf(const model::SourceId& id) const {
        if (loader() != nullptr) {
            if (auto src = loader()->source(id)) {
                return src;
            }
        }
        return maths().source(id);
    }

    std::optional<model::SourceId> effectiveMain() const {
        if (main && sourceOf(*main) != nullptr) {
            return main;
        }
        const auto all = sources();
        if (!all.empty()) {
            return all.front()->id;
        }
        return std::nullopt;
    }

    // The scope of the edited module: its own source for local and transient maths, else the Main source.
    std::optional<model::SourceId> scopeSource() const {
        if (module && module->scope != ModuleRef::Scope::Global && sourceOf(module->source) != nullptr) {
            return module->source;
        }
        return effectiveMain();
    }

    std::shared_ptr<const model::DataSource> scope() const {
        const std::optional<model::SourceId> id = scopeSource();
        return id ? sourceOf(*id) : nullptr;
    }

    // ---- module documents ---------------------------------------------------------------------------------

    std::optional<maths::MathModule> localModule(const model::SourceId& id) const {
        const auto src = sourceOf(id);
        if (src == nullptr || units() == nullptr) {
            return std::nullopt;
        }
        maths::MathModule m;
        m.id = std::string(maths::MathModule::kLocalId);
        const std::optional<JsonDoc>& doc = src->sidecar.layer(model::kNormalLayer).localMaths;
        if (doc && !doc->isNull()) {
            std::vector<std::string> log;
            Result<maths::MathModule> parsed = maths::MathModuleIo::fromJson(*doc, *units(), log);
            if (!parsed) {
                return std::nullopt;
            }
            m = std::move(parsed).value();
        }
        m.origin = maths::MathModule::Origin::Local;
        if (m.id.empty()) {
            m.id = std::string(maths::MathModule::kLocalId);
        }
        return m;
    }

    // Transient maths is listed from the status rows (names and kinds): it is owned by the channel commands.
    std::optional<maths::MathModule> transientModule(const model::SourceId& id) const {
        maths::MathModule m;
        m.id = std::string(maths::MathModule::kTransientId);
        m.origin = maths::MathModule::Origin::Transient;
        for (const qt::MathsService::StatusRow& r : maths().statusRows()) {
            if (r.source == id && r.moduleId == maths::MathModule::kTransientId &&
                model::canonicalLayerName(r.layer) == model::kNormalLayer) {
                maths::MathItem item;
                item.kind = r.kind;
                item.name = r.name;
                m.items.push_back(std::move(item));
            }
        }
        if (m.items.empty()) {
            return std::nullopt;
        }
        return m;
    }

    std::optional<maths::MathModule> moduleDoc(const ModuleRef& ref) const {
        switch (ref.scope) {
        case ModuleRef::Scope::Global:
            for (const maths::MathModule& m : session().mathsModules()) {
                if (qs(m.id) == ref.moduleId) {
                    return m;
                }
            }
            return std::nullopt;
        case ModuleRef::Scope::Local:
            return localModule(ref.source);
        case ModuleRef::Scope::Transient:
            return transientModule(ref.source);
        }
        return std::nullopt;
    }

    bool writable(const ModuleRef& ref) const {
        switch (ref.scope) {
        case ModuleRef::Scope::Global:
            return true;
        case ModuleRef::Scope::Local:
            return loader() != nullptr && loader()->source(ref.source) != nullptr && localModule(ref.source);
        case ModuleRef::Scope::Transient:
            return false;
        }
        return false;
    }

    bool writeGlobals(std::vector<maths::MathModule> all, const QString& text) {
        if (all == session().mathsModules()) {
            return true;
        }
        modulesEdited = true;
        session().undoStack().push(new qt::EditMathModulesCommand(session(), std::move(all), text));
        return true;
    }

    bool writeModule(const ModuleRef& ref, maths::MathModule after, const QString& text) {
        if (!writable(ref)) {
            return false;
        }
        if (ref.scope == ModuleRef::Scope::Global) {
            std::vector<maths::MathModule> all = session().mathsModules();
            const auto it = std::find_if(all.begin(), all.end(), [&ref](const maths::MathModule& m) {
                return qs(m.id) == ref.moduleId;
            });
            if (it == all.end()) {
                return false;
            }
            *it = std::move(after);
            return writeGlobals(std::move(all), text);
        }
        const auto src = loader()->source(ref.source);
        if (src == nullptr || units() == nullptr) {
            return false;
        }
        std::optional<model::OpaqueJson> doc;
        if (!after.items.empty() || !after.constants.empty() || !after.enumerations.empty() ||
            after.condition || !after.unknown.isNull()) {
            after.id = std::string(maths::MathModule::kLocalId);
            after.origin = maths::MathModule::Origin::Local;
            Result<model::OpaqueJson> json = maths::MathModuleIo::toJsonDoc(after, *units());
            if (!json) {
                showBar(tr("The local maths cannot be written: %1").arg(qs(json.error().toString())), false);
                return false;
            }
            doc = std::move(json).value();
        }
        if (doc == src->sidecar.layer(model::kNormalLayer).localMaths) {
            return true;
        }
        session().undoStack().push(new qt::SetLocalMathsCommand(
            *loader(), *src, std::string(model::kNormalLayer), std::move(doc), text));
        return true;
    }

    // ---- quantities and units -----------------------------------------------------------------------------

    std::optional<units::QuantityId> quantityOfName(const QString& name) const {
        const units::UnitRegistry* reg = units();
        if (reg == nullptr || name.isEmpty()) {
            return std::nullopt;
        }
        const std::string n = us(name);
        const std::string cf = caseFold(n);
        if (const auto src = scope()) {
            const maths::ChannelKey key = maths::ChannelKey::make(src->id, model::kNormalLayer, n);
            if (const std::optional<model::ChannelDescriptor> d = maths().descriptor(key)) {
                return d->quantity != units::kUnitless ? d->quantity : reg->quantityOf(d->unit);
            }
            if (const model::DataLayer* layer = src->layer(model::kNormalLayer)) {
                const model::ChannelResolver resolver(session().mappingTable(), session().aliasTable(), reg);
                const model::ChannelResolver::Resolution res = resolver.resolve(n, *layer, src->id);
                if (res.channel != nullptr) {
                    return res.channel->desc.quantity != units::kUnitless
                               ? res.channel->desc.quantity
                               : reg->quantityOf(res.channel->desc.unit);
                }
            }
            for (const model::Detail& d : src->details) {
                if (caseFold(d.id) == cf) {
                    if (const auto* qv = std::get_if<model::Quantityd>(&d.value);
                        qv != nullptr && reg->contains(qv->unit)) {
                        return reg->quantityOf(qv->unit);
                    }
                }
            }
        }
        const auto fromModule = [&](const maths::MathModule& m) -> std::optional<units::QuantityId> {
            for (const maths::MathItem& item : m.items) {
                if (caseFold(item.name) == cf) {
                    if (item.quantity != units::kUnitless) {
                        return item.quantity;
                    }
                    if (item.resultUnit != units::kNoUnit && reg->contains(item.resultUnit)) {
                        return reg->quantityOf(item.resultUnit);
                    }
                }
            }
            for (const maths::MathConstant& c : m.constants) {
                if (caseFold(c.name) == cf && reg->contains(c.value.unit)) {
                    return reg->quantityOf(c.value.unit);
                }
            }
            return std::nullopt;
        };
        if (module && module->scope == ModuleRef::Scope::Local) {
            if (const auto local = localModule(module->source)) {
                if (auto qid = fromModule(*local)) {
                    return qid;
                }
            }
        }
        for (const maths::MathModule& m : session().mathsModules()) {
            if (auto qid = fromModule(m)) {
                return qid;
            }
        }
        return std::nullopt;
    }

    QStringList unitSymbols(units::QuantityId quantity) const {
        QStringList out;
        const units::UnitRegistry* reg = units();
        if (reg == nullptr || !reg->containsQuantity(quantity)) {
            return out;
        }
        for (const units::UnitId u : reg->unitsOf(quantity)) {
            const units::Unit& unit = reg->unit(u);
            if (!unit.isCompat() && !unit.symbol.empty()) {
                out << qs(unit.symbol);
            }
        }
        return out;
    }

    bool unitKnown(const QString& text) const {
        const QString t = text.trimmed();
        return t.isEmpty() || (units() != nullptr && units()->find(us(t)).has_value());
    }

    units::UnitId unitOfText(const QString& text) const {
        const QString t = text.trimmed();
        if (t.isEmpty() || units() == nullptr) {
            return units::kNoUnit;
        }
        return units()->find(us(t)).value_or(units::kNoUnit);
    }

    QString unitText(units::UnitId id) const {
        if (units() == nullptr || id == units::kNoUnit || !units()->contains(id)) {
            return {};
        }
        return qs(units()->unit(id).symbol);
    }

    void fillUnitCombo(QComboBox* box, units::QuantityId quantity, const QString& noneText,
                       units::UnitId select) {
        const QSignalBlocker block(box);
        box->clear();
        box->addItem(noneText, QVariant::fromValue<uint>(units::kNoUnit));
        const units::UnitRegistry* reg = units();
        if (reg != nullptr && quantity != units::kUnitless && reg->containsQuantity(quantity)) {
            for (const units::UnitId u : reg->unitsOf(quantity)) {
                const units::Unit& unit = reg->unit(u);
                if (!unit.isCompat() || u == select) {
                    box->addItem(qs(unit.symbol), QVariant::fromValue<uint>(u));
                }
            }
        }
        const int at = box->findData(QVariant::fromValue<uint>(select));
        box->setCurrentIndex(std::max(at, 0));
    }

    void fillUnitTextCombo(QComboBox* box, const QStringList& symbols, const QString& current) {
        const QSignalBlocker block(box);
        box->clear();
        box->addItem(QString());
        box->addItems(symbols);
        box->setEditText(current);
        const int at = box->findText(current);
        if (at >= 0) {
            box->setCurrentIndex(at);
        }
    }

    // ---- the bar ------------------------------------------------------------------------------------------

    void showBar(const QString& text, bool offerRevert) {
        barText->setText(text);
        barRevert->setVisible(offerRevert);
        bar->show();
    }

    void hideBar() {
        barText->clear();
        bar->hide();
    }

    // ---- diagnostics --------------------------------------------------------------------------------------

    std::optional<maths::ItemKind> editedKind() const {
        if (!edited || edited->constant) {
            return std::nullopt;
        }
        if (edited->index < 0) {
            return newKind;
        }
        if (!module) {
            return std::nullopt;
        }
        const auto doc = moduleDoc(*module);
        if (!doc || edited->index >= static_cast<int>(doc->items.size())) {
            return std::nullopt;
        }
        return doc->items[static_cast<std::size_t>(edited->index)].kind;
    }

    maths::Diagnostics pageDiagnostics() const {
        maths::Diagnostics out;
        if (!edited || !module || !writable(*module)) {
            return out;
        }
        const QString name = nameEdit->text().trimmed();
        if (name.isEmpty()) {
            out.push_back(makeDiag("E-NAME-EMPTY", maths::Diagnostic::Error,
                                   edited->constant ? tr("a constant needs a name")
                                                    : tr("a maths item needs an output name")));
        }
        if (edited->constant) {
            bool ok = false;
            const double v = QLocale::c().toDouble(constValueEdit->text().trimmed(), &ok);
            if (!ok || !std::isfinite(v)) {
                out.push_back(makeDiag("E-NUMBER-FORMAT", maths::Diagnostic::Error,
                                       tr("the value '%1' is not a number").arg(constValueEdit->text())));
            }
            if (!unitKnown(constUnitBox->currentText())) {
                out.push_back(makeDiag("E-UNIT-UNKNOWN", maths::Diagnostic::Error,
                                       tr("unknown unit '%1'").arg(constUnitBox->currentText().trimmed())));
            }
            return out;
        }
        const std::optional<maths::ItemKind> kind = editedKind();
        if (kind == maths::ItemKind::Filter || kind == maths::ItemKind::ScaleOffset) {
            if (!name.isEmpty()) {
                // The channel the item reads: unknown with data loaded is an Error, I-UNRESOLVED without.
                for (maths::Diagnostic d :
                     maths().check(us(QLatin1Char('\'') + name + QLatin1Char('\'')), scopeSource())) {
                    if (d.code == "E-UNKNOWN-CHANNEL" || d.code == "I-UNRESOLVED") {
                        d.span = {};
                        out.push_back(std::move(d));
                    }
                }
            }
            if (kind == maths::ItemKind::Filter && filterValueBox->value() <= 0.0) {
                out.push_back(
                    makeDiag("E-ARG-VALUE", maths::Diagnostic::Error,
                             tr("the filter window must be at least one sample or a positive time")));
            }
            if (kind == maths::ItemKind::ScaleOffset && !unitKnown(soOffsetUnitBox->currentText())) {
                out.push_back(
                    makeDiag("E-UNIT-UNKNOWN", maths::Diagnostic::Error,
                             tr("unknown unit '%1'").arg(soOffsetUnitBox->currentText().trimmed())));
            }
        }
        return out;
    }

    // Page and expression diagnostics; `sync` checks the expression now instead of using the debounced
    // result.
    maths::Diagnostics allDiagnostics(bool sync) const {
        maths::Diagnostics out = pageDiagnostics();
        if (edited && module && writable(*module) && editedKind() == maths::ItemKind::Expression) {
            const maths::Diagnostics expr =
                sync ? maths().check(us(exprEdit->toPlainText()), scopeSource()) : exprDiags;
            out.insert(out.end(), expr.begin(), expr.end());
        }
        return out;
    }

    static QString describe(const maths::Diagnostic& d, const QString& text) {
        QString where;
        if (d.span.end > d.span.begin || d.span.begin > 0) {
            where = tr(" at column %1").arg(assist::charPosition(text, d.span.begin) + 1);
        }
        return QStringLiteral("%1%2: %3").arg(qs(d.code), where, qs(d.message));
    }

    void updateDiagnosticsUi() {
        const maths::Diagnostics all = allDiagnostics(false);
        const QString text = exprEdit->toPlainText();
        diagList->clear();
        for (const maths::Diagnostic& d : all) {
            auto* row = new QListWidgetItem(describe(d, text), diagList);
            row->setForeground(levelColour(d.level));
            row->setData(Qt::UserRole, levelName(d.level));
        }
        QPalette pal = diagLine->palette();
        if (const maths::Diagnostic* top = headline(all)) {
            diagLine->setText(describe(*top, text));
            diagLine->setProperty(kLevelProperty, levelName(top->level));
            pal.setColor(QPalette::WindowText, levelColour(top->level));
        } else {
            diagLine->setText(edited ? tr("No problems") : QString());
            diagLine->setProperty(kLevelProperty, QStringLiteral("ok"));
            pal.setColor(QPalette::WindowText, q.palette().color(QPalette::WindowText));
        }
        diagLine->setPalette(pal);
        // Underline every span of the expression.
        QList<QTextEdit::ExtraSelection> marks;
        if (editedKind() == maths::ItemKind::Expression) {
            for (const maths::Diagnostic& d : exprDiags) {
                int from = assist::charPosition(text, d.span.begin);
                int to = assist::charPosition(text, d.span.end);
                if (to <= from) {
                    if (text.isEmpty()) {
                        continue;
                    }
                    from = std::min(from, static_cast<int>(text.size()) - 1);
                    to = from + 1;
                }
                QTextEdit::ExtraSelection s;
                s.cursor = QTextCursor(exprEdit->document());
                s.cursor.setPosition(from);
                s.cursor.setPosition(to, QTextCursor::KeepAnchor);
                s.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
                s.format.setUnderlineColor(levelColour(d.level));
                s.format.setToolTip(describe(d, text));
                marks.push_back(s);
            }
        }
        exprEdit->setExtraSelections(marks);
    }

    // ---- status -------------------------------------------------------------------------------------------

    QString moduleKey() const {
        if (!module) {
            return {};
        }
        switch (module->scope) {
        case ModuleRef::Scope::Global:
            return module->moduleId;
        case ModuleRef::Scope::Local:
            return qs(maths::MathModule::kLocalId);
        case ModuleRef::Scope::Transient:
            return qs(maths::MathModule::kTransientId);
        }
        return {};
    }

    std::optional<maths::MathStatus> statusOf(const std::vector<qt::MathsService::StatusRow>& status,
                                              const std::string& name) const {
        const std::optional<model::SourceId> src = scopeSource();
        if (!src || !module) {
            return std::nullopt;
        }
        const std::string key = us(moduleKey());
        const std::string cf = caseFold(name);
        for (const qt::MathsService::StatusRow& r : status) {
            if (r.source == *src && r.moduleId == key && caseFold(r.name) == cf &&
                model::canonicalLayerName(r.layer) == model::kNormalLayer) {
                return r.status;
            }
        }
        return std::nullopt;
    }

    void refreshStatus() {
        const std::vector<qt::MathsService::StatusRow> status = maths().statusRows();
        const auto doc = module ? moduleDoc(*module) : std::nullopt;
        ++loading;
        {
            const QSignalBlocker block(itemsTable);
            for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
                const Entry& e = rows[static_cast<std::size_t>(r)];
                QTableWidgetItem* cell = itemsTable->item(r, StatusCol);
                if (cell == nullptr || e.constant || !doc || e.index < 0 ||
                    e.index >= static_cast<int>(doc->items.size())) {
                    continue;
                }
                const std::optional<maths::MathStatus> st =
                    statusOf(status, doc->items[static_cast<std::size_t>(e.index)].name);
                cell->setText(st ? statusText(*st) : QString());
                cell->setForeground(st && st->state == maths::MathState::Error
                                        ? QBrush(levelColour(maths::Diagnostic::Error))
                                    : st && st->state == maths::MathState::Warning
                                        ? QBrush(levelColour(maths::Diagnostic::Warning))
                                        : QBrush());
            }
        }
        --loading;
        QString line;
        if (edited && !edited->constant && edited->index >= 0 && doc &&
            edited->index < static_cast<int>(doc->items.size())) {
            const std::optional<maths::MathStatus> st =
                statusOf(status, doc->items[static_cast<std::size_t>(edited->index)].name);
            line = st ? tr("Status: %1").arg(statusText(*st)) : tr("Status: no data");
        }
        statusLabel->setText(line);
    }

    // ---- panes --------------------------------------------------------------------------------------------

    static std::optional<ModuleRef> refOf(const QTreeWidgetItem* item) {
        if (item == nullptr || !item->data(0, ScopeRole).isValid()) {
            return std::nullopt;
        }
        ModuleRef ref;
        ref.scope = static_cast<ModuleRef::Scope>(item->data(0, ScopeRole).toInt());
        ref.moduleId = item->data(0, ModuleIdRole).toString();
        ref.source.lo = item->data(0, SourceLoRole).toULongLong();
        ref.source.hi = item->data(0, SourceHiRole).toULongLong();
        return ref;
    }

    static void setRef(QTreeWidgetItem* item, const ModuleRef& ref) {
        item->setData(0, ScopeRole, static_cast<int>(ref.scope));
        item->setData(0, ModuleIdRole, ref.moduleId);
        item->setData(0, SourceLoRole, QVariant::fromValue<qulonglong>(ref.source.lo));
        item->setData(0, SourceHiRole, QVariant::fromValue<qulonglong>(ref.source.hi));
    }

    // The open sources that have transient maths, in source order.
    std::vector<model::SourceId> transientSources() const {
        std::vector<model::SourceId> out;
        for (const auto& src : sources()) {
            if (transientModule(src->id)) {
                out.push_back(src->id);
            }
        }
        return out;
    }

    QTreeWidgetItem* findTreeItem(const ModuleRef& ref) const {
        for (QTreeWidgetItemIterator it(modulesTree); *it != nullptr; ++it) {
            if (const auto r = refOf(*it); r && *r == ref) {
                return *it;
            }
        }
        return nullptr;
    }

    void rebuildModules() {
        ++loading;
        {
            const QSignalBlocker block(modulesTree);
            modulesTree->clear();
            for (const maths::MathModule& m : session().mathsModules()) {
                auto* item = new QTreeWidgetItem(modulesTree, {qs(m.id)});
                item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
                item->setCheckState(0, m.enabled ? Qt::Checked : Qt::Unchecked);
                ModuleRef ref;
                ref.moduleId = qs(m.id);
                setRef(item, ref);
                if (m.condition) {
                    item->setToolTip(0, tr("Applies where %1").arg(qs(*m.condition)));
                }
            }
            auto* localNode = new QTreeWidgetItem(modulesTree, {tr("Local maths")});
            localNode->setFlags(Qt::ItemIsEnabled);
            for (const auto& src : sources()) {
                auto* item = new QTreeWidgetItem(localNode, {sourceLabel(*src)});
                item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
                ModuleRef ref;
                ref.scope = ModuleRef::Scope::Local;
                ref.source = src->id;
                setRef(item, ref);
            }
            listedTransient = transientSources();
            if (!listedTransient.empty()) {
                auto* transientNode = new QTreeWidgetItem(modulesTree, {tr("Transient")});
                transientNode->setFlags(Qt::ItemIsEnabled);
                for (const model::SourceId& id : listedTransient) {
                    const auto src = sourceOf(id);
                    auto* item = new QTreeWidgetItem(transientNode, {src ? sourceLabel(*src) : qs(id.hex())});
                    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
                    item->setToolTip(0,
                                     tr("Filters from a channel label: kept for this session, never saved"));
                    ModuleRef ref;
                    ref.scope = ModuleRef::Scope::Transient;
                    ref.source = id;
                    setRef(item, ref);
                }
            }
            modulesTree->expandAll();
            QTreeWidgetItem* current = module ? findTreeItem(*module) : nullptr;
            if (current == nullptr) {
                module.reset();
            } else {
                modulesTree->setCurrentItem(current);
            }
        }
        --loading;
        const std::optional<maths::MathModule> doc = module ? moduleDoc(*module) : std::nullopt;
        const bool global = module && module->scope == ModuleRef::Scope::Global;
        const QSignalBlocker block(conditionEdit);
        conditionEdit->setEnabled(global);
        conditionEdit->setText(global && doc && doc->condition ? qs(*doc->condition) : QString());
    }

    void fillRow(int r, const Entry& e, const maths::MathModule& doc, bool readOnly) {
        const Qt::ItemFlags base = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
        const auto cell = [&](int col, const QString& text) {
            auto* it = new QTableWidgetItem(text);
            it->setFlags(base);
            itemsTable->setItem(r, col, it);
            return it;
        };
        auto* enabled = cell(EnabledCol, QString());
        if (e.index < 0) {
            enabled->setFlags(base);
            cell(KindCol, e.constant ? tr("Constant") : kindName(newKind));
            cell(NameCol, tr("(new)"));
            cell(RateCol, QString());
            cell(UnitCol, QString());
            cell(StatusCol, QString());
            cell(CommentCol, QString());
            return;
        }
        if (!readOnly) {
            enabled->setFlags(base | Qt::ItemIsUserCheckable);
        }
        if (e.constant) {
            const maths::MathConstant& c = doc.constants[static_cast<std::size_t>(e.index)];
            enabled->setCheckState(c.enabled ? Qt::Checked : Qt::Unchecked);
            cell(KindCol, tr("Constant"));
            cell(NameCol, qs(c.name));
            cell(RateCol, QString());
            cell(UnitCol, unitText(c.value.unit));
            cell(StatusCol, QString::number(c.value.v, 'g', 12));
            cell(CommentCol, qs(c.comment));
            return;
        }
        const maths::MathItem& item = doc.items[static_cast<std::size_t>(e.index)];
        enabled->setCheckState(item.enabled ? Qt::Checked : Qt::Unchecked);
        cell(KindCol, kindName(item.kind));
        cell(NameCol, qs(item.name));
        cell(RateCol, item.kind != maths::ItemKind::Expression ? QString()
                      : item.rate.automatic                    ? tr("Auto")
                                            : QString::number(item.rate.rate.hz(), 'g', 6));
        cell(UnitCol, unitText(item.resultUnit));
        cell(StatusCol, QString());
        cell(CommentCol, qs(item.comment));
    }

    void rebuildItems() {
        ++loading;
        {
            const QSignalBlocker block(itemsTable);
            itemsTable->clearContents();
            rows.clear();
            const std::optional<maths::MathModule> doc = module ? moduleDoc(*module) : std::nullopt;
            if (doc) {
                for (int i = 0; i < static_cast<int>(doc->items.size()); ++i) {
                    rows.push_back(Entry{false, i});
                }
                for (int i = 0; i < static_cast<int>(doc->constants.size()); ++i) {
                    rows.push_back(Entry{true, i});
                }
                if (edited && edited->index < 0) {
                    rows.push_back(*edited);
                }
            }
            itemsTable->setRowCount(static_cast<int>(rows.size()));
            const bool readOnly = !module || !writable(*module);
            for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
                fillRow(r, rows[static_cast<std::size_t>(r)], *doc, readOnly);
            }
        }
        --loading;
        refreshStatus();
    }

    int rowOf(const Entry& e) const {
        const auto it = std::find(rows.begin(), rows.end(), e);
        return it == rows.end() ? -1 : static_cast<int>(it - rows.begin());
    }

    // The row of the edited entry after a document change: by kind and name, else by position.
    int findEditedRow() const {
        if (!edited || !module) {
            return -1;
        }
        if (edited->index < 0) {
            return rowOf(*edited);
        }
        const auto doc = moduleDoc(*module);
        if (!doc) {
            return -1;
        }
        const std::string name = us(editedName);
        const auto nameAt = [&doc](const Entry& e) -> const std::string& {
            return e.constant ? doc->constants[static_cast<std::size_t>(e.index)].name
                              : doc->items[static_cast<std::size_t>(e.index)].name;
        };
        // The same position first (names may repeat in a module), then the first entry with the name.
        if (const int same = rowOf(*edited); same >= 0 && nameAt(*edited) == name) {
            return same;
        }
        for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
            const Entry& e = rows[static_cast<std::size_t>(r)];
            if (e.constant == edited->constant && e.index >= 0 && nameAt(e) == name) {
                return r;
            }
        }
        return -1;
    }

    void selectRow(int r) {
        ++loading;
        {
            const QSignalBlocker block(itemsTable);
            const QSignalBlocker blockSel(itemsTable->selectionModel());
            itemsTable->clearSelection();
            if (r >= 0) {
                itemsTable->setCurrentCell(r, NameCol);
                itemsTable->selectRow(r);
            } else {
                itemsTable->setCurrentCell(-1, -1);
            }
        }
        --loading;
    }

    void setPageReadOnly(bool readOnly) {
        for (QWidget* w : std::initializer_list<QWidget*>{
                 nameEdit, commentEdit, quantityBox, resultUnitBox, displayUnitBox, rateAutoBox, rateHzBox,
                 dpsBox, colourBox, interpBox, constValueEdit, constUnitBox, filterModeBox, filterValueBox,
                 soScaleBox, soOffsetBox, soOffsetUnitBox}) {
            w->setEnabled(!readOnly);
        }
        exprEdit->setReadOnly(readOnly);
        insertRangeButton->setEnabled(!readOnly);
        insertChannelButton->setEnabled(!readOnly);
    }

    void loadExpressionFields(const maths::MathItem& item) {
        const units::UnitRegistry* reg = units();
        {
            const QSignalBlocker block(quantityBox);
            const int at = quantityBox->findData(QVariant::fromValue<uint>(item.quantity));
            quantityBox->setCurrentIndex(std::max(at, 0));
        }
        units::QuantityId quantity = item.quantity;
        if (quantity == units::kUnitless && reg != nullptr && item.resultUnit != units::kNoUnit &&
            reg->contains(item.resultUnit)) {
            quantity = reg->quantityOf(item.resultUnit);
        }
        fillUnitCombo(resultUnitBox, quantity, tr("(none)"), item.resultUnit);
        fillUnitCombo(displayUnitBox, quantity, tr("(result unit)"), item.display.unit);
        rateAutoBox->setChecked(item.rate.automatic);
        rateHzBox->setValue(item.rate.rate.valid() ? item.rate.rate.hz() : 100.0);
        rateHzBox->setEnabled(!item.rate.automatic);
        dpsBox->setValue(item.display.dps);
        colourBox->setValue(item.display.colourIndex);
        interpBox->setCurrentIndex(static_cast<int>(item.interp));
        exprEdit->setPlainText(qs(item.expr));
    }

    void loadPage(std::optional<Entry> e) {
        ++loading;
        hideUnitPopup();
        hideBar();
        dirty = false;
        edited = e;
        const std::optional<maths::MathModule> doc = module ? moduleDoc(*module) : std::nullopt;
        nameEdit->clear();
        commentEdit->clear();
        exprEdit->clear();
        nameLabel->setText(tr("Name"));
        const bool readOnly = !module || !writable(*module);
        if (!e || !doc ||
            (e->index >= 0 && (e->constant ? e->index >= static_cast<int>(doc->constants.size())
                                           : e->index >= static_cast<int>(doc->items.size())))) {
            edited.reset();
            editedName.clear();
            kindLabel->setText(module ? tr("Select or add an item") : tr("Select a module"));
            pages->setCurrentWidget(emptyPage);
        } else if (e->constant) {
            maths::MathConstant c;
            if (e->index >= 0) {
                c = doc->constants[static_cast<std::size_t>(e->index)];
            } else {
                c.name = us(editedName);
            }
            editedName = qs(c.name);
            kindLabel->setText(tr("Constant"));
            nameEdit->setText(qs(c.name));
            commentEdit->setText(qs(c.comment));
            constValueEdit->setText(QLocale::c().toString(c.value.v, 'g', 15));
            QStringList symbols;
            if (units() != nullptr) {
                for (const units::Unit& u : units()->units()) {
                    if (!u.isCompat() && !u.symbol.empty()) {
                        symbols << qs(u.symbol);
                    }
                }
            }
            fillUnitTextCombo(constUnitBox, symbols, unitText(c.value.unit));
            pages->setCurrentWidget(constPage);
        } else {
            maths::MathItem item;
            if (e->index >= 0) {
                item = doc->items[static_cast<std::size_t>(e->index)];
            } else {
                item.kind = newKind;
                item.name = us(editedName);
            }
            editedName = qs(item.name);
            kindLabel->setText(kindName(item.kind));
            nameEdit->setText(qs(item.name));
            commentEdit->setText(qs(item.comment));
            switch (item.kind) {
            case maths::ItemKind::Expression:
                loadExpressionFields(item);
                pages->setCurrentWidget(exprPage);
                break;
            case maths::ItemKind::Filter: {
                nameLabel->setText(tr("Channel"));
                const bool seconds = item.window.seconds.has_value();
                filterModeBox->setCurrentIndex(seconds ? 1 : 0);
                filterValueBox->setDecimals(seconds ? 3 : 0);
                filterValueBox->setValue(seconds ? *item.window.seconds
                                                 : static_cast<double>(item.window.points.value_or(5)));
                pages->setCurrentWidget(filterPage);
                break;
            }
            case maths::ItemKind::ScaleOffset: {
                nameLabel->setText(tr("Channel"));
                soScaleBox->setValue(item.scale);
                soOffsetBox->setValue(item.offset.v);
                const std::optional<units::QuantityId> qid = quantityOfName(qs(item.name));
                fillUnitTextCombo(soOffsetUnitBox, qid ? unitSymbols(*qid) : QStringList{},
                                  unitText(item.offset.unit));
                pages->setCurrentWidget(soPage);
                break;
            }
            default:
                otherText->setText(
                    tr("%1 items are kept as they are; their editor arrives in a later version.")
                        .arg(kindName(item.kind)));
                pages->setCurrentWidget(otherPage);
                break;
            }
        }
        setPageReadOnly(readOnly ||
                        (edited && !edited->constant && editedKind() != maths::ItemKind::Expression &&
                         editedKind() != maths::ItemKind::Filter &&
                         editedKind() != maths::ItemKind::ScaleOffset));
        applyButton->setEnabled(edited.has_value() && !readOnly);
        revertButton->setEnabled(edited.has_value() && !readOnly);
        --loading;
        exprDiags.clear();
        live->setSource(scopeSource());
        live->setExpression(editedKind() == maths::ItemKind::Expression ? exprEdit->toPlainText()
                                                                        : QString());
        live->flush();
        if (edited && edited->index < 0) {
            dirty = true; // a new item exists only once applied
        }
        refreshStatus();
        updateDiagnosticsUi();
    }

    // Rebuilds the panes after a document or source change; unapplied page edits are kept.
    void refreshAll() {
        rebuildModules();
        rebuildItems();
        live->setSource(scopeSource()); // a source opened or closed changes the scope (re-checked)
        const int r = findEditedRow();
        if (r >= 0) {
            selectRow(r);
            if (dirty) {
                edited = rows[static_cast<std::size_t>(r)];
                updateDiagnosticsUi();
            } else {
                loadPage(rows[static_cast<std::size_t>(r)]);
            }
        } else if (edited) {
            // The edited item is gone (an undo removed it): its page goes too.
            dirty = false;
            edited.reset();
            selectRow(rows.empty() ? -1 : 0);
            loadPage(rows.empty() ? std::nullopt : std::optional<Entry>(rows.front()));
        } else if (!rows.empty() && itemsTable->currentRow() < 0) {
            selectRow(0);
            loadPage(rows.front());
        } else if (rows.empty()) {
            loadPage(std::nullopt);
        }
    }

    void selectModule(const ModuleRef& ref, const QString& itemName) {
        module = ref;
        edited.reset();
        editedName.clear();
        dirty = false;
        rebuildModules();
        rebuildItems();
        int r = rows.empty() ? -1 : 0;
        if (!itemName.isEmpty() && module) {
            r = -1;
            const auto doc = moduleDoc(*module);
            const std::string cf = caseFold(us(itemName));
            for (int i = 0; doc && i < static_cast<int>(rows.size()); ++i) {
                const Entry& e = rows[static_cast<std::size_t>(i)];
                const std::string& n = e.constant ? doc->constants[static_cast<std::size_t>(e.index)].name
                                                  : doc->items[static_cast<std::size_t>(e.index)].name;
                if (caseFold(n) == cf) {
                    r = i;
                    break;
                }
            }
        }
        selectRow(r);
        loadPage(r >= 0 ? std::optional<Entry>(rows[static_cast<std::size_t>(r)]) : std::nullopt);
    }

    // ---- applying -----------------------------------------------------------------------------------------

    maths::MathConstant pageConstant(maths::MathConstant c) const {
        c.name = us(nameEdit->text().trimmed());
        c.comment = us(commentEdit->text());
        c.value.v = QLocale::c().toDouble(constValueEdit->text().trimmed());
        c.value.unit = unitOfText(constUnitBox->currentText());
        return c;
    }

    maths::MathItem pageItem(maths::MathItem item) const {
        item.name = us(nameEdit->text().trimmed());
        item.comment = us(commentEdit->text());
        switch (item.kind) {
        case maths::ItemKind::Expression:
            item.expr = us(exprEdit->toPlainText());
            item.quantity = static_cast<units::QuantityId>(quantityBox->currentData().toUInt());
            item.resultUnit = static_cast<units::UnitId>(resultUnitBox->currentData().toUInt());
            item.display.unit = static_cast<units::UnitId>(displayUnitBox->currentData().toUInt());
            item.display.dps = dpsBox->value();
            item.display.colourIndex = colourBox->value();
            item.interp = static_cast<store::Interp>(std::clamp(interpBox->currentIndex(), 0, 3));
            item.rate.automatic = rateAutoBox->isChecked();
            if (!item.rate.automatic) {
                item.rate.rate = rateOfHz(rateHzBox->value());
            }
            break;
        case maths::ItemKind::Filter:
            if (filterModeBox->currentIndex() == 0) {
                item.window.points = static_cast<int>(std::lround(filterValueBox->value()));
                item.window.seconds.reset();
            } else {
                item.window.seconds = filterValueBox->value();
                item.window.points.reset();
            }
            break;
        case maths::ItemKind::ScaleOffset:
            item.scale = soScaleBox->value();
            item.offset.v = soOffsetBox->value();
            item.offset.unit = unitOfText(soOffsetUnitBox->currentText());
            break;
        default:
            break;
        }
        return item;
    }

    // Applies the edited item; on refusal the bar says why (with Revert when the edit is invalid).
    bool commit() {
        if (!edited || !dirty || !module) {
            hideBar();
            return true;
        }
        if (!writable(*module)) {
            showBar(tr("This module cannot be edited here."), true);
            return false;
        }
        const maths::Diagnostics diags = allDiagnostics(true);
        if (const maths::Diagnostic* err = firstOfLevel(diags, maths::Diagnostic::Error)) {
            showBar(
                tr("'%1' has an error and cannot be applied: %2")
                    .arg(nameEdit->text().trimmed().isEmpty() ? tr("The item") : nameEdit->text().trimmed(),
                         describe(*err, exprEdit->toPlainText())),
                true);
            return false;
        }
        std::optional<maths::MathModule> doc = moduleDoc(*module);
        if (!doc) {
            return false;
        }
        Entry target = *edited;
        QString undoText;
        std::string newName;
        if (edited->constant) {
            maths::MathConstant c;
            if (edited->index >= 0 && edited->index < static_cast<int>(doc->constants.size())) {
                c = doc->constants[static_cast<std::size_t>(edited->index)];
            }
            c = pageConstant(std::move(c));
            newName = c.name;
            if (edited->index < 0) {
                doc->constants.push_back(std::move(c));
                target.index = static_cast<int>(doc->constants.size()) - 1;
                undoText = tr("Add maths constant '%1'").arg(qs(newName));
            } else {
                doc->constants[static_cast<std::size_t>(edited->index)] = std::move(c);
                undoText = tr("Edit maths constant '%1'").arg(qs(newName));
            }
        } else {
            maths::MathItem item;
            if (edited->index >= 0 && edited->index < static_cast<int>(doc->items.size())) {
                item = doc->items[static_cast<std::size_t>(edited->index)];
            } else {
                item.kind = newKind;
            }
            item = pageItem(std::move(item));
            newName = item.name;
            if (edited->index < 0) {
                doc->items.push_back(std::move(item));
                target.index = static_cast<int>(doc->items.size()) - 1;
                undoText = tr("Add maths item '%1'").arg(qs(newName));
            } else {
                doc->items[static_cast<std::size_t>(edited->index)] = std::move(item);
                undoText = tr("Edit maths item '%1'").arg(qs(newName));
            }
        }
        const std::optional<Entry> before = edited;
        edited = target;
        editedName = qs(newName);
        dirty = false;
        if (!writeModule(*module, std::move(*doc), undoText)) {
            edited = before;
            dirty = true;
            return false;
        }
        hideBar();
        refreshAll();
        return true;
    }

    // Leaving the page (another item, module or an operation): applies it, or refuses.
    bool leavePage() {
        if (!dirty) {
            return true;
        }
        return commit();
    }

    void revertPage() {
        if (edited && edited->index < 0) {
            edited.reset();
        }
        dirty = false;
        hideBar();
        refreshAll();
        if (!edited) {
            const int r = itemsTable->currentRow();
            if (r >= 0 && r < static_cast<int>(rows.size()) && rows[static_cast<std::size_t>(r)].index >= 0) {
                loadPage(rows[static_cast<std::size_t>(r)]);
            } else {
                selectRow(rows.empty() ? -1 : 0);
                loadPage(rows.empty() ? std::nullopt : std::optional<Entry>(rows.front()));
            }
        }
    }

    void touched() {
        if (loading > 0) {
            return;
        }
        dirty = true;
        hideBar();
        updateDiagnosticsUi();
    }

    // ---- selection of entries -----------------------------------------------------------------------------

    std::vector<Entry> selectedEntries() const {
        std::set<int> picked;
        for (const QModelIndex& i : itemsTable->selectionModel()->selectedRows()) {
            picked.insert(i.row());
        }
        if (picked.empty() && itemsTable->currentRow() >= 0) {
            picked.insert(itemsTable->currentRow());
        }
        std::vector<Entry> out;
        for (const int r : picked) {
            if (r >= 0 && r < static_cast<int>(rows.size()) && rows[static_cast<std::size_t>(r)].index >= 0) {
                out.push_back(rows[static_cast<std::size_t>(r)]);
            }
        }
        return out;
    }

    bool removeEntries(std::vector<Entry> entries, const QString& text) {
        if (!module || entries.empty() || !writable(*module)) {
            return false;
        }
        std::optional<maths::MathModule> doc = moduleDoc(*module);
        if (!doc) {
            return false;
        }
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) { return a.index > b.index; });
        for (const Entry& e : entries) {
            if (e.constant && e.index < static_cast<int>(doc->constants.size())) {
                doc->constants.erase(doc->constants.begin() + e.index);
            } else if (!e.constant && e.index < static_cast<int>(doc->items.size())) {
                doc->items.erase(doc->items.begin() + e.index);
            }
            if (edited && *edited == e) {
                edited.reset();
                editedName.clear();
                dirty = false;
            }
        }
        return writeModule(*module, std::move(*doc), text);
    }

    // ---- unit popup ---------------------------------------------------------------------------------------

    void hideUnitPopup() {
        if (unitPopup != nullptr) {
            unitPopup->hide();
        }
    }

    void updateUnitPopup() {
        if (exprEdit->isReadOnly() || editedKind() != maths::ItemKind::Expression) {
            hideUnitPopup();
            return;
        }
        const int caret = exprEdit->textCursor().position();
        const std::optional<assist::UnitBracket> b = assist::unitBracketAt(exprEdit->toPlainText(), caret);
        const QStringList all = b ? q.unitsAt(caret) : QStringList{};
        QStringList shown;
        for (const QString& s : all) {
            if (b->typed.trimmed().isEmpty() || s.startsWith(b->typed.trimmed(), Qt::CaseInsensitive)) {
                shown << s;
            }
        }
        if (shown.isEmpty()) {
            hideUnitPopup();
            return;
        }
        const QString current =
            unitPopup->currentItem() != nullptr ? unitPopup->currentItem()->text() : QString();
        unitPopup->clear();
        unitPopup->addItems(shown);
        const auto match = unitPopup->findItems(current, Qt::MatchExactly);
        unitPopup->setCurrentRow(match.isEmpty() ? 0 : unitPopup->row(match.front()));
        const QRect caretRect = exprEdit->cursorRect();
        const QPoint at = exprEdit->viewport()->mapTo(&q, caretRect.bottomLeft() + QPoint(0, 2));
        const int rowsShown = std::min(static_cast<int>(shown.size()), 8);
        const int h = unitPopup->sizeHintForRow(0) * rowsShown + 2 * unitPopup->frameWidth() + 2;
        unitPopup->setGeometry(at.x(), at.y(), std::max(140, unitPopup->sizeHintForColumn(0) + 32),
                               std::max(h, 24));
        unitPopup->raise();
        unitPopup->show();
    }

    bool acceptPopup() {
        if (!unitPopup->isVisible() || unitPopup->currentItem() == nullptr) {
            return false;
        }
        const QString symbol = unitPopup->currentItem()->text();
        const int caret = exprEdit->textCursor().position();
        const std::optional<assist::UnitBracket> b = assist::unitBracketAt(exprEdit->toPlainText(), caret);
        hideUnitPopup();
        if (!b) {
            return false;
        }
        const assist::Edit edit = assist::insertUnit(exprEdit->toPlainText(), *b, symbol);
        QTextCursor c(exprEdit->document());
        c.beginEditBlock();
        c.select(QTextCursor::Document);
        c.insertText(edit.text);
        c.endEditBlock();
        c.setPosition(edit.caret);
        exprEdit->setTextCursor(c);
        hideUnitPopup();
        return true;
    }

    void insertAtCaret(const QString& text) {
        if (editedKind() != maths::ItemKind::Expression || exprEdit->isReadOnly()) {
            return;
        }
        exprEdit->textCursor().insertText(text);
        exprEdit->setFocus();
    }

    // ---- construction -------------------------------------------------------------------------------------

    void build();
    void connectAll();
};

void MathsEditor::Impl::build() {
    q.setWindowTitle(tr("Maths"));
    q.setModal(false);
    q.setSizeGripEnabled(true);
    q.resize(1100, 680);
    auto* root = new QVBoxLayout(&q);

    // The refusal bar (cannot apply / cannot close), with Revert.
    bar = new QFrame(&q);
    bar->setFrameShape(QFrame::StyledPanel);
    bar->setAutoFillBackground(true);
    {
        QPalette pal = bar->palette();
        pal.setColor(QPalette::Window, QColor(0xFD, 0xEC, 0xEA));
        pal.setColor(QPalette::WindowText, QColor(0x5F, 0x10, 0x10));
        bar->setPalette(pal);
    }
    auto* barLayout = new QHBoxLayout(bar);
    barText = new QLabel(bar);
    barText->setWordWrap(true);
    barRevert = new QPushButton(tr("Revert"), bar);
    barRevert->setAutoDefault(false);
    auto* barClose = new QPushButton(tr("Keep editing"), bar);
    barClose->setAutoDefault(false);
    barLayout->addWidget(barText, 1);
    barLayout->addWidget(barRevert);
    barLayout->addWidget(barClose);
    bar->hide();
    root->addWidget(bar);
    QObject::connect(barClose, &QPushButton::clicked, &q, [this] { hideBar(); });

    auto* split = new QSplitter(Qt::Horizontal, &q);
    root->addWidget(split, 1);

    // Modules pane.
    auto* left = new QWidget(split);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    modulesTree = new QTreeWidget(left);
    modulesTree->setHeaderLabel(tr("Modules"));
    modulesTree->setAccessibleName(tr("Maths modules"));
    leftLayout->addWidget(modulesTree, 1);
    auto* moduleButtons = new QHBoxLayout;
    const auto smallButton = [](const QString& text, QWidget* parent) {
        auto* b = new QPushButton(text, parent);
        b->setAutoDefault(false);
        return b;
    };
    auto* addModuleButton = smallButton(tr("Add..."), left);
    auto* deleteModuleButton = smallButton(tr("Delete"), left);
    auto* upButton = smallButton(tr("Up"), left);
    auto* downButton = smallButton(tr("Down"), left);
    auto* importButton = smallButton(tr("Import..."), left);
    auto* exportButton = smallButton(tr("Export..."), left);
    for (QPushButton* b : {addModuleButton, deleteModuleButton, upButton, downButton}) {
        moduleButtons->addWidget(b);
    }
    leftLayout->addLayout(moduleButtons);
    auto* moduleButtons2 = new QHBoxLayout;
    moduleButtons2->addWidget(importButton);
    moduleButtons2->addWidget(exportButton);
    leftLayout->addLayout(moduleButtons2);
    auto* conditionRow = new QFormLayout;
    conditionEdit = new QLineEdit(left);
    conditionEdit->setPlaceholderText(tr("'Vehicle Id' == \"GT3-07\""));
    conditionEdit->setProperty(kTextEntryProperty, true);
    conditionEdit->setAccessibleName(tr("Module condition"));
    conditionRow->addRow(tr("Condition"), conditionEdit);
    leftLayout->addLayout(conditionRow);

    // Items pane.
    auto* centre = new QWidget(split);
    auto* centreLayout = new QVBoxLayout(centre);
    centreLayout->setContentsMargins(0, 0, 0, 0);
    itemsTable = new QTableWidget(0, ColumnCount, centre);
    itemsTable->setHorizontalHeaderLabels(
        {QString(), tr("Kind"), tr("Name"), tr("Rate"), tr("Unit"), tr("Status"), tr("Comment")});
    itemsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    itemsTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    itemsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    itemsTable->verticalHeader()->hide();
    itemsTable->horizontalHeader()->setStretchLastSection(true);
    itemsTable->setAccessibleName(tr("Maths items"));
    centreLayout->addWidget(itemsTable, 1);
    auto* itemButtons = new QHBoxLayout;
    auto* addItemButton = new QToolButton(centre);
    addItemButton->setText(tr("Add"));
    addItemButton->setPopupMode(QToolButton::InstantPopup);
    auto* addMenu = new QMenu(addItemButton);
    addItemButton->setMenu(addMenu);
    auto* deleteButton = smallButton(tr("Delete"), centre);
    auto* cutButton = smallButton(tr("Cut"), centre);
    auto* copyButton = smallButton(tr("Copy"), centre);
    auto* pasteButton = smallButton(tr("Paste"), centre);
    auto* recalcButton = smallButton(tr("Recalculate"), centre);
    itemButtons->addWidget(addItemButton);
    for (QPushButton* b : {deleteButton, cutButton, copyButton, pasteButton, recalcButton}) {
        itemButtons->addWidget(b);
    }
    itemButtons->addStretch(1);
    centreLayout->addLayout(itemButtons);

    // Item pane.
    auto* right = new QWidget(split);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    kindLabel = new QLabel(right);
    QFont bold = kindLabel->font();
    bold.setBold(true);
    kindLabel->setFont(bold);
    rightLayout->addWidget(kindLabel);
    auto* common = new QFormLayout;
    nameLabel = new QLabel(tr("Name"), right);
    nameEdit = new QLineEdit(right);
    // A name is any text without the single quote that delimits references (03 section 2.1).
    nameEdit->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[^']*")), nameEdit));
    nameEdit->setProperty(kTextEntryProperty, true);
    nameEdit->setAccessibleName(tr("Item name"));
    common->addRow(nameLabel, nameEdit);
    commentEdit = new QLineEdit(right);
    commentEdit->setProperty(kTextEntryProperty, true);
    commentEdit->setAccessibleName(tr("Item comment"));
    common->addRow(tr("Comment"), commentEdit);
    rightLayout->addLayout(common);
    pages = new QStackedWidget(right);
    rightLayout->addWidget(pages, 1);
    statusLabel = new QLabel(right);
    statusLabel->setAccessibleName(tr("Item status"));
    rightLayout->addWidget(statusLabel);

    emptyPage = new QWidget(pages);
    pages->addWidget(emptyPage);

    // Expression page.
    exprPage = new QWidget(pages);
    auto* exprLayout = new QVBoxLayout(exprPage);
    exprLayout->setContentsMargins(0, 0, 0, 0);
    auto* exprForm = new QFormLayout;
    quantityBox = new QComboBox(exprPage);
    if (const units::UnitRegistry* reg = units()) {
        for (const units::Quantity& qty : reg->quantities()) {
            quantityBox->addItem(qs(qty.name), QVariant::fromValue<uint>(qty.id));
        }
    }
    exprForm->addRow(tr("Quantity"), quantityBox);
    resultUnitBox = new QComboBox(exprPage);
    exprForm->addRow(tr("Result unit"), resultUnitBox);
    displayUnitBox = new QComboBox(exprPage);
    exprForm->addRow(tr("Display unit"), displayUnitBox);
    auto* rateRow = new QHBoxLayout;
    rateAutoBox = new QCheckBox(tr("Auto"), exprPage);
    rateHzBox = new QDoubleSpinBox(exprPage);
    rateHzBox->setRange(0.001, 1.0e6);
    rateHzBox->setDecimals(3);
    rateHzBox->setSuffix(QStringLiteral(" Hz"));
    rateRow->addWidget(rateAutoBox);
    rateRow->addWidget(rateHzBox, 1);
    exprForm->addRow(tr("Rate"), rateRow);
    dpsBox = new QSpinBox(exprPage);
    dpsBox->setRange(0, 9);
    exprForm->addRow(tr("Decimal places"), dpsBox);
    colourBox = new QSpinBox(exprPage);
    colourBox->setRange(-1, 255);
    exprForm->addRow(tr("Colour index"), colourBox);
    interpBox = new QComboBox(exprPage);
    interpBox->addItems({tr("Linear"), tr("Closest"), tr("Previous"), tr("Next")});
    exprForm->addRow(tr("Interpolate"), interpBox);
    exprLayout->addLayout(exprForm);
    auto* exprTools = new QHBoxLayout;
    exprTools->addWidget(new QLabel(tr("Expression"), exprPage));
    exprTools->addStretch(1);
    insertChannelButton = new QToolButton(exprPage);
    insertChannelButton->setText(tr("Insert Channel"));
    insertChannelButton->setPopupMode(QToolButton::InstantPopup);
    insertChannelButton->setMenu(new QMenu(insertChannelButton));
    insertRangeButton = new QToolButton(exprPage);
    insertRangeButton->setText(tr("Insert Range"));
    insertRangeButton->setPopupMode(QToolButton::InstantPopup);
    insertRangeButton->setMenu(new QMenu(insertRangeButton));
    exprTools->addWidget(insertChannelButton);
    exprTools->addWidget(insertRangeButton);
    exprLayout->addLayout(exprTools);
    exprEdit = new QPlainTextEdit(exprPage);
    exprEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    exprEdit->setTabChangesFocus(true);
    exprEdit->setProperty(kTextEntryProperty, true);
    exprEdit->setAccessibleName(tr("Expression"));
    exprEdit->viewport()->setMouseTracking(true);
    exprLayout->addWidget(exprEdit, 2);
    diagLine = new QLabel(exprPage);
    diagLine->setWordWrap(true);
    diagLine->setAccessibleName(tr("Expression diagnostics"));
    exprLayout->addWidget(diagLine);
    diagList = new QListWidget(exprPage);
    diagList->setMaximumHeight(90);
    diagList->setAccessibleName(tr("All diagnostics"));
    exprLayout->addWidget(diagList, 1);
    pages->addWidget(exprPage);

    // Constant page.
    constPage = new QWidget(pages);
    auto* constForm = new QFormLayout(constPage);
    constValueEdit = new QLineEdit(constPage);
    constValueEdit->setProperty(kTextEntryProperty, true);
    constForm->addRow(tr("Value"), constValueEdit);
    constUnitBox = new QComboBox(constPage);
    constUnitBox->setEditable(true);
    constUnitBox->setInsertPolicy(QComboBox::NoInsert);
    constForm->addRow(tr("Unit"), constUnitBox);
    pages->addWidget(constPage);

    // Filter page (the simple Filter: a moving average replacing the channel, 03 section 4.4).
    filterPage = new QWidget(pages);
    auto* filterForm = new QFormLayout(filterPage);
    filterModeBox = new QComboBox(filterPage);
    filterModeBox->addItems({tr("Points"), tr("Seconds")});
    filterForm->addRow(tr("Window in"), filterModeBox);
    filterValueBox = new QDoubleSpinBox(filterPage);
    filterValueBox->setRange(0.0, 1.0e6);
    filterForm->addRow(tr("Window"), filterValueBox);
    pages->addWidget(filterPage);

    // Scale & Offset page.
    soPage = new QWidget(pages);
    auto* soForm = new QFormLayout(soPage);
    soScaleBox = new QDoubleSpinBox(soPage);
    soScaleBox->setRange(-1.0e9, 1.0e9);
    soScaleBox->setDecimals(6);
    soForm->addRow(tr("Scale"), soScaleBox);
    soOffsetBox = new QDoubleSpinBox(soPage);
    soOffsetBox->setRange(-1.0e9, 1.0e9);
    soOffsetBox->setDecimals(6);
    soForm->addRow(tr("Offset"), soOffsetBox);
    soOffsetUnitBox = new QComboBox(soPage);
    soOffsetUnitBox->setEditable(true);
    soOffsetUnitBox->setInsertPolicy(QComboBox::NoInsert);
    soForm->addRow(tr("Offset unit"), soOffsetUnitBox);
    pages->addWidget(soPage);

    otherPage = new QWidget(pages);
    auto* otherLayout = new QVBoxLayout(otherPage);
    otherText = new QLabel(otherPage);
    otherText->setWordWrap(true);
    otherLayout->addWidget(otherText);
    otherLayout->addStretch(1);
    pages->addWidget(otherPage);

    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 4);
    split->setStretchFactor(2, 5);

    // Dialog buttons.
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    applyButton = new QPushButton(tr("Apply"), &q);
    applyButton->setAutoDefault(false);
    revertButton = new QPushButton(tr("Revert"), &q);
    revertButton->setAutoDefault(false);
    auto* closeButton = new QPushButton(tr("Close"), &q);
    closeButton->setAutoDefault(false);
    buttons->addWidget(applyButton);
    buttons->addWidget(revertButton);
    buttons->addWidget(closeButton);
    root->addLayout(buttons);

    // The unit popup: a list over the dialog that never takes the focus (the caret stays in the field).
    unitPopup = new QListWidget(&q);
    unitPopup->setFocusPolicy(Qt::NoFocus);
    unitPopup->setAccessibleName(tr("Units"));
    unitPopup->hide();

    statusTimer = new QTimer(&q);
    statusTimer->setSingleShot(true);
    statusTimer->setInterval(kStatusRefreshMs);

    live = std::make_unique<qt::LiveDiagnostics>(maths());

    // Wiring of the widgets.
    QObject::connect(barRevert, &QPushButton::clicked, &q, [this] { revertPage(); });
    QObject::connect(applyButton, &QPushButton::clicked, &q, [this] { static_cast<void>(commit()); });
    QObject::connect(revertButton, &QPushButton::clicked, &q, [this] { revertPage(); });
    QObject::connect(closeButton, &QPushButton::clicked, &q, [this] { q.close(); });
    QObject::connect(addModuleButton, &QPushButton::clicked, &q, [this] {
        bool ok = false;
        const QString id = QInputDialog::getText(&q, tr("Add maths module"), tr("Module name"),
                                                 QLineEdit::Normal, QString(), &ok);
        if (ok && !q.addModule(id.trimmed())) {
            showBar(tr("'%1' is not a valid new module name.").arg(id.trimmed()), false);
        }
    });
    QObject::connect(deleteModuleButton, &QPushButton::clicked, &q, [this] {
        if (module && module->scope == ModuleRef::Scope::Global) {
            static_cast<void>(q.removeModule(module->moduleId));
        }
    });
    QObject::connect(upButton, &QPushButton::clicked, &q, [this] {
        if (module && module->scope == ModuleRef::Scope::Global) {
            static_cast<void>(q.moveModule(module->moduleId, -1));
        }
    });
    QObject::connect(downButton, &QPushButton::clicked, &q, [this] {
        if (module && module->scope == ModuleRef::Scope::Global) {
            static_cast<void>(q.moveModule(module->moduleId, 1));
        }
    });
    QObject::connect(importButton, &QPushButton::clicked, &q, [this] {
        const QString file = QFileDialog::getOpenFileName(&q, tr("Import maths module"), QString(),
                                                          tr("Maths modules (*.maths.json);;All files (*)"));
        QString error;
        if (!file.isEmpty() && !q.importModule(pathFromUtf8(us(file)), &error)) {
            showBar(error, false);
        }
    });
    QObject::connect(exportButton, &QPushButton::clicked, &q, [this] {
        if (!module || module->scope != ModuleRef::Scope::Global) {
            return;
        }
        const QString file = QFileDialog::getSaveFileName(&q, tr("Export maths module"),
                                                          module->moduleId + QStringLiteral(".maths.json"),
                                                          tr("Maths modules (*.maths.json)"));
        QString error;
        if (!file.isEmpty() && !q.exportModule(module->moduleId, pathFromUtf8(us(file)), &error)) {
            showBar(error, false);
        }
    });
    QObject::connect(conditionEdit, &QLineEdit::editingFinished, &q, [this] {
        if (module && module->scope == ModuleRef::Scope::Global) {
            static_cast<void>(q.setModuleCondition(module->moduleId, conditionEdit->text()));
        }
    });
    addMenu->addAction(tr("Expression"), &q,
                       [this] { static_cast<void>(q.addItem(maths::ItemKind::Expression)); });
    addMenu->addAction(tr("Constant"), &q, [this] { static_cast<void>(q.addConstant()); });
    addMenu->addAction(tr("Filter"), &q, [this] { static_cast<void>(q.addItem(maths::ItemKind::Filter)); });
    addMenu->addAction(tr("Scale && Offset"), &q,
                       [this] { static_cast<void>(q.addItem(maths::ItemKind::ScaleOffset)); });
    QObject::connect(deleteButton, &QPushButton::clicked, &q,
                     [this] { static_cast<void>(q.removeSelected()); });
    QObject::connect(cutButton, &QPushButton::clicked, &q, [this] { static_cast<void>(q.cutSelected()); });
    QObject::connect(copyButton, &QPushButton::clicked, &q, [this] { static_cast<void>(q.copySelected()); });
    QObject::connect(pasteButton, &QPushButton::clicked, &q, [this] { static_cast<void>(q.paste()); });
    QObject::connect(recalcButton, &QPushButton::clicked, &q, [this] { q.recalculate(); });
    QObject::connect(insertRangeButton->menu(), &QMenu::aboutToShow, &q, [this] {
        QMenu* menu = insertRangeButton->menu();
        menu->clear();
        for (const QString& path : q.rangePaths()) {
            menu->addAction(path, &q, [this, path] { q.insertRange(path); });
        }
    });
    QObject::connect(insertChannelButton->menu(), &QMenu::aboutToShow, &q, [this] {
        QMenu* menu = insertChannelButton->menu();
        menu->clear();
        for (const QString& name : q.channelNames()) {
            menu->addAction(name, &q, [this, name] { q.insertChannel(name); });
        }
    });
    QObject::connect(unitPopup, &QListWidget::itemClicked, &q, [this](QListWidgetItem* item) {
        unitPopup->setCurrentItem(item);
        static_cast<void>(acceptPopup());
        exprEdit->setFocus();
    });
}

void MathsEditor::Impl::connectAll() {
    // Page fields.
    for (QLineEdit* e : {nameEdit, commentEdit, constValueEdit}) {
        QObject::connect(e, &QLineEdit::textChanged, &q, [this] { touched(); });
    }
    QObject::connect(nameEdit, &QLineEdit::textChanged, &q, [this](const QString& text) {
        if (loading == 0 && editedKind() == maths::ItemKind::ScaleOffset) {
            const std::optional<units::QuantityId> qid = quantityOfName(text.trimmed());
            const QString current = soOffsetUnitBox->currentText();
            fillUnitTextCombo(soOffsetUnitBox, qid ? unitSymbols(*qid) : QStringList{}, current);
        }
    });
    for (QComboBox* c : {resultUnitBox, displayUnitBox, interpBox, filterModeBox}) {
        QObject::connect(c, &QComboBox::currentIndexChanged, &q, [this] { touched(); });
    }
    for (QComboBox* c : {constUnitBox, soOffsetUnitBox}) {
        QObject::connect(c, &QComboBox::currentTextChanged, &q, [this] { touched(); });
    }
    QObject::connect(quantityBox, &QComboBox::currentIndexChanged, &q, [this] {
        if (loading > 0) {
            return;
        }
        const auto qid = static_cast<units::QuantityId>(quantityBox->currentData().toUInt());
        const units::UnitRegistry* reg = units();
        units::UnitId result = units::kNoUnit;
        if (reg != nullptr && qid != units::kUnitless && reg->containsQuantity(qid)) {
            result = reg->quantity(qid).base;
        }
        fillUnitCombo(resultUnitBox, qid, tr("(none)"), result);
        fillUnitCombo(displayUnitBox, qid, tr("(result unit)"), units::kNoUnit);
        touched();
    });
    QObject::connect(filterModeBox, &QComboBox::currentIndexChanged, &q, [this](int index) {
        if (loading == 0) {
            filterValueBox->setDecimals(index == 1 ? 3 : 0);
        }
    });
    QObject::connect(rateAutoBox, &QCheckBox::toggled, &q, [this](bool on) {
        rateHzBox->setEnabled(!on && !exprEdit->isReadOnly());
        touched();
    });
    for (QDoubleSpinBox* s : {rateHzBox, filterValueBox, soScaleBox, soOffsetBox}) {
        QObject::connect(s, &QDoubleSpinBox::valueChanged, &q, [this] { touched(); });
    }
    for (QSpinBox* s : {dpsBox, colourBox}) {
        QObject::connect(s, &QSpinBox::valueChanged, &q, [this] { touched(); });
    }
    QObject::connect(exprEdit, &QPlainTextEdit::textChanged, &q, [this] {
        if (loading > 0) {
            return;
        }
        dirty = true;
        hideBar();
        live->setExpression(exprEdit->toPlainText());
        updateUnitPopup();
    });
    QObject::connect(exprEdit, &QPlainTextEdit::cursorPositionChanged, &q, [this] {
        if (loading == 0) {
            updateUnitPopup();
        }
    });
    QObject::connect(live.get(), &qt::LiveDiagnostics::diagnosticsReady, &q,
                     [this](const maths::Diagnostics& diags) {
                         exprDiags = diags;
                         updateDiagnosticsUi();
                     });
    exprEdit->installEventFilter(&q);
    exprEdit->viewport()->installEventFilter(&q);

    // Panes.
    QObject::connect(modulesTree, &QTreeWidget::currentItemChanged, &q,
                     [this](QTreeWidgetItem* current, QTreeWidgetItem* previous) {
                         if (loading > 0) {
                             return;
                         }
                         const std::optional<ModuleRef> ref = refOf(current);
                         const auto restore = [this, previous] {
                             ++loading;
                             const QSignalBlocker block(modulesTree);
                             modulesTree->setCurrentItem(previous);
                             --loading;
                         };
                         if (!ref) {
                             restore();
                             return;
                         }
                         if (module && *ref == *module) {
                             return;
                         }
                         if (!leavePage()) {
                             restore();
                             return;
                         }
                         selectModule(*ref, {});
                     });
    QObject::connect(modulesTree, &QTreeWidget::itemChanged, &q, [this](QTreeWidgetItem* item, int) {
        if (loading > 0) {
            return;
        }
        const std::optional<ModuleRef> ref = refOf(item);
        if (ref && ref->scope == ModuleRef::Scope::Global) {
            static_cast<void>(q.setModuleEnabled(ref->moduleId, item->checkState(0) == Qt::Checked));
        }
    });
    QObject::connect(
        itemsTable, &QTableWidget::currentCellChanged, &q, [this](int current, int, int previous, int) {
            if (loading > 0 || current == previous) {
                return;
            }
            if (current < 0 || current >= static_cast<int>(rows.size())) {
                return;
            }
            const Entry target = rows[static_cast<std::size_t>(current)];
            if (edited && target == *edited) {
                return;
            }
            if (!leavePage()) {
                selectRow(findEditedRow());
                return;
            }
            const int r = rowOf(target);
            selectRow(r);
            loadPage(r >= 0 ? std::optional<Entry>(rows[static_cast<std::size_t>(r)]) : std::nullopt);
        });
    QObject::connect(itemsTable, &QTableWidget::itemChanged, &q, [this](QTableWidgetItem* cell) {
        if (loading > 0 || cell->column() != EnabledCol || !module) {
            return;
        }
        const int r = cell->row();
        if (r < 0 || r >= static_cast<int>(rows.size()) || rows[static_cast<std::size_t>(r)].index < 0) {
            return;
        }
        const Entry e = rows[static_cast<std::size_t>(r)];
        std::optional<maths::MathModule> doc = moduleDoc(*module);
        if (!doc || !leavePage()) {
            refreshAll();
            return;
        }
        const bool on = cell->checkState() == Qt::Checked;
        if (e.constant) {
            doc->constants[static_cast<std::size_t>(e.index)].enabled = on;
        } else {
            doc->items[static_cast<std::size_t>(e.index)].enabled = on;
        }
        if (!writeModule(*module, std::move(*doc), on ? tr("Enable maths item") : tr("Disable maths item"))) {
            refreshAll();
        }
    });

    // Documents, sources and status.
    QObject::connect(&session(), &qt::ProjectSession::documentsChanged, &q, [this] { refreshAll(); });
    if (loader() != nullptr) {
        QObject::connect(loader(), &qt::SourceLoader::sourcePublished, &q, [this] { refreshAll(); });
        QObject::connect(loader(), &qt::SourceLoader::sourceReplaced, &q, [this] { refreshAll(); });
        QObject::connect(loader(), &qt::SourceLoader::sourceClosed, &q, [this] { refreshAll(); });
    }
    QObject::connect(&maths(), &qt::MathsService::statusChanged, statusTimer, [this] {
        if (!statusTimer->isActive()) {
            statusTimer->start();
        }
    });
    QObject::connect(statusTimer, &QTimer::timeout, &q, [this] {
        // A transient module appears or goes with a status change (filters from a channel label).
        if (transientSources() != listedTransient) {
            refreshAll();
        } else {
            refreshStatus();
        }
    });
}

// ---- MathsEditor ------------------------------------------------------------------------------------------

MathsEditor::MathsEditor(qt::ProjectSession& session, qt::MathsService& maths, qt::SourceLoader* loader,
                         QWidget* parent)
    : QDialog(parent), d_(std::make_unique<Impl>(*this)), session_(session), maths_(maths), loader_(loader) {
    d_->build();
    d_->connectAll();
    d_->rebuildModules();
    if (!session_.mathsModules().empty()) {
        ModuleRef first;
        first.moduleId = qs(session_.mathsModules().front().id);
        d_->selectModule(first, {});
    } else {
        d_->rebuildItems();
        d_->loadPage(std::nullopt);
    }
}

MathsEditor::~MathsEditor() {
    // ~QWidget deletes the children after the Impl is gone: their signals must not reach it any more.
    for (QObject* child : findChildren<QObject*>()) {
        child->disconnect(this);
    }
}

void MathsEditor::setMainSource(std::optional<model::SourceId> source) {
    d_->main = std::move(source);
    d_->live->setSource(d_->scopeSource());
    d_->refreshStatus();
    if (d_->editedKind() == maths::ItemKind::Expression) {
        d_->live->flush();
    }
}

std::optional<model::SourceId> MathsEditor::mainSource() const {
    return d_->effectiveMain();
}

bool MathsEditor::openAt(const QString& moduleId, const QString& itemName) {
    if (moduleId == qs(maths::MathModule::kLocalId) || moduleId == qs(maths::MathModule::kTransientId)) {
        const ModuleRef::Scope scope = moduleId == qs(maths::MathModule::kLocalId)
                                           ? ModuleRef::Scope::Local
                                           : ModuleRef::Scope::Transient;
        std::vector<model::SourceId> candidates;
        if (const auto mainId = d_->effectiveMain()) {
            candidates.push_back(*mainId);
        }
        for (const auto& src : d_->sources()) {
            candidates.push_back(src->id);
        }
        const std::string cf = caseFold(us(itemName));
        for (const model::SourceId& id : candidates) {
            ModuleRef ref;
            ref.scope = scope;
            ref.source = id;
            const auto doc = d_->moduleDoc(ref);
            if (doc &&
                (std::any_of(doc->items.begin(), doc->items.end(),
                             [&cf](const maths::MathItem& i) { return caseFold(i.name) == cf; }) ||
                 std::any_of(doc->constants.begin(), doc->constants.end(),
                             [&cf](const maths::MathConstant& c) { return caseFold(c.name) == cf; }))) {
                return openAt(ref, itemName);
            }
        }
        return false;
    }
    ModuleRef ref;
    ref.moduleId = moduleId;
    return openAt(ref, itemName);
}

bool MathsEditor::openAt(const ModuleRef& module, const QString& itemName) {
    const std::optional<maths::MathModule> doc = d_->moduleDoc(module);
    if (!doc) {
        return false;
    }
    const std::string cf = caseFold(us(itemName));
    const bool found = itemName.isEmpty() ||
                       std::any_of(doc->items.begin(), doc->items.end(),
                                   [&cf](const maths::MathItem& i) { return caseFold(i.name) == cf; }) ||
                       std::any_of(doc->constants.begin(), doc->constants.end(),
                                   [&cf](const maths::MathConstant& c) { return caseFold(c.name) == cf; });
    if (!found) {
        return false;
    }
    if (d_->module && *d_->module == module && caseFold(us(d_->editedName)) == cf && !d_->dirty) {
        return true;
    }
    if (!d_->leavePage()) {
        return false;
    }
    d_->selectModule(module, itemName);
    return true;
}

std::optional<MathsEditor::ModuleRef> MathsEditor::currentModule() const {
    return d_->module;
}

QString MathsEditor::currentItemName() const {
    return d_->edited ? d_->editedName : QString();
}

maths::Diagnostics MathsEditor::currentDiagnostics() const {
    return d_->allDiagnostics(false);
}

void MathsEditor::flushDiagnostics() {
    d_->live->flush();
    d_->updateDiagnosticsUi();
}

bool MathsEditor::canClose(QString* reason) const {
    if (!d_->dirty || !d_->edited) {
        return true;
    }
    const maths::Diagnostics diags = d_->allDiagnostics(true);
    if (const maths::Diagnostic* err = firstOfLevel(diags, maths::Diagnostic::Error)) {
        if (reason != nullptr) {
            *reason = tr("'%1' has an error: %2")
                          .arg(d_->nameEdit->text().trimmed().isEmpty() ? tr("The item")
                                                                        : d_->nameEdit->text().trimmed(),
                               Impl::describe(*err, d_->exprEdit->toPlainText()));
        }
        return false;
    }
    return true;
}

bool MathsEditor::apply() {
    return d_->commit();
}

void MathsEditor::revert() {
    d_->revertPage();
}

bool MathsEditor::isPageDirty() const noexcept {
    return d_->dirty;
}

QString MathsEditor::refusalText() const {
    return d_->bar->isHidden() ? QString() : d_->barText->text();
}

// ---- module and item operations ---------------------------------------------------------------------------

bool MathsEditor::addModule(const QString& id) {
    if (!validModuleId(id)) {
        return false;
    }
    const std::string sid = us(id);
    std::vector<maths::MathModule> all = session_.mathsModules();
    if (std::any_of(all.begin(), all.end(),
                    [&sid](const maths::MathModule& m) { return caseFold(m.id) == caseFold(sid); })) {
        return false;
    }
    if (!d_->leavePage()) {
        return false;
    }
    maths::MathModule m;
    m.id = sid;
    all.push_back(std::move(m));
    d_->writeGlobals(std::move(all), tr("Add maths module '%1'").arg(id));
    ModuleRef ref;
    ref.moduleId = id;
    d_->selectModule(ref, {});
    return true;
}

bool MathsEditor::removeModule(const QString& id) {
    std::vector<maths::MathModule> all = session_.mathsModules();
    const auto it =
        std::find_if(all.begin(), all.end(), [&id](const maths::MathModule& m) { return qs(m.id) == id; });
    if (it == all.end()) {
        return false;
    }
    const bool current =
        d_->module && d_->module->scope == ModuleRef::Scope::Global && d_->module->moduleId == id;
    if (current) {
        d_->dirty = false; // the edited item goes with its module
        d_->edited.reset();
    } else if (!d_->leavePage()) {
        return false;
    }
    all.erase(it);
    d_->writeGlobals(std::move(all), tr("Delete maths module '%1'").arg(id));
    if (current) {
        if (!session_.mathsModules().empty()) {
            ModuleRef first;
            first.moduleId = qs(session_.mathsModules().front().id);
            d_->selectModule(first, {});
        } else {
            d_->module.reset();
            d_->refreshAll();
        }
    }
    return true;
}

bool MathsEditor::moveModule(const QString& id, int delta) {
    std::vector<maths::MathModule> all = session_.mathsModules();
    const auto it =
        std::find_if(all.begin(), all.end(), [&id](const maths::MathModule& m) { return qs(m.id) == id; });
    if (it == all.end()) {
        return false;
    }
    const auto from = static_cast<int>(it - all.begin());
    const int to = from + delta;
    if (delta == 0 || to < 0 || to >= static_cast<int>(all.size()) || !d_->leavePage()) {
        return false;
    }
    maths::MathModule moved = std::move(all[static_cast<std::size_t>(from)]);
    all.erase(all.begin() + from);
    all.insert(all.begin() + to, std::move(moved));
    return d_->writeGlobals(std::move(all), tr("Move maths module '%1'").arg(id));
}

bool MathsEditor::setModuleEnabled(const QString& id, bool enabled) {
    std::vector<maths::MathModule> all = session_.mathsModules();
    const auto it =
        std::find_if(all.begin(), all.end(), [&id](const maths::MathModule& m) { return qs(m.id) == id; });
    if (it == all.end()) {
        return false;
    }
    it->enabled = enabled;
    return d_->writeGlobals(std::move(all), enabled ? tr("Enable maths module '%1'").arg(id)
                                                    : tr("Disable maths module '%1'").arg(id));
}

bool MathsEditor::setModuleCondition(const QString& id, const QString& condition) {
    std::vector<maths::MathModule> all = session_.mathsModules();
    const auto it =
        std::find_if(all.begin(), all.end(), [&id](const maths::MathModule& m) { return qs(m.id) == id; });
    if (it == all.end()) {
        return false;
    }
    const QString c = condition.trimmed();
    it->condition = c.isEmpty() ? std::nullopt : std::optional<std::string>(us(c));
    return d_->writeGlobals(std::move(all), tr("Condition of maths module '%1'").arg(id));
}

bool MathsEditor::importModule(const std::filesystem::path& file, QString* error) {
    const units::UnitRegistry* reg = d_->units();
    if (reg == nullptr) {
        return false;
    }
    std::vector<std::string> log;
    Result<maths::MathModule> loaded = maths::MathModuleIo::load(file, *reg, log);
    if (!loaded) {
        const QString text =
            tr("Cannot import %1: %2").arg(qs(pathToUtf8(file)), qs(loaded.error().toString()));
        if (error != nullptr) {
            *error = text;
        }
        Q_EMIT message(text);
        return false;
    }
    maths::MathModule m = std::move(loaded).value();
    QString base = qs(m.id);
    if (!validModuleId(base)) {
        std::string stem = pathToUtf8(file.filename());
        for (const std::string_view suffix : {".maths.json", ".json"}) {
            if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
                stem.resize(stem.size() - suffix.size());
                break;
            }
        }
        base = qs(stem);
    }
    if (!validModuleId(base)) {
        base = QStringLiteral("imported");
    }
    std::vector<maths::MathModule> all = session_.mathsModules();
    QString id = base;
    for (int i = 2;
         std::any_of(all.begin(), all.end(),
                     [&id](const maths::MathModule& x) { return caseFold(x.id) == caseFold(us(id)); });
         ++i) {
        id = QStringLiteral("%1-%2").arg(base).arg(i);
    }
    if (!d_->leavePage()) {
        return false;
    }
    m.id = us(id);
    m.origin = maths::MathModule::Origin::Global;
    all.push_back(std::move(m));
    d_->writeGlobals(std::move(all), tr("Import maths module '%1'").arg(id));
    ModuleRef ref;
    ref.moduleId = id;
    d_->selectModule(ref, {});
    return true;
}

bool MathsEditor::exportModule(const QString& id, const std::filesystem::path& file, QString* error) const {
    const units::UnitRegistry* reg = d_->units();
    for (const maths::MathModule& m : session_.mathsModules()) {
        if (qs(m.id) != id || reg == nullptr) {
            continue;
        }
        const Result<void> saved = maths::MathModuleIo::save(m, file, *reg);
        if (!saved && error != nullptr) {
            *error = tr("Cannot export %1: %2").arg(id, qs(saved.error().toString()));
        }
        return saved.ok();
    }
    if (error != nullptr) {
        *error = tr("No module '%1'").arg(id);
    }
    return false;
}

bool MathsEditor::addItem(maths::ItemKind kind) {
    if (!d_->module || !d_->writable(*d_->module) ||
        (kind != maths::ItemKind::Expression && kind != maths::ItemKind::Filter &&
         kind != maths::ItemKind::ScaleOffset) ||
        !d_->leavePage()) {
        return false;
    }
    const auto doc = d_->moduleDoc(*d_->module);
    if (!doc) {
        return false;
    }
    std::vector<std::string> names;
    for (const maths::MathItem& i : doc->items) {
        names.push_back(i.name);
    }
    QString name;
    if (kind == maths::ItemKind::Expression) {
        name = uniqueName(tr("New Expression"), names);
    } else {
        const QStringList channels = channelNames();
        name = channels.isEmpty() ? QString() : channels.front();
    }
    d_->newKind = kind;
    d_->edited = Impl::Entry{false, -1};
    d_->editedName = name;
    d_->rebuildItems();
    const int r = d_->rowOf(*d_->edited);
    d_->selectRow(r);
    d_->loadPage(d_->edited);
    d_->nameEdit->setFocus();
    return true;
}

bool MathsEditor::addConstant() {
    if (!d_->module || !d_->writable(*d_->module) || !d_->leavePage()) {
        return false;
    }
    const auto doc = d_->moduleDoc(*d_->module);
    if (!doc) {
        return false;
    }
    std::vector<std::string> names;
    for (const maths::MathConstant& c : doc->constants) {
        names.push_back(c.name);
    }
    d_->edited = Impl::Entry{true, -1};
    d_->editedName = uniqueName(tr("New Constant"), names);
    d_->rebuildItems();
    d_->selectRow(d_->rowOf(*d_->edited));
    d_->loadPage(d_->edited);
    d_->nameEdit->setFocus();
    return true;
}

bool MathsEditor::removeSelected() {
    std::vector<Impl::Entry> entries = d_->selectedEntries();
    const bool editedIncluded =
        d_->edited && std::find(entries.begin(), entries.end(), *d_->edited) != entries.end();
    if (d_->edited && d_->edited->index < 0) {
        // Deleting a new item that was never applied drops the page.
        d_->edited.reset();
        d_->dirty = false;
        d_->refreshAll();
        if (entries.empty()) {
            return true;
        }
    }
    if (!editedIncluded && !d_->leavePage()) {
        return false;
    }
    const auto count = entries.size();
    if (!d_->removeEntries(std::move(entries),
                           count == 1 ? tr("Delete maths item") : tr("Delete maths items"))) {
        return false;
    }
    d_->refreshAll();
    return true;
}

bool MathsEditor::copySelected() const {
    const units::UnitRegistry* reg = d_->units();
    const std::vector<Impl::Entry> entries = d_->selectedEntries();
    if (!d_->module || entries.empty() || reg == nullptr) {
        return false;
    }
    const auto doc = d_->moduleDoc(*d_->module);
    if (!doc || d_->module->scope == ModuleRef::Scope::Transient) {
        return false;
    }
    maths::MathModule clip;
    clip.id = "clipboard";
    for (const Impl::Entry& e : entries) {
        if (e.constant) {
            clip.constants.push_back(doc->constants[static_cast<std::size_t>(e.index)]);
        } else {
            clip.items.push_back(doc->items[static_cast<std::size_t>(e.index)]);
        }
    }
    const Result<std::string> json = maths::MathModuleIo::toJson(clip, *reg);
    if (!json) {
        return false;
    }
    auto* mime = new QMimeData;
    mime->setData(QString::fromLatin1(kItemsMimeType), QByteArray::fromStdString(json.value()));
    QStringList names;
    for (const maths::MathItem& i : clip.items) {
        names << qs(i.name);
    }
    for (const maths::MathConstant& c : clip.constants) {
        names << qs(c.name);
    }
    mime->setText(names.join(QLatin1Char('\n')));
    QGuiApplication::clipboard()->setMimeData(mime);
    return true;
}

bool MathsEditor::cutSelected() {
    if (!copySelected()) {
        return false;
    }
    std::vector<Impl::Entry> entries = d_->selectedEntries();
    const bool editedIncluded =
        d_->edited && std::find(entries.begin(), entries.end(), *d_->edited) != entries.end();
    if (!editedIncluded && !d_->leavePage()) {
        return false;
    }
    if (!d_->removeEntries(std::move(entries), tr("Cut maths items"))) {
        return false;
    }
    d_->refreshAll();
    return true;
}

bool MathsEditor::paste() {
    const units::UnitRegistry* reg = d_->units();
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!d_->module || !d_->writable(*d_->module) || reg == nullptr || mime == nullptr ||
        !mime->hasFormat(QString::fromLatin1(kItemsMimeType)) || !d_->leavePage()) {
        return false;
    }
    std::vector<std::string> log;
    Result<maths::MathModule> clip =
        maths::MathModuleIo::parse(mime->data(QString::fromLatin1(kItemsMimeType)).toStdString(), *reg, log);
    std::optional<maths::MathModule> doc = d_->moduleDoc(*d_->module);
    if (!clip || !doc || (clip->items.empty() && clip->constants.empty())) {
        return false;
    }
    // Names are kept (cut and paste preserves them); a duplicate in the module is W-DUP-IGNORED.
    doc->items.insert(doc->items.end(), clip->items.begin(), clip->items.end());
    doc->constants.insert(doc->constants.end(), clip->constants.begin(), clip->constants.end());
    if (!d_->writeModule(*d_->module, std::move(*doc), tr("Paste maths items"))) {
        return false;
    }
    d_->refreshAll();
    return true;
}

bool MathsEditor::setSelectedEnabled(bool enabled) {
    const std::vector<Impl::Entry> entries = d_->selectedEntries();
    if (!d_->module || entries.empty() || !d_->leavePage()) {
        return false;
    }
    std::optional<maths::MathModule> doc = d_->moduleDoc(*d_->module);
    if (!doc) {
        return false;
    }
    for (const Impl::Entry& e : entries) {
        if (e.constant) {
            doc->constants[static_cast<std::size_t>(e.index)].enabled = enabled;
        } else {
            doc->items[static_cast<std::size_t>(e.index)].enabled = enabled;
        }
    }
    return d_->writeModule(*d_->module, std::move(*doc),
                           enabled ? tr("Enable maths items") : tr("Disable maths items"));
}

void MathsEditor::recalculate() {
    if (d_->module) {
        maths_.invalidate(us(d_->moduleKey()));
        d_->refreshStatus();
    }
}

// ---- expression assistance --------------------------------------------------------------------------------

bool MathsEditor::unitPopupVisible() const {
    return !d_->unitPopup->isHidden();
}

QStringList MathsEditor::unitPopupEntries() const {
    QStringList out;
    if (d_->unitPopup->isHidden()) {
        return out;
    }
    for (int i = 0; i < d_->unitPopup->count(); ++i) {
        out << d_->unitPopup->item(i)->text();
    }
    return out;
}

QStringList MathsEditor::unitsAt(int position) const {
    const std::optional<assist::UnitBracket> b = assist::unitBracketAt(d_->exprEdit->toPlainText(), position);
    if (!b) {
        return {};
    }
    const std::optional<units::QuantityId> qid = d_->quantityOfName(b->channel);
    if (!qid || *qid == units::kUnitless) {
        return {};
    }
    return d_->unitSymbols(*qid);
}

bool MathsEditor::acceptUnitPopup() {
    return d_->acceptPopup();
}

QStringList MathsEditor::rangePaths() const {
    QStringList out;
    if (const auto src = d_->scope()) {
        if (const model::DataLayer* layer = src->layer(model::kNormalLayer)) {
            layer->ranges.forEach([&out](const model::RangeGroup& g) { out << qs(g.path); });
        }
    }
    return out;
}

void MathsEditor::insertRange(const QString& groupPath) {
    d_->insertAtCaret(QLatin1Char('"') + groupPath + QLatin1Char('"'));
}

QStringList MathsEditor::channelNames() const {
    QStringList out;
    const auto src = d_->scope();
    if (src == nullptr) {
        return out;
    }
    std::set<std::string> seen;
    if (const model::DataLayer* layer = src->layer(model::kNormalLayer)) {
        for (const model::Channel& c : layer->channels) {
            if (seen.insert(caseFold(c.desc.name)).second) {
                out << qs(c.desc.name);
            }
        }
    }
    for (const qt::MathsService::StatusRow& r : maths_.statusRows()) {
        if (r.source == src->id && model::canonicalLayerName(r.layer) == model::kNormalLayer &&
            seen.insert(caseFold(r.name)).second) {
            out << qs(r.name);
        }
    }
    return out;
}

void MathsEditor::insertChannel(const QString& name) {
    QString text = QLatin1Char('\'') + name + QLatin1Char('\'');
    // The unit the channel is displayed in (its override), else the stored unit; nothing for unitless data.
    QString unit;
    if (const project::ChannelOverrides::Entry* o = session_.overrides().find(us(name));
        o != nullptr && o->displayUnit) {
        unit = qs(*o->displayUnit);
    } else if (const auto src = d_->scope()) {
        const maths::ChannelKey key = maths::ChannelKey::make(src->id, model::kNormalLayer, us(name));
        units::UnitId id = units::kNoUnit;
        if (const auto desc = maths_.descriptor(key)) {
            id = desc->unit;
        } else if (const model::DataLayer* layer = src->layer(model::kNormalLayer)) {
            if (const model::Channel* c = layer->find(us(name))) {
                id = c->desc.unit;
            }
        }
        unit = d_->unitText(id);
    }
    if (!unit.isEmpty()) {
        text += QLatin1Char('[') + unit + QLatin1Char(']');
    }
    d_->insertAtCaret(text);
}

QString MathsEditor::hoverText(int position) const {
    const QString text = d_->exprEdit->toPlainText();
    for (const int pos : {position, position - 1}) {
        if (pos < 0 || pos >= text.size()) {
            continue;
        }
        const maths::Token t = assist::tokenAt(text, pos);
        switch (t.kind) {
        case maths::TokenKind::Operator:
        case maths::TokenKind::KeywordOp:
            return assist::precedenceHelp(maths_.compat());
        case maths::TokenKind::Ident:
            if (const maths::FunctionManifest* f = maths_.functions().find(caseFold(t.text))) {
                return assist::functionHelp(*f);
            }
            return {};
        default:
            break;
        }
    }
    return {};
}

// ---- panes ------------------------------------------------------------------------------------------------

QTreeWidget& MathsEditor::modulesTree() noexcept {
    return *d_->modulesTree;
}

QTableWidget& MathsEditor::itemsTable() noexcept {
    return *d_->itemsTable;
}

QLineEdit& MathsEditor::nameEdit() noexcept {
    return *d_->nameEdit;
}

QPlainTextEdit& MathsEditor::expressionEdit() noexcept {
    return *d_->exprEdit;
}

QLabel& MathsEditor::diagnosticsLine() noexcept {
    return *d_->diagLine;
}

QListWidget& MathsEditor::diagnosticsList() noexcept {
    return *d_->diagList;
}

QLabel& MathsEditor::statusLine() noexcept {
    return *d_->statusLabel;
}

QWidget* MathsEditor::field(const QString& name) const {
    const std::pair<const char*, QWidget*> fields[] = {
        {"quantity", d_->quantityBox},       {"resultUnit", d_->resultUnitBox},
        {"displayUnit", d_->displayUnitBox}, {"rateAuto", d_->rateAutoBox},
        {"rateHz", d_->rateHzBox},           {"dps", d_->dpsBox},
        {"colour", d_->colourBox},           {"interp", d_->interpBox},
        {"comment", d_->commentEdit},        {"constValue", d_->constValueEdit},
        {"constUnit", d_->constUnitBox},     {"filterMode", d_->filterModeBox},
        {"filterValue", d_->filterValueBox}, {"soScale", d_->soScaleBox},
        {"soOffset", d_->soOffsetBox},       {"soOffsetUnit", d_->soOffsetUnitBox}};
    for (const auto& [key, widget] : fields) {
        if (name == QLatin1String(key)) {
            return widget;
        }
    }
    return nullptr;
}

// ---- closing ----------------------------------------------------------------------------------------------

void MathsEditor::closeEvent(QCloseEvent* event) {
    QString reason;
    if (!canClose(&reason)) {
        d_->showBar(tr("The editor cannot close: %1").arg(reason), true);
        event->ignore();
        return;
    }
    QDialog::closeEvent(event); // calls reject(), which applies and saves; ignored while still visible
}

void MathsEditor::reject() {
    QString reason;
    if (!canClose(&reason)) {
        d_->showBar(tr("The editor cannot close: %1").arg(reason), true);
        return;
    }
    // A valid edit is applied (03 section 11 item 9: like an item switch).
    if (d_->dirty && !d_->commit()) {
        return;
    }
    d_->hideUnitPopup();
    // Auto-save on close (D-019): the module files and the project order only (D-177 (a), D-190); the
    // project's other edits stay unsaved, undoable and journalled.
    if (d_->modulesEdited && session_.isOpen()) {
        const Result<void> saved = session_.saveMathsModules();
        if (!saved) {
            Q_EMIT message(tr("The maths modules could not be saved: %1").arg(qs(saved.error().toString())));
        }
    }
    d_->modulesEdited = false;
    QDialog::reject();
}

bool MathsEditor::eventFilter(QObject* watched, QEvent* event) {
    if (watched == d_->exprEdit && event->type() == QEvent::KeyPress && !d_->unitPopup->isHidden()) {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Down:
            d_->unitPopup->setCurrentRow(
                std::min(d_->unitPopup->currentRow() + 1, d_->unitPopup->count() - 1));
            return true;
        case Qt::Key_Up:
            d_->unitPopup->setCurrentRow(std::max(d_->unitPopup->currentRow() - 1, 0));
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab:
            return d_->acceptPopup();
        case Qt::Key_Escape:
            d_->hideUnitPopup();
            return true;
        default:
            break;
        }
    }
    if (watched == d_->exprEdit && event->type() == QEvent::FocusOut) {
        d_->hideUnitPopup();
    }
    if (watched == d_->exprEdit->viewport() && event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const int pos = d_->exprEdit->cursorForPosition(help->pos()).position();
        const QString text = hoverText(pos);
        if (text.isEmpty()) {
            QToolTip::hideText();
        } else {
            QToolTip::showText(help->globalPos(), text, d_->exprEdit->viewport());
        }
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace esses::widgets
