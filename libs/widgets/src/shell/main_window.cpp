// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/base/utf8.hpp>
#include <esses/base/version.hpp>
#include <esses/qtmodel/qtmodel.hpp>
#include <esses/widgets/components/component_widget.hpp>
#include <esses/widgets/docks/channels_window.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/docks/messages_window.hpp>
#include <esses/widgets/docks/values_window.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/key_event_router.hpp>
#include <esses/widgets/shell/layout_file.hpp>
#include <esses/widgets/shell/main_window.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMimeData>
#include <QPointer>
#include <QScreen>
#include <QStatusBar>
#include <QTabBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <algorithm>
#include <map>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace esses::widgets {

namespace {

QString qPath(const std::filesystem::path& p) {
    return QString::fromStdWString(p.wstring());
}

std::shared_ptr<const units::UnitRegistry> loadUnits(const Paths& paths, QString& error) {
    const std::filesystem::path user = paths.configDir() / "units.user.json";
    std::error_code ec;
    Result<units::UnitRegistry> r = units::UnitRegistry::loadFiles(
        paths.dataDir() / "units.json", std::filesystem::exists(user, ec) ? user : std::filesystem::path());
    if (!r) {
        error = QString::fromStdString(r.error().toString());
        return nullptr;
    }
    return std::make_shared<const units::UnitRegistry>(std::move(r).value());
}

// "view.select_worksheet" -> "Select Worksheet" (the title of a command registered before its milestone).
QString titleFromId(const QString& id) {
    QString words = id.section(QLatin1Char('.'), 1);
    words.replace(QLatin1Char('_'), QLatin1Char(' '));
    QStringList parts = words.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (QString& p : parts) {
        p[0] = p[0].toUpper();
    }
    return parts.join(QLatin1Char(' '));
}

// The menu of a command id by its area (06 section 2.2).
QString menuForId(const QString& id) {
    static const std::map<QString, QString> menus{
        {QStringLiteral("file"), QStringLiteral("File")},
        {QStringLiteral("edit"), QStringLiteral("Edit")},
        {QStringLiteral("layout"), QStringLiteral("Layout")},
        {QStringLiteral("data"), QStringLiteral("Data")},
        {QStringLiteral("component"), QStringLiteral("Component")},
        {QStringLiteral("channel"), QStringLiteral("Component/Active Channel")},
        {QStringLiteral("graph"), QStringLiteral("Component/Display")},
        {QStringLiteral("cursor"), QStringLiteral("Component/Cursor")},
        {QStringLiteral("zoom"), QStringLiteral("Component/Zoom")},
        {QStringLiteral("pan"), QStringLiteral("Component/Pan")},
        {QStringLiteral("datum"), QStringLiteral("Component/Datum")},
        {QStringLiteral("offset"), QStringLiteral("Component/Offset")},
        {QStringLiteral("track"), QStringLiteral("Component/Track")},
        {QStringLiteral("tools"), QStringLiteral("Tools")},
        {QStringLiteral("view"), QStringLiteral("View")},
        {QStringLiteral("help"), QStringLiteral("Help")}};
    const auto it = menus.find(id.section(QLatin1Char('.'), 0, 0));
    return it == menus.end() ? QString() : it->second;
}

void post(qt::MessagesModel& messages, qt::Message::Severity severity, qt::Message::Origin origin,
          const QString& source, const QString& text, const QString& code = {}) {
    qt::Message m;
    m.severity = severity;
    m.origin = origin;
    m.source = source;
    m.text = text;
    m.code = code;
    messages.append(std::move(m));
}

// Drops of application/x-esses-channels on a component that does not take drops itself: the shell adds the
// channels through ComponentWidget::addChannel (06 section 7 item 5; CTRL at the drop = new panel).
class ChannelDropFilter final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* component = qobject_cast<ComponentWidget*>(watched);
        if (component == nullptr || !component->acceptsChannelDrop()) {
            return false;
        }
        switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            auto* e = static_cast<QDropEvent*>(event);
            if (e->mimeData() != nullptr && ChannelsWindow::parseMime(*e->mimeData())) {
                e->setDropAction(Qt::CopyAction);
                e->accept();
                return true;
            }
            return false;
        }
        case QEvent::Drop: {
            auto* e = static_cast<QDropEvent*>(event);
            const auto drop =
                e->mimeData() != nullptr ? ChannelsWindow::parseMime(*e->mimeData()) : std::nullopt;
            if (!drop) {
                return false;
            }
            const bool newPanel = drop->newPanel || e->modifiers().testFlag(Qt::ControlModifier);
            for (qsizetype i = 0; i < drop->channels.size(); ++i) {
                component->addChannel(drop->channels[i], newPanel && i == 0);
            }
            e->setDropAction(Qt::CopyAction);
            e->accept();
            return true;
        }
        default:
            return false;
        }
    }
};

// The window whose hook the chunk cache calls (one hook per cache; the newest window installs it). A window
// clears the hook on destruction only while it is still the owner, so closing an older window never takes
// the hook from a newer one. GUI thread only.
struct BudgetHookOwner {
    const store::ChunkCache* cache = nullptr;
    const void* window = nullptr;
};
BudgetHookOwner gBudgetHookOwner;

} // namespace

struct MainWindow::Impl {
    Impl(qt::AppRuntime& rt, Paths p)
        : runtime(rt), paths(std::move(p)), units(loadUnits(paths, unitsError)),
          settings(std::make_unique<qt::SettingsStore>(paths.configDir() / "settings.json")),
          session(std::make_unique<qt::ProjectSession>(paths, settings.get())),
          selection(std::make_unique<qt::DataSelectionModel>(session.get())),
          workspace(std::make_unique<qt::WorkspaceViewModel>()),
          maths(std::make_unique<qt::MathsService>(rt.pool(), rt.dispatcher(), units)),
          distance(std::make_unique<qt::DistanceService>(*maths, rt.dispatcher())),
          render(
              std::make_unique<qt::RenderService>(rt.pool(), rt.dispatcher(), *selection, *maths, *distance)),
          loader(std::make_unique<qt::SourceLoader>(rt.pool(), rt.dispatcher(), units)),
          commands(std::make_unique<qt::CommandRegistry>()),
          keymap(std::make_unique<qt::KeymapBinder>(*commands)),
          messages(std::make_unique<qt::MessagesModel>()),
          channelsModel(std::make_unique<qt::ChannelsModel>(*selection, *maths)),
          valuesModel(std::make_unique<qt::ValuesModel>(*selection, *maths)) {}

    qt::AppRuntime& runtime;
    Paths paths;
    QString unitsError;
    std::shared_ptr<const units::UnitRegistry> units;
    std::unique_ptr<qt::SettingsStore> settings;
    std::unique_ptr<qt::ProjectSession> session;
    std::unique_ptr<qt::DataSelectionModel> selection;
    std::unique_ptr<qt::WorkspaceViewModel> workspace;
    std::unique_ptr<qt::MathsService> maths;
    std::unique_ptr<qt::DistanceService> distance;
    std::unique_ptr<qt::RenderService> render;
    std::unique_ptr<qt::SourceLoader> loader;
    std::unique_ptr<qt::CommandRegistry> commands;
    std::unique_ptr<qt::KeymapBinder> keymap;
    std::unique_ptr<qt::MessagesModel> messages;
    std::unique_ptr<qt::ChannelsModel> channelsModel;
    std::unique_ptr<qt::ValuesModel> valuesModel;
    // One per worksheet of the active workbook; contexts reference the view models (declared after them, so
    // destroyed first).
    std::vector<std::unique_ptr<qt::WorksheetViewModel>> worksheets;
    std::vector<std::unique_ptr<WorksheetContext>> contexts;
    int activeWorksheet = -1;
    LayoutFile layout;
    bool layoutSaved = false;
    bool saveLayoutOnExit = true;

    QWidget* central = nullptr;
    QComboBox* workbookCombo = nullptr;
    QTabBar* tabs = nullptr;
    WorksheetCanvas* canvas = nullptr;
    DockHost* docks = nullptr;
    CommandActions* actions = nullptr;
    KeyEventRouter* router = nullptr;
    QToolBar* toolBar = nullptr;
    DataWindow* dataWindow = nullptr;
    ChannelsWindow* channelsWindow = nullptr;
    ValuesWindow* valuesWindow = nullptr;
    MessagesWindow* messagesWindow = nullptr;
    ChannelDropFilter* dropFilter = nullptr;
    std::map<QString, QLabel*> status;
    QMetaObject::Connection cursorInfo;
    QPointer<ComponentWidget> lastComponent; // view.return_focus target
    std::shared_ptr<ILogSink> messagesSink;  // this window's Messages log sink (removed with the window)
};

MainWindow::MainWindow(qt::AppRuntime& runtime, Paths paths, QWidget* parent)
    : QMainWindow(parent), impl_(std::make_unique<Impl>(runtime, std::move(paths))) {
    Impl& d = *impl_;
    setObjectName(QStringLiteral("esses.mainWindow"));
    if (!d.unitsError.isEmpty()) {
        post(*impl_->messages, qt::Message::Severity::Error, qt::Message::Origin::System,
             QStringLiteral("units.json"), tr("The unit table could not be loaded: %1").arg(d.unitsError));
    }
    if (const Result<void> r = d.settings->load(); !r) {
        post(*impl_->messages, qt::Message::Severity::Warning, qt::Message::Origin::System,
             qPath(d.settings->file()), QString::fromStdString(r.error().toString()));
    }
    connect(d.session.get(), &qt::ProjectSession::logMessage, this, [this](const QString& text) {
        post(*impl_->messages, qt::Message::Severity::Info, qt::Message::Origin::Project, {}, text);
    });
    if (const Result<void> r = d.session->loadDefaults(); !r) {
        post(*impl_->messages, qt::Message::Severity::Error, qt::Message::Origin::Project,
             qPath(d.paths.dataDir()),
             tr("The shipped defaults could not be loaded: %1")
                 .arg(QString::fromStdString(r.error().toString())));
    }
    wireModels();
    buildWidgets();
    registerShellCommands();
    qt::registerSelectionCommands(*d.commands, *d.selection);
    qt::registerViewCommands(*d.commands, [self = QPointer<MainWindow>(this)]() -> qt::WorksheetViewModel* {
        return self.isNull() ? nullptr : self->activeWorksheet();
    });
    // The toolbar's Overlays button shows the overlays (checked) or hides them (F4, 06 section 2.1).
    if (const qt::CommandSpec* f4 = d.commands->find(QStringLiteral("data.overlays_toggle"))) {
        qt::CommandSpec s = *f4;
        s.title = tr("Overlays");
        s.checkable = true;
        s.checked = [sel = QPointer<qt::DataSelectionModel>(d.selection.get())] {
            return !sel.isNull() && !sel->snapshot().overlaysHidden;
        };
        d.commands->add(std::move(s));
    }
    registerPlannedCommands();
    const QStringList rejected = d.keymap->setKeymap(d.session->keymap());
    for (const QString& line : rejected) {
        post(*impl_->messages, qt::Message::Severity::Warning, qt::Message::Origin::System,
             QStringLiteral("keymap"), line);
    }

    // Menus (06 section 2.2) in the keymap row order; commands that need arguments stay out of the menus.
    QStringList order;
    QStringList withArgs;
    for (const project::KeyBinding& row : d.session->keymap().bindings) {
        const QString id = QString::fromStdString(row.command);
        if (!order.contains(id)) {
            order << id;
        }
        if (!row.args.isNull() && !withArgs.contains(id)) {
            withArgs << id;
        }
    }
    d.actions->populateMenuBar(*menuBar(), order, withArgs);
    d.actions->populateToolBar(
        *d.toolBar, {QStringLiteral("file.open_log"), QStringLiteral("file.save_project"), QString(),
                     QStringLiteral("data.toggle_domain"), QStringLiteral("data.overlays_toggle"),
                     QStringLiteral("data.single_colour_main")});

    d.router = new KeyEventRouter(*d.keymap, *d.docks, *d.canvas, this);
    d.router->setScope(this);
    if (qApp != nullptr) {
        qApp->installEventFilter(d.router);
    }

    rebuildWorksheets();
    restoreLayout();
    updateTitle();
    d.actions->refresh();
}

MainWindow::~MainWindow() {
    Impl& d = *impl_;
    if (d.saveLayoutOnExit && !d.layoutSaved) {
        saveLayout();
    }
    if (gBudgetHookOwner.cache == &d.runtime.cache() && gBudgetHookOwner.window == this) {
        d.runtime.cache().setBudgetWarningHook({});
        gBudgetHookOwner = {};
    }
    if (d.messagesSink != nullptr) {
        d.runtime.removeSink(d.messagesSink);
        d.messagesSink.reset();
    }
    if (d.router != nullptr && qApp != nullptr) {
        qApp->removeEventFilter(d.router);
    }
    // Widgets reference the models: tear them down before the models (impl_). The components reference the
    // worksheet contexts; the DockHost's backend goes before the dock widgets it tracks.
    disconnect(d.cursorInfo);
    d.canvas->clear();
    d.valuesWindow->setWorksheet(nullptr);
    delete d.docks;
    d.docks = nullptr;
    const QList<QWidget*> kids = findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
    qDeleteAll(kids);
    impl_.reset();
}

// ---- assembly ---------------------------------------------------------------------------------------------

void MainWindow::wireModels() {
    Impl& d = *impl_;
    if (const Result<void> r = d.maths->loadSettingsSchema(d.paths.dataDir()); !r) {
        post(*impl_->messages, qt::Message::Severity::Warning, qt::Message::Origin::Maths, {},
             tr("The corrected speed settings schema could not be loaded: %1")
                 .arg(QString::fromStdString(r.error().toString())));
    }
    d.maths->setCorrectedSpeedSettings(d.session->correctedSpeedSettings());
    d.maths->setCorrectedSpeedEnabled(d.session->correctedSpeedEnabled());
    d.loader->setRuleRunner(d.session->ruleRunner());
    d.loader->setMathsService(d.maths.get());
    d.loader->setDistanceService(d.distance.get());
    qt::connectSourceLoader(*d.loader, *d.selection, d.messages.get());
    d.messagesSink = qt::makeMessagesLogSink(*d.messages, d.runtime.dispatcher());
    d.runtime.addSink(d.messagesSink);

    // Chunk-cache budget warnings (D-011, 01 section 7.2) -> Messages (Budget, Warning). The hook runs on a
    // worker; the guard is made here on the GUI thread and only copied there.
    const QPointer<qt::MessagesModel> guard(d.messages.get());
    qt::Dispatcher* dispatcher = &d.runtime.dispatcher();
    gBudgetHookOwner = {&d.runtime.cache(), this};
    d.runtime.cache().setBudgetWarningHook([guard, dispatcher](const store::ChunkCache::BudgetWarning& w) {
        qt::Message m;
        m.severity = qt::Message::Severity::Warning;
        m.origin = qt::Message::Origin::Budget;
        m.source = QStringLiteral("chunk cache");
        m.code = QStringLiteral("budget.chunk_cache");
        m.text = QStringLiteral("The chunk cache holds %1 MiB, %2 % of its %3 MiB budget")
                     .arg(static_cast<double>(w.residentBytes) / 1048576.0, 0, 'f', 0)
                     .arg(w.percent)
                     .arg(static_cast<double>(w.budgetBytes) / 1048576.0, 0, 'f', 0);
        dispatcher->post(dispatcher, [guard, m = std::move(m)]() mutable {
            if (qt::MessagesModel* model = guard.data()) {
                model->append(std::move(m));
            }
        });
    });

    connect(d.maths.get(), &qt::MathsService::moduleMessage, this,
            [this](const model::SourceId& source, const QString& code, const QString& text) {
                const auto src = impl_->selection->source(source);
                const QString name = src == nullptr ? QString() : qPath(src->prov.path.filename());
                post(*impl_->messages,
                     code.startsWith(QLatin1String("E-")) ? qt::Message::Severity::Error
                                                          : qt::Message::Severity::Warning,
                     qt::Message::Origin::Maths, name, text, code);
            });
    connect(d.loader.get(), &qt::SourceLoader::sourcePublished, this,
            [this](std::shared_ptr<const model::DataSource> source) {
                if (source != nullptr) {
                    impl_->session->addRecentSource(*source);
                }
            });
    connect(d.selection.get(), &qt::DataSelectionModel::hint, this,
            [this](const QString& text) { statusBar()->showMessage(text, 5000); });
    connect(d.selection.get(), &qt::DataSelectionModel::changed, this, [this] { impl_->actions->refresh(); });
    connect(d.selection.get(), &qt::DataSelectionModel::sourcesChanged, this,
            [this] { impl_->actions->refresh(); });
    connect(d.messages.get(), &qt::MessagesModel::rowsInserted, this,
            [this](const QModelIndex&, int first, int last) {
                for (int r = first; r <= last; ++r) {
                    const qt::Message& m = impl_->messages->at(r);
                    if (m.severity == qt::Message::Severity::Error) {
                        statusBar()->showMessage(m.text,
                                                 5000); // an Error flashes the message area (06 section 11)
                    }
                }
            });
    connect(d.messages.get(), &qt::MessagesModel::firstError, this, [this] {
        if (!impl_->settings->value(QStringLiteral("ui.doNotAutoShowMessages"), false).toBool() &&
            impl_->docks != nullptr) {
            impl_->docks->show(QString::fromLatin1(MessagesWindow::kDockId));
        }
    });
}

void MainWindow::buildWidgets() {
    Impl& d = *impl_;
    d.central = new QWidget(this);
    d.central->setObjectName(QStringLiteral("esses.central"));
    d.workbookCombo = new QComboBox(d.central);
    d.workbookCombo->setObjectName(QStringLiteral("workbookSelector"));
    d.workbookCombo->setAccessibleName(tr("Workbook"));
    d.workbookCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    d.tabs = new QTabBar(d.central);
    d.tabs->setObjectName(QStringLiteral("worksheetTabs"));
    d.tabs->setAccessibleName(tr("Worksheets"));
    d.tabs->setExpanding(false);
    d.tabs->setDocumentMode(true);
    d.tabs->setFocusPolicy(Qt::NoFocus);
    d.canvas = new WorksheetCanvas(d.central);
    d.canvas->setObjectName(QStringLiteral("worksheetCanvas"));
    auto* top = new QHBoxLayout;
    top->setContentsMargins(4, 2, 4, 0);
    top->addWidget(d.workbookCombo);
    top->addWidget(d.tabs, 1);
    auto* column = new QVBoxLayout(d.central);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addLayout(top);
    column->addWidget(d.canvas, 1);
    connect(d.tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index != impl_->activeWorksheet) {
            selectWorksheet(index);
        }
    });
    connect(d.workbookCombo, &QComboBox::activated, this, &MainWindow::selectWorkbook);

    d.docks = new DockHost(*this, d.central);
    d.actions = new CommandActions(*d.commands, *d.keymap, this);
    d.dataWindow = new DataWindow(*d.selection, *d.commands, this);
    d.dataWindow->setActions(d.actions);
    d.dataWindow->setScheme(d.session->scheme());
    d.dataWindow->setCloseSource(
        [loader = QPointer<qt::SourceLoader>(d.loader.get())](const model::SourceId& id) {
            if (!loader.isNull()) {
                loader->close(id);
            }
        });
    d.channelsWindow = new ChannelsWindow(*d.channelsModel, *d.commands, this);
    d.valuesWindow = new ValuesWindow(*d.valuesModel, this);
    d.messagesWindow = new MessagesWindow(*d.messages, *d.commands, this);
    d.docks->addDock(QString::fromLatin1(DataWindow::kDockId), tr("Data"), d.dataWindow, DockArea::Left,
                     DockState::Docked);
    d.docks->addDock(QString::fromLatin1(ChannelsWindow::kDockId), tr("Channels"), d.channelsWindow,
                     DockArea::Left, DockState::Docked);
    d.docks->addDock(QString::fromLatin1(ValuesWindow::kDockId), tr("Values"), d.valuesWindow,
                     DockArea::Right, DockState::Docked);
    d.docks->addDock(QString::fromLatin1(MessagesWindow::kDockId), tr("Messages"), d.messagesWindow,
                     DockArea::Bottom, DockState::AutoHide);
    connect(d.docks, &DockHost::visibilityChanged, this, [this] { impl_->actions->refresh(); });
    // Checked and enabled states change with every command (F9 flips the toolbar's Time/Distance button).
    connect(d.commands.get(), &qt::CommandRegistry::invoked, this, [this] { impl_->actions->refresh(); });

    d.toolBar = addToolBar(tr("Main"));
    d.toolBar->setObjectName(QStringLiteral("mainToolBar"));
    d.toolBar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    for (const char* name : {"t", "d", "lap", "dt", "dd"}) {
        auto* label = new QLabel(statusBar());
        label->setObjectName(QStringLiteral("status.") + QLatin1String(name));
        label->setMinimumWidth(QFontMetrics(label->font()).horizontalAdvance(QStringLiteral("d 00000.0 m")));
        statusBar()->addPermanentWidget(label);
        d.status.emplace(QString::fromLatin1(name), label);
    }

    d.dropFilter = new ChannelDropFilter(this);
    // A component edit (add channel, scale, active channel) lands in the session's workbook (M1: no undo; the
    // project is saved with file.save_project).
    connect(d.canvas, &WorksheetCanvas::componentEdited, this, [this] {
        Impl& i = *impl_;
        const int wb = i.session->activeWorkbook();
        const auto& books = i.session->workbooks();
        if (wb < 0 || static_cast<std::size_t>(wb) >= books.size() || i.activeWorksheet < 0 ||
            static_cast<std::size_t>(i.activeWorksheet) >=
                books[static_cast<std::size_t>(wb)].worksheets.size()) {
            return;
        }
        project::WorkbookFile book = books[static_cast<std::size_t>(wb)];
        book.worksheets[static_cast<std::size_t>(i.activeWorksheet)] = i.canvas->worksheet();
        i.session->replaceWorkbook(wb, std::move(book));
        i.valuesWindow->refreshChannels();
    });
    connect(d.canvas, &WorksheetCanvas::focusedComponentChanged, this, [this](ComponentWidget* component) {
        if (component != nullptr) {
            impl_->lastComponent = component;
        }
        impl_->valuesWindow->followComponent(component);
    });
    connect(d.channelsWindow, &ChannelsWindow::addChannelRequested, this,
            [this](const QString& name, bool newPanel) {
                ComponentWidget* c = impl_->canvas->focusedComponent();
                if (c == nullptr) {
                    c = impl_->lastComponent.data();
                }
                if (c == nullptr || !c->acceptsChannelDrop()) {
                    statusBar()->showMessage(tr("The focused component does not take channels"), 5000);
                    return;
                }
                c->addChannel(name, newPanel);
                impl_->valuesWindow->refreshChannels();
            });
    connect(d.valuesWindow, &ValuesWindow::allChannelsChanged, this, [this](bool on) {
        impl_->layout.setDockOption(QString::fromLatin1(ValuesWindow::kDockId), QStringLiteral("allChannels"),
                                    on);
    });
}

void MainWindow::registerShellCommands() {
    Impl& d = *impl_;
    const QPointer<MainWindow> self(this);
    using C = qt::CommandContext;
    const auto add = [&](const char* id, const QString& title, const char* menu, C context, QStringList keys,
                         std::function<void(MainWindow&, const QVariantList&)> run,
                         std::function<bool(MainWindow&)> enabled = {},
                         std::function<bool(MainWindow&)> checked = {}) {
        qt::CommandSpec s;
        s.id = QString::fromLatin1(id);
        s.title = title;
        s.menuPath = QString::fromLatin1(menu);
        s.context = context;
        s.defaultKeys = std::move(keys);
        s.invoke = [self, run = std::move(run)](const QVariantList& args) {
            if (!self.isNull() && self->impl_ != nullptr) {
                run(*self, args);
            }
        };
        if (enabled) {
            s.enabled = [self, enabled = std::move(enabled)] {
                return !self.isNull() && self->impl_ != nullptr && enabled(*self);
            };
        }
        if (checked) {
            s.checkable = true;
            s.checked = [self, checked = std::move(checked)] {
                return !self.isNull() && self->impl_ != nullptr && checked(*self);
            };
        }
        d.commands->add(std::move(s));
    };
    const QString dataDock = QString::fromLatin1(DataWindow::kDockId);
    const QString channelsDock = QString::fromLatin1(ChannelsWindow::kDockId);
    const QString valuesDock = QString::fromLatin1(ValuesWindow::kDockId);
    const QString messagesDock = QString::fromLatin1(MessagesWindow::kDockId);

    add("file.open_log", tr("Open Log File…"), "File", C::Global, {QStringLiteral("Ctrl+O")},
        [](MainWindow& w, const QVariantList&) {
            const QString folder =
                w.impl_->settings->value(QStringLiteral("openDialog.lastFolder")).toString();
            const QStringList files = QFileDialog::getOpenFileNames(&w, tr("Open Log File"), folder);
            for (const QString& f : files) {
                w.openLog(f.toStdWString());
            }
            if (!files.isEmpty()) {
                w.impl_->settings->setValue(QStringLiteral("openDialog.lastFolder"),
                                            QFileInfo(files.front()).absolutePath());
            }
        });
    add(
        "file.save_project", tr("Save Project"), "File", C::Global, {QStringLiteral("Ctrl+S")},
        [](MainWindow& w, const QVariantList&) {
            if (const Result<void> r = w.impl_->session->save(); !r) {
                post(*w.impl_->messages, qt::Message::Severity::Error, qt::Message::Origin::Project,
                     qPath(w.impl_->session->folder().value_or(std::filesystem::path())),
                     tr("The project could not be saved: %1")
                         .arg(QString::fromStdString(r.error().toString())));
            } else {
                w.statusBar()->showMessage(tr("Project saved"), 5000);
            }
        },
        [](MainWindow& w) { return w.impl_->session->isOpen(); });
    add("file.open_project", tr("Open Project…"), "File", C::Global, {},
        [](MainWindow& w, const QVariantList&) {
            const QString folder = QFileDialog::getExistingDirectory(&w, tr("Open Project"),
                                                                     qPath(w.impl_->paths.projectsDir()));
            if (!folder.isEmpty()) {
                w.openProject(folder.toStdWString());
            }
        });
    add("view.data_window", tr("Data Window"), "View", C::Global, {QStringLiteral("F8")},
        [dataDock](MainWindow& w, const QVariantList&) { w.impl_->docks->focus(dataDock); });
    add("view.channels_window", tr("Channels Window"), "View", C::Worksheet, {QStringLiteral("C")},
        [channelsDock](MainWindow& w, const QVariantList&) { w.impl_->docks->focus(channelsDock); });
    add(
        "view.values_window", tr("Values Window"), "View", C::Worksheet, {QStringLiteral("V")},
        [valuesDock](MainWindow& w, const QVariantList&) { w.impl_->docks->toggle(valuesDock); }, {},
        [valuesDock](MainWindow& w) { return w.impl_->docks->isVisible(valuesDock); });
    add(
        "view.messages_window", tr("Messages"), "View", C::Global, {},
        [messagesDock](MainWindow& w, const QVariantList&) { w.impl_->docks->toggle(messagesDock); }, {},
        [messagesDock](MainWindow& w) { return w.impl_->docks->isVisible(messagesDock); });
    add("view.return_focus", tr("Return Focus to Worksheet"), "View", C::Global, {QStringLiteral("Esc")},
        [](MainWindow& w, const QVariantList&) { w.returnFocus(); });
    add("view.reset_docks", tr("Reset Dock Windows"), "View", C::Global, {},
        [](MainWindow& w, const QVariantList&) { w.resetDocks(); });
    add(
        "view.worksheet_n", tr("Select Worksheet N"), "View", C::Worksheet, {},
        [](MainWindow& w, const QVariantList& args) {
            const int n = args.isEmpty() ? 0 : args.front().toInt();
            if (n >= 1 && n <= w.worksheetCount()) {
                w.selectWorksheet(n - 1); // keys 1-9 select tabs 1-9, key 0 the tenth (args 10)
            }
        },
        [](MainWindow& w) { return w.worksheetCount() > 0; });
    add(
        "view.prev_worksheet", tr("Previous Worksheet"), "View", C::Global, {QStringLiteral("F11")},
        [](MainWindow& w, const QVariantList&) {
            const int n = w.worksheetCount();
            w.selectWorksheet((w.activeWorksheetIndex() + n - 1) % n);
        },
        [](MainWindow& w) { return w.worksheetCount() > 0; });
    add(
        "view.next_worksheet", tr("Next Worksheet"), "View", C::Global, {QStringLiteral("F12")},
        [](MainWindow& w, const QVariantList&) {
            const int n = w.worksheetCount();
            w.selectWorksheet((w.activeWorksheetIndex() + 1) % n);
        },
        [](MainWindow& w) { return w.worksheetCount() > 0; });
}

// Every keymap command no layer registered yet: bound, disabled, with the milestone in the tool tip (06
// section 14.4; reserved rows share the generic "reserved" note of 06 section 14.5).
void MainWindow::registerPlannedCommands() {
    Impl& d = *impl_;
    for (const project::KeyBinding& row : d.session->keymap().bindings) {
        const QString id = QString::fromStdString(row.command);
        if (d.commands->find(id) == nullptr) {
            qt::CommandSpec s;
            s.id = id;
            s.title = titleFromId(id);
            s.menuPath = menuForId(id);
            s.context = qt::parseCommandContext(QString::fromStdString(row.context))
                            .value_or(qt::CommandContext::Global);
            for (const std::string& k : row.keys) {
                s.defaultKeys << QString::fromStdString(k);
            }
            d.commands->add(std::move(s));
        }
        if (!d.commands->isEnabled(id) && d.commands->find(id)->invoke == nullptr) {
            d.actions->setNote(id, row.status == project::KeymapStatus::Reserved
                                       ? tr("reserved")
                                       : tr("planned for %1").arg(QString::fromStdString(row.milestone)));
        }
    }
}

// ---- worksheets -------------------------------------------------------------------------------------------

namespace {

std::string groupPathFor(const project::Profile& profile, const std::string& linkTo) {
    for (const project::DataView& v : profile.dataViews) {
        if (v.title == linkTo && !v.tree.empty()) {
            return v.tree.back();
        }
    }
    return profile.primaryRanges.empty() ? std::string("Outings:Laps") : profile.primaryRanges;
}

} // namespace

void MainWindow::rebuildWorksheets() {
    Impl& d = *impl_;
    disconnect(d.cursorInfo);
    d.canvas->clear();
    d.valuesWindow->setWorksheet(nullptr);
    d.contexts.clear();
    d.worksheets.clear();
    d.activeWorksheet = -1;
    {
        const QSignalBlocker block(d.tabs);
        while (d.tabs->count() > 0) {
            d.tabs->removeTab(0);
        }
    }
    const auto& books = d.session->workbooks();
    const int wb = d.session->activeWorkbook();
    if (wb >= 0 && static_cast<std::size_t>(wb) < books.size()) {
        const project::WorkbookFile& book = books[static_cast<std::size_t>(wb)];
        for (const project::WorksheetDef& def : book.worksheets) {
            auto vm = std::make_unique<qt::WorksheetViewModel>(def, *d.workspace, *d.selection, *d.distance);
            d.contexts.push_back(std::unique_ptr<WorksheetContext>(
                new WorksheetContext{*vm, *d.selection, *d.maths, *d.render, *d.commands, d.session->scheme(),
                                     QString::fromStdString(def.id.str())}));
            d.worksheets.push_back(std::move(vm));
        }
        const QSignalBlocker block(d.tabs);
        for (std::size_t i = 0; i < book.worksheets.size(); ++i) {
            const int t = d.tabs->addTab(QString::fromStdString(book.worksheets[i].name));
            if (i < 10) {
                d.tabs->setTabToolTip(t, tr("Worksheet %1 (key %2)").arg(i + 1).arg((i + 1) % 10));
            }
        }
    }
    refreshWorkbookSelector();
}

void MainWindow::refreshWorkbookSelector() {
    Impl& d = *impl_;
    const QSignalBlocker block(d.workbookCombo);
    d.workbookCombo->clear();
    for (const project::WorkbookFile& book : d.session->workbooks()) {
        d.workbookCombo->addItem(QString::fromStdString(book.name), QString::fromStdString(book.id.str()));
    }
    d.workbookCombo->setCurrentIndex(d.session->activeWorkbook());
}

int MainWindow::worksheetCount() const {
    return impl_ == nullptr ? 0 : static_cast<int>(impl_->worksheets.size());
}

int MainWindow::activeWorksheetIndex() const {
    return impl_ == nullptr ? -1 : impl_->activeWorksheet;
}

void MainWindow::selectWorksheet(int index) {
    Impl& d = *impl_;
    if (index < 0 || index >= worksheetCount() || index == d.activeWorksheet) {
        return; // the shown worksheet stays as it is (its components and their layers are kept, D-058)
    }
    const bool focusInCanvas =
        QApplication::focusWidget() != nullptr &&
        (QApplication::focusWidget() == d.canvas || d.canvas->isAncestorOf(QApplication::focusWidget()));
    d.activeWorksheet = index;
    qt::WorksheetViewModel& vm = *d.worksheets[static_cast<std::size_t>(index)];
    vm.activate();
    d.selection->setDisplayedGroupPath(groupPathFor(d.session->profile(), vm.definition().linkTo));
    d.valuesWindow->setWorksheet(&vm);
    disconnect(d.cursorInfo);
    for (auto& [name, label] : d.status) {
        label->clear();
    }
    d.cursorInfo =
        connect(&vm, &qt::WorksheetViewModel::cursorInfo, this,
                [this](const QString& t, const QString& dist, const QString& lap, const QString& dt,
                       const QString& dd) {
                    Impl& i = *impl_;
                    const auto field = [&i](const char* name, const QString& prefix, const QString& value) {
                        i.status[QString::fromLatin1(name)]->setText(
                            value.isEmpty() || prefix.isEmpty() ? value : prefix + value);
                    };
                    field("t", QStringLiteral("t "), t);
                    field("d", QStringLiteral("d "), dist);
                    field("lap", QString(), lap);
                    field("dt", QStringLiteral("Δt "), dt);
                    field("dd", QStringLiteral("Δd "), dd);
                });
    // The session's definition: component edits (componentEdited) are stored there, not in the view model.
    const auto& books = d.session->workbooks();
    const int wb = d.session->activeWorkbook();
    const project::WorksheetDef& def =
        wb >= 0 && static_cast<std::size_t>(wb) < books.size() &&
                static_cast<std::size_t>(index) < books[static_cast<std::size_t>(wb)].worksheets.size()
            ? books[static_cast<std::size_t>(wb)].worksheets[static_cast<std::size_t>(index)]
            : vm.definition();
    d.canvas->setWorksheet(def, *d.contexts[static_cast<std::size_t>(index)]);
    for (ComponentWidget* c : d.canvas->components()) {
        if (!c->acceptDrops()) {
            c->setAcceptDrops(true);
            c->installEventFilter(d.dropFilter);
        }
    }
    d.lastComponent = d.canvas->focusedComponent();
    d.valuesWindow->followComponent(d.canvas->focusedComponent());
    {
        const QSignalBlocker block(d.tabs);
        d.tabs->setCurrentIndex(index);
    }
    if (focusInCanvas) {
        returnFocus();
    }
    d.actions->refresh();
    Q_EMIT worksheetActivated(index);
}

void MainWindow::selectWorkbook(int index) {
    Impl& d = *impl_;
    const auto& books = d.session->workbooks();
    if (index < 0 || static_cast<std::size_t>(index) >= books.size() ||
        index == d.session->activeWorkbook()) {
        refreshWorkbookSelector();
        return;
    }
    // The leaving workbook keeps its perspective; the arriving one restores its own if it has one (06 3.4).
    const QString leaving =
        QString::fromStdString(books[static_cast<std::size_t>(d.session->activeWorkbook())].id.str());
    d.layout.setPerspective(leaving, d.docks->savePerspective());
    d.session->setActiveWorkbook(index);
    rebuildWorksheets();
    const project::WorkbookFile& book = d.session->workbooks()[static_cast<std::size_t>(index)];
    const QByteArray perspective = d.layout.perspective(QString::fromStdString(book.id.str()));
    if (!perspective.isEmpty() && !d.docks->restorePerspective(perspective)) {
        d.docks->resetToDefault();
    }
    int sheet = 0;
    if (book.activeWorksheet) {
        for (std::size_t i = 0; i < book.worksheets.size(); ++i) {
            if (book.worksheets[i].name == *book.activeWorksheet) {
                sheet = static_cast<int>(i);
            }
        }
    }
    selectWorksheet(sheet);
    saveLayout(); // written on workbook switch (06 section 3.4)
}

void MainWindow::returnFocus() {
    Impl& d = *impl_;
    QWidget* focus = QApplication::focusWidget();
    const bool inCanvas = focus != nullptr && (focus == d.canvas || d.canvas->isAncestorOf(focus));
    // ESC in the worksheet first ends a gesture in progress in the focused component (the rubber band, 04
    // section 8.6), then restores a maximised component (06 section 14.4 view.return_focus).
    if (inCanvas) {
        if (ComponentWidget* c = d.canvas->focusedComponent(); c != nullptr && c->cancelTransient()) {
            return;
        }
        if (d.canvas->restoreMaximised()) {
            return;
        }
    }
    d.canvas->returnFocus();
    focus = QApplication::focusWidget();
    if (focus == nullptr || (focus != d.canvas && !d.canvas->isAncestorOf(focus))) {
        d.canvas->setFocus(Qt::OtherFocusReason); // an empty worksheet
    }
}

// ---- files ------------------------------------------------------------------------------------------------

void MainWindow::openLog(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::filesystem::path p = ec ? path : absolute;
    impl_->loader->open(p);
    impl_->settings->addRecentLog(qPath(p));
    Q_EMIT sourceOpened(qPath(p));
}

bool MainWindow::openProject(const std::filesystem::path& folder) {
    Impl& d = *impl_;
    std::filesystem::path f = folder;
    if (f.filename() == "project.json") {
        f = f.parent_path();
    }
    std::error_code ec;
    if (const std::filesystem::path a = std::filesystem::absolute(f, ec); !ec) {
        f = a;
    }
    saveLayout(); // the layout of the state being left (06 section 3.4: written on project close)
    if (const Result<void> r = d.session->openProject(f); !r) {
        post(*impl_->messages, qt::Message::Severity::Error, qt::Message::Origin::Project, qPath(f),
             tr("The project could not be opened: %1").arg(QString::fromStdString(r.error().toString())));
        return false;
    }
    d.settings->addRecentProject(qPath(f));
    d.maths->setCorrectedSpeedSettings(d.session->correctedSpeedSettings());
    d.maths->setCorrectedSpeedEnabled(d.session->correctedSpeedEnabled());
    d.loader->setRuleRunner(d.session->ruleRunner());
    d.dataWindow->setScheme(d.session->scheme());
    rebuildWorksheets();
    restoreLayout();
    updateTitle();
    d.actions->refresh();
    return true;
}

void MainWindow::updateTitle() {
    Impl& d = *impl_;
    if (d.session->isOpen()) {
        setWindowTitle(QString::fromStdString(d.session->projectFile().name) + QStringLiteral(" - Esses"));
    } else {
        setWindowTitle(QStringLiteral("Esses"));
    }
}

// ---- layout (06 sections 3.4 and 18) ----------------------------------------------------------------------

std::filesystem::path MainWindow::layoutFilePath() const {
    const Impl& d = *impl_;
    if (d.session->isOpen() && d.session->folder()) {
        return *d.session->folder() / ".local" / "layout.json";
    }
    return d.paths.configDir() / "layout.json";
}

void MainWindow::restoreLayout() {
    Impl& d = *impl_;
    QString error;
    if (!LayoutFile::load(layoutFilePath(), d.layout, &error)) {
        post(*impl_->messages, qt::Message::Severity::Info, qt::Message::Origin::Project,
             qPath(layoutFilePath()),
             tr("The saved layout could not be read (%1); the docks use the default layout").arg(error));
        d.docks->resetToDefault();
    }
    const QByteArray geometry = d.layout.windowGeometry();
    if (geometry.isEmpty() || !restoreGeometry(geometry)) {
        resize(1366, 768);
    }
    if (d.layout.windowMaximised()) {
        setWindowState(windowState() | Qt::WindowMaximized);
    }
    const auto& books = d.session->workbooks();
    const int wb = d.session->activeWorkbook();
    const project::WorkbookFile* book = wb >= 0 && static_cast<std::size_t>(wb) < books.size()
                                            ? &books[static_cast<std::size_t>(wb)]
                                            : nullptr;
    const QString bookId = book == nullptr ? QString() : QString::fromStdString(book->id.str());
    QByteArray perspective = d.layout.perspective(bookId);
    if (perspective.isEmpty()) {
        perspective = d.layout.perspective(QString::fromLatin1(LayoutFile::kDefaultPerspective));
    }
    if (perspective.isEmpty()) {
        d.docks->resetToDefault(); // a missing file is not an error: the default perspective (02 section 6.7)
    } else if (!d.docks->restorePerspective(perspective)) {
        post(*impl_->messages, qt::Message::Severity::Info, qt::Message::Origin::Project,
             qPath(layoutFilePath()),
             tr("The saved dock layout does not fit this version (written by %1, %2); the docks use the "
                "default layout")
                 .arg(d.layout.appVersion(), d.layout.dockBackend()));
        d.docks->resetToDefault();
    }
    const QJsonValue allChannels =
        d.layout.dock(QString::fromLatin1(ValuesWindow::kDockId)).value(QStringLiteral("allChannels"));
    if (allChannels.isBool()) {
        d.valuesWindow->setAllChannels(allChannels.toBool());
    }
    // The active worksheet: layout.json "active" for this workbook, else the workbook's activeWorksheet.
    int sheet = 0;
    if (book != nullptr) {
        std::optional<QString> name;
        if (d.layout.activeWorkbook() == bookId) {
            name = d.layout.activeWorksheet();
        }
        if (!name && book->activeWorksheet) {
            name = QString::fromStdString(*book->activeWorksheet);
        }
        for (std::size_t i = 0; name && i < book->worksheets.size(); ++i) {
            if (QString::fromStdString(book->worksheets[i].name) == *name) {
                sheet = static_cast<int>(i);
                break;
            }
        }
    }
    if (worksheetCount() > 0) {
        selectWorksheet(sheet);
    }
    d.layoutSaved = false;
}

bool MainWindow::saveLayout() {
    Impl& d = *impl_;
    d.layout.setVersions(
        QString::fromLatin1(versionString().data(), static_cast<qsizetype>(versionString().size())),
        QString::fromLatin1(qVersion()), QString::fromLatin1(d.docks->backend().name()),
        d.docks->backend().libraryVersion());
    const QScreen* s = screen();
    d.layout.setWindow(saveGeometry(), isMaximized(), s == nullptr ? QString() : s->name(),
                       devicePixelRatioF());
    const QByteArray perspective = d.docks->savePerspective();
    const auto& books = d.session->workbooks();
    const int wb = d.session->activeWorkbook();
    QString bookId;
    QString sheetName;
    if (wb >= 0 && static_cast<std::size_t>(wb) < books.size()) {
        const project::WorkbookFile& book = books[static_cast<std::size_t>(wb)];
        bookId = QString::fromStdString(book.id.str());
        if (d.activeWorksheet >= 0 && static_cast<std::size_t>(d.activeWorksheet) < book.worksheets.size()) {
            sheetName =
                QString::fromStdString(book.worksheets[static_cast<std::size_t>(d.activeWorksheet)].name);
        }
        d.layout.setPerspective(bookId, perspective);
    }
    d.layout.setPerspective(QString::fromLatin1(LayoutFile::kDefaultPerspective), perspective);
    d.layout.setActive(bookId, sheetName);
    for (const QString& id : d.docks->dockIds()) {
        d.layout.setDockState(id, dockStateName(d.docks->state(id)));
    }
    d.layout.setDockOption(QString::fromLatin1(ValuesWindow::kDockId), QStringLiteral("allChannels"),
                           d.valuesWindow->allChannels());
    QString error;
    if (!d.layout.save(layoutFilePath(), &error)) {
        post(*impl_->messages, qt::Message::Severity::Warning, qt::Message::Origin::Project,
             qPath(layoutFilePath()), tr("The layout could not be saved: %1").arg(error));
        return false;
    }
    d.layoutSaved = true;
    return true;
}

void MainWindow::resetDocks() {
    Impl& d = *impl_;
    d.docks->resetToDefault();
    const auto& books = d.session->workbooks();
    const int wb = d.session->activeWorkbook();
    if (wb >= 0 && static_cast<std::size_t>(wb) < books.size()) {
        d.layout.removePerspective(QString::fromStdString(books[static_cast<std::size_t>(wb)].id.str()));
    }
    if (isMaximized() || isFullScreen()) {
        showNormal();
    }
    if (const QScreen* s = screen(); s != nullptr && !s->availableGeometry().contains(frameGeometry())) {
        resize(1366, 768);
    }
    saveLayout(); // written on Reset Dock Windows (06 section 3.4)
}

void MainWindow::setSaveLayoutOnExit(bool on) {
    impl_->saveLayoutOnExit = on;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (impl_->saveLayoutOnExit) {
        saveLayout();
    }
    QMainWindow::closeEvent(event);
}

// ---- accessors --------------------------------------------------------------------------------------------

qt::ProjectSession& MainWindow::session() {
    return *impl_->session;
}
qt::SettingsStore& MainWindow::settings() {
    return *impl_->settings;
}
qt::DataSelectionModel& MainWindow::selection() {
    return *impl_->selection;
}
qt::WorkspaceViewModel& MainWindow::workspace() {
    return *impl_->workspace;
}
qt::WorksheetViewModel* MainWindow::activeWorksheet() {
    if (impl_ == nullptr) {
        return nullptr;
    }
    const int i = impl_->activeWorksheet;
    return i >= 0 && static_cast<std::size_t>(i) < impl_->worksheets.size()
               ? impl_->worksheets[static_cast<std::size_t>(i)].get()
               : nullptr;
}
qt::MathsService& MainWindow::maths() {
    return *impl_->maths;
}
qt::DistanceService& MainWindow::distance() {
    return *impl_->distance;
}
qt::RenderService& MainWindow::render() {
    return *impl_->render;
}
qt::SourceLoader& MainWindow::loader() {
    return *impl_->loader;
}
qt::CommandRegistry& MainWindow::commands() {
    return *impl_->commands;
}
qt::KeymapBinder& MainWindow::keymap() {
    return *impl_->keymap;
}
qt::MessagesModel& MainWindow::messages() {
    return *impl_->messages;
}
DockHost& MainWindow::docks() {
    return *impl_->docks;
}
WorksheetCanvas& MainWindow::canvas() {
    return *impl_->canvas;
}
CommandActions& MainWindow::actions() {
    return *impl_->actions;
}
KeyEventRouter& MainWindow::router() {
    return *impl_->router;
}
DataWindow& MainWindow::dataWindow() {
    return *impl_->dataWindow;
}
ChannelsWindow& MainWindow::channelsWindow() {
    return *impl_->channelsWindow;
}
ValuesWindow& MainWindow::valuesWindow() {
    return *impl_->valuesWindow;
}
MessagesWindow& MainWindow::messagesWindow() {
    return *impl_->messagesWindow;
}
QTabBar& MainWindow::worksheetTabs() {
    return *impl_->tabs;
}
QComboBox& MainWindow::workbookSelector() {
    return *impl_->workbookCombo;
}
QToolBar& MainWindow::mainToolBar() {
    return *impl_->toolBar;
}
QLabel* MainWindow::statusField(const QString& name) const {
    const auto it = impl_->status.find(name);
    return it == impl_->status.end() ? nullptr : it->second;
}

} // namespace esses::widgets
