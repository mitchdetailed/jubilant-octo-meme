// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MainWindow's M3 wiring (07 section 6.1; 06 sections 2, 4.1-4.5, 12, 13.5, 13.6, 14.4, 15): the zoom summary
// bar, the Track Map dock, animation, the command palette, the layout and project tools, and the M3 hotkey
// rows (main_window.hpp, "M3 wiring").

#include "main_window_impl.hpp"

#include <esses/analysis/section_detector.hpp>
#include <esses/analysis/track_sections_rule.hpp>
#include <esses/base/uuid.hpp>
#include <esses/model/marker.hpp>
#include <esses/plotcore/component_bodies.hpp>
#include <esses/qtmodel/distance_service.hpp>
#include <esses/qtmodel/maths_service.hpp>
#include <esses/qtmodel/render_service.hpp>
#include <esses/qtmodel/report_model.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/track_map_service.hpp>
#include <esses/qtmodel/undo_commands.hpp>
#include <esses/widgets/components/channel_report.hpp>
#include <esses/widgets/components/time_report.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/docks/messages_window.hpp>
#include <esses/widgets/docks/values_window.hpp>
#include <esses/widgets/properties_shell/component_clipboard.hpp>
#include <esses/widgets/properties_shell/component_commands.hpp>
#include <esses/widgets/properties_shell/properties_dialog.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/dock_host.hpp>
#include <esses/widgets/shell/key_event_router.hpp>
#include <esses/widgets/shell/worksheet_canvas.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenuBar>
#include <QStatusBar>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <set>

namespace esses::widgets {

namespace {

constexpr const char* kTrackMapDock = "trackmap";

QString qPath(const std::filesystem::path& p) {
    return QString::fromStdWString(p.wstring());
}

void post(qt::MessagesModel& messages, qt::Message::Severity severity, qt::Message::Origin origin,
          const QString& source, const QString& text) {
    qt::Message m;
    m.severity = severity;
    m.origin = origin;
    m.source = source;
    m.text = text;
    messages.append(std::move(m));
}

// Component.Export Data / File.Export Data: the bytes go to a file as they come.
class FileSink final : public ExportSink {
public:
    explicit FileSink(const QString& path) : file_(path) {}
    bool write(const QByteArray& bytes) override {
        if (!file_.isOpen() && !file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        return file_.write(bytes) == bytes.size();
    }
    qint64 written() const { return file_.size(); }

private:
    QFile file_;
};

double projectOptionNumber(const qt::ProjectSession& session, const char* group, const char* name,
                           double fallback) {
    const JsonDoc& options = session.projectFile().options;
    if (!options.isObject()) {
        return fallback;
    }
    const QJsonValue v = QJsonDocument::fromJson(QByteArray::fromStdString(options.compact()))
                             .object()
                             .value(QLatin1String(group))
                             .toObject()
                             .value(QLatin1String(name));
    return v.isDouble() ? v.toDouble() : fallback;
}

// A top-level string of project.json `options` (`lapTimeFormat`), `fallback` when absent.
QString projectOptionString(const qt::ProjectSession& session, const char* name, const QString& fallback) {
    const JsonDoc& options = session.projectFile().options;
    if (!options.isObject()) {
        return fallback;
    }
    const QJsonValue v = QJsonDocument::fromJson(QByteArray::fromStdString(options.compact()))
                             .object()
                             .value(QLatin1String(name));
    return v.isString() && !v.toString().isEmpty() ? v.toString() : fallback;
}

} // namespace

// ---- prompts and accessors --------------------------------------------------------------------------------

void MainWindow::setPrompts(Prompts prompts) {
    Prompts& p = impl_->prompts;
    if (prompts.file) {
        p.file = std::move(prompts.file);
    }
    if (prompts.folder) {
        p.folder = std::move(prompts.folder);
    }
    if (prompts.text) {
        p.text = std::move(prompts.text);
    }
    if (prompts.choose) {
        p.choose = std::move(prompts.choose);
    }
}

ZoomSummaryBar& MainWindow::zoomSummaryBar() {
    return *impl_->zoomBar;
}

qt::TrackMapService& MainWindow::trackMaps() {
    return *impl_->trackMaps;
}

CommandPalette& MainWindow::commandPalette() {
    return *impl_->palette;
}

AnimationController& MainWindow::animation() {
    return *impl_->animation;
}

TrackMapWindow& MainWindow::trackMapWindow() {
    return *impl_->trackMap;
}

LayoutEditor& MainWindow::layoutEditor() {
    Impl& d = *impl_;
    if (d.layoutEditor.isNull()) {
        d.layoutEditor = new LayoutEditor(*d.session, this);
        connect(d.layoutEditor.data(), &LayoutEditor::message, this, [this](const QString& text) {
            post(*impl_->messages, qt::Message::Severity::Warning, qt::Message::Origin::Project,
                 tr("Layout Editor"), text);
        });
        // Channel names an imported workbook uses that no open log has (06 section 4.2 item 8).
        d.layoutEditor->setUnresolvedChannels([self = QPointer<MainWindow>(this)](
                                                  const project::WorkbookFile& book) {
            QStringList missing;
            if (self.isNull()) {
                return missing;
            }
            std::set<std::string> seen;
            for (const project::WorksheetDef& sheet : book.worksheets) {
                for (const project::ComponentSpec& c : sheet.components) {
                    for (const plot::ChannelUse& use : plot::componentChannelUses(c)) {
                        if (use.name.empty() || !seen.insert(use.name).second) {
                            continue;
                        }
                        bool found = false;
                        for (const auto& src : self->selection().sources()) {
                            const model::DataLayer* layer = src == nullptr ? nullptr : src->layer("Normal");
                            found = found || (layer != nullptr && layer->find(use.name) != nullptr);
                        }
                        if (!found && !self->selection().sources().empty()) {
                            missing << QString::fromStdString(use.name);
                        }
                    }
                }
            }
            return missing;
        });
    }
    return *d.layoutEditor;
}

CorrectedSpeedDialog& MainWindow::correctedSpeedDialog() {
    Impl& d = *impl_;
    if (d.correctedSpeed.isNull()) {
        d.correctedSpeed = new CorrectedSpeedDialog(*d.session, *d.maths, this);
    }
    return *d.correctedSpeed;
}

TrackEditor& MainWindow::trackEditor() {
    Impl& d = *impl_;
    if (d.trackEditor.isNull()) {
        d.trackEditor = new TrackEditor(*d.session, *d.selection, *d.maths, *d.trackMaps, this);
        d.trackEditor->setObjectName(QStringLiteral("trackEditor"));
    }
    return *d.trackEditor;
}

UnitsListDialog& MainWindow::unitsList() {
    Impl& d = *impl_;
    if (d.unitsList.isNull()) {
        d.unitsList = new UnitsListDialog(d.units, this);
    }
    return *d.unitsList;
}

SelectWorksheetDialog* MainWindow::selectWorksheetDialog() const {
    return impl_->selectWorksheet.data();
}

OpenLogDialog* MainWindow::openLogDialog() const {
    return impl_->openLog.data();
}

bool MainWindow::fullScreenMode() const {
    return impl_->fullScreen;
}

QString MainWindow::lastHint() const {
    return impl_->lastHint;
}

void MainWindow::clearHint() {
    impl_->lastHint.clear();
}

void MainWindow::hint(const QString& text) {
    impl_->lastHint = text;
    statusBar()->showMessage(text, 5000);
}

// ---- widgets ----------------------------------------------------------------------------------------------

void MainWindow::buildM3Widgets(QWidget* central) {
    Impl& d = *impl_;
    // The zoom summary bar sits above the workbook selector and the worksheet tabs (06 section 2.1): it shows
    // the one workspace zoom whatever worksheet is active.
    d.zoomBar = new ZoomSummaryBar(central);
    d.zoomBar->setSelection(d.selection.get());
    d.zoomBar->setScheme(d.session->scheme());
    if (auto* column = qobject_cast<QVBoxLayout*>(central->layout())) {
        column->insertWidget(0, d.zoomBar);
    }
    // The venues' track maps reach the builds (ComponentBuildInputs::trackMap) and the open pipeline (the
    // Track Sections groups); their read problems go to Messages.
    d.render->setTrackMaps(d.trackMaps.get());
    d.loader->setTrackMaps(d.trackMaps.get());
    connect(d.trackMaps.get(), &qt::TrackMapService::logMessage, this, [this](const QString& text) {
        post(*impl_->messages, qt::Message::Severity::Warning, qt::Message::Origin::Project, tr("Track maps"),
             text);
    });
    // The Track Map dock (06 section 9): AutoHide on the right.
    d.trackMap = new TrackMapWindow(this);
    d.trackMap->setObjectName(QStringLiteral("trackMapWindow"));
    d.docks->addDock(QString::fromLatin1(kTrackMapDock), tr("Track Map"), d.trackMap, DockArea::Right,
                     DockState::AutoHide);
    connect(d.trackMap, &TrackMapWindow::editorRequested, this,
            [this] { impl_->commands->invoke(QStringLiteral("tools.track_editor")); });
    // A log of a venue without a map: generate it when the project asks for it (06 section 9; D-203 (u)).
    connect(
        d.loader.get(), &qt::SourceLoader::sourcePublished, this,
        [this](const std::shared_ptr<const model::DataSource>& source) { autoGenerateTrackMap(source, 0); });
    // The editor's cursor span follows the selection and the venue's map as well as the worksheet.
    connect(d.selection.get(), &qt::DataSelectionModel::changed, this, [this] { syncTrackEditorSpan(); });
    connect(d.trackMaps.get(), &qt::TrackMapService::mapChanged, this, [this] { syncTrackEditorSpan(); });
    connect(d.trackMap, &TrackMapWindow::regenerateRequested, this,
            [this] { impl_->commands->invoke(QStringLiteral("tools.track_editor")); });

    // The command palette (06 section 15) and its prefix lists.
    d.palette = new CommandPalette(*d.commands, d.keymap.get(), d.settings.get(), this);
    const QPointer<MainWindow> self(this);
    d.palette->setProvider(QLatin1Char('>'), [self]() {
        QList<PaletteItem> items;
        if (self.isNull()) {
            return items;
        }
        const auto& books = self->session().workbooks();
        for (std::size_t b = 0; b < books.size(); ++b) {
            for (std::size_t s = 0; s < books[b].worksheets.size(); ++s) {
                PaletteItem item;
                item.key = QStringLiteral("worksheet:%1:%2").arg(b).arg(s);
                item.title = QString::fromStdString(books[b].worksheets[s].name);
                item.detail = QString::fromStdString(books[b].name);
                item.invoke = [self, b, s](Qt::KeyboardModifiers) {
                    if (!self.isNull()) {
                        self->selectWorkbook(static_cast<int>(b));
                        self->selectWorksheet(static_cast<int>(s));
                    }
                };
                items.append(item);
            }
        }
        return items;
    });
    d.palette->setProvider(QLatin1Char('@'), [self]() {
        QList<PaletteItem> items;
        if (self.isNull()) {
            return items;
        }
        std::set<QString> names;
        for (const auto& src : self->selection().sources()) {
            if (const model::DataLayer* layer = src == nullptr ? nullptr : src->layer("Normal")) {
                for (const model::Channel& ch : layer->channels) {
                    names.insert(QString::fromStdString(ch.desc.name));
                }
            }
        }
        for (const QString& name : names) {
            PaletteItem item;
            item.key = QStringLiteral("channel:") + name;
            item.title = name;
            item.detail = tr("Add to the focused component (Ctrl+Enter: new panel)");
            item.invoke = [self, name](Qt::KeyboardModifiers mods) {
                if (self.isNull()) {
                    return;
                }
                ComponentWidget* c = self->canvas().focusedComponent();
                if (c == nullptr || !c->acceptsChannelDrop()) {
                    self->hint(tr("The focused component does not take channels"));
                    return;
                }
                c->addChannel(name, mods.testFlag(Qt::ControlModifier));
            };
            items.append(item);
        }
        return items;
    });
    d.palette->setProvider(QLatin1Char('#'), [self]() {
        QList<PaletteItem> items;
        if (self.isNull() || !self->selection().hasMain()) {
            return items;
        }
        qt::DataSelectionModel& sel = self->selection();
        const std::vector<model::RangeRef> ranges = sel.displayedRanges(sel.snapshot().main.src);
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            const model::Range* r = sel.resolve(ranges[i]);
            if (r == nullptr) {
                continue;
            }
            PaletteItem item;
            item.key = QStringLiteral("range:%1").arg(i);
            item.title = QString::fromStdString(r->name);
            item.detail = QStringLiteral("%1 s").arg(static_cast<double>(r->duration()) / 1e6, 0, 'f', 3);
            const model::RangeRef ref = ranges[i];
            item.invoke = [self, ref](Qt::KeyboardModifiers mods) {
                if (self.isNull()) {
                    return;
                }
                if (mods.testFlag(Qt::ControlModifier)) {
                    self->selection().toggleOverlay0(ref);
                } else if (mods.testFlag(Qt::AltModifier)) {
                    self->selection().toggleOverlay(ref);
                } else {
                    self->selection().setMain(ref);
                }
            };
            items.append(item);
        }
        return items;
    });

    // Animation (04 section 8.5 item 7): A and the toolbar toggle follow the controller.
    d.animation = new AnimationController(this);
    d.animation->setRate(projectOptionNumber(*d.session, "animation", "rate", 1.0));
    connect(d.animation, &AnimationController::animatingChanged, this,
            [this] { impl_->actions->refresh(QStringLiteral("data.animate")); });

    // Geometry edits on the canvas (move, resize, splitter, nudge, z order) are workbook edits; consecutive
    // geometry edits of one component merge (06 section 4.3 item 11).
    connect(d.canvas, &WorksheetCanvas::layoutEdited, this,
            [this](const QString& undoText, const QString& mergeKey) {
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
                if (book == books[static_cast<std::size_t>(wb)]) {
                    return;
                }
                i.session->undoStack().push(new qt::WorkbookCommand(
                    *i.session, wb, std::move(book), undoText,
                    mergeKey.isEmpty() ? qt::UndoMergeId::None : qt::UndoMergeId::ComponentGeometry,
                    mergeKey.toStdString()));
            });
    connect(d.canvas, &WorksheetCanvas::selectionChanged, this, [this] { impl_->actions->refresh(); });
    // The component rows are enabled by the focused component's key context (the tool bar shows them at
    // once).
    connect(d.canvas, &WorksheetCanvas::focusedComponentChanged, this, [this] { impl_->actions->refresh(); });

    // Properties dialogs keep their geometry per component type in layout.json "dialogs" (06 section 12 item
    // 5).
    PropertiesDialog::setGeometryStore(
        [self](const QString& type) {
            return self.isNull() || self->impl_ == nullptr
                       ? QByteArray()
                       : self->impl_->layout.dialogGeometry(QStringLiteral("properties.") + type);
        },
        [self](const QString& type, const QByteArray& geometry) {
            if (!self.isNull() && self->impl_ != nullptr) {
                self->impl_->layout.setDialogGeometry(QStringLiteral("properties.") + type, geometry);
            }
        });

    // The heading pickers offer the active profile's report headings (04 section 12 item 6; D-203 (y)).
    PropertiesDialog::setHeadingSource([self]() {
        QStringList out;
        if (!self.isNull() && self->impl_ != nullptr) {
            for (const std::string& h : self->session().profile().reportHeadings) {
                out.push_back(QString::fromStdString(h));
            }
        }
        return out;
    });

    // The default questions: Qt's dialogs.
    d.prompts.file = [this](const QString& title, const QString& filter, bool save) {
        return save ? QFileDialog::getSaveFileName(this, title, QString(), filter)
                    : QFileDialog::getOpenFileName(this, title, QString(), filter);
    };
    d.prompts.folder = [this](const QString& title) {
        return QFileDialog::getExistingDirectory(this, title, qPath(impl_->paths.projectsDir()));
    };
    d.prompts.text = [this](const QString& title, const QString& label,
                            const QString& initial) -> std::optional<QString> {
        bool ok = false;
        const QString text = QInputDialog::getText(this, title, label, QLineEdit::Normal, initial, &ok);
        return ok ? std::optional<QString>(text) : std::nullopt;
    };
    d.prompts.choose = [this](const QString& title, const QStringList& items) -> std::optional<int> {
        bool ok = false;
        const QString item = QInputDialog::getItem(this, title, title, items, 0, false, &ok);
        return ok ? std::optional<int>(static_cast<int>(items.indexOf(item))) : std::nullopt;
    };
}

void MainWindow::buildM3Menus() {
    Impl& d = *impl_;
    // Component > Add > type (06 section 2.2, section 4.3 item 3; D-203 (t)).
    if (QMenu* componentMenu = d.actions->menu(QStringLiteral("Component"))) {
        QMenu* add = componentMenu->addMenu(tr("Add"));
        add->setObjectName(QStringLiteral("menu.Component/Add"));
        for (const auto& [type, title] : WorksheetCanvas::addableTypes()) {
            QAction* a = add->addAction(title);
            a->setObjectName(QStringLiteral("add.") + type);
            connect(a, &QAction::triggered, this,
                    [this, id = type] { impl_->commands->invoke(QStringLiteral("component.add"), {id}); });
        }
        connect(add, &QMenu::aboutToShow, add, [this, add] {
            const bool can = activeWorksheet() != nullptr && !impl_->canvas->layoutLocked();
            for (QAction* a : add->actions()) {
                a->setEnabled(can);
            }
        });
    }
    // Data > Animation Rate (06 section 2.2): one checkable entry per rate of AnimationController::kRates.
    QMenu* dataMenu = d.actions->menu(QStringLiteral("Data"));
    if (dataMenu == nullptr) {
        return;
    }
    d.rateMenu = dataMenu->addMenu(tr("Animation Rate"));
    d.rateMenu->setObjectName(QStringLiteral("menu.Data/Animation Rate"));
    auto* group = new QActionGroup(d.rateMenu);
    for (const double rate : AnimationController::kRates) {
        QAction* a = d.rateMenu->addAction(QStringLiteral("x%1").arg(rate));
        a->setCheckable(true);
        a->setChecked(rate == d.animation->rate());
        group->addAction(a);
        connect(a, &QAction::triggered, this,
                [this, rate] { impl_->commands->invoke(QStringLiteral("data.animation_rate"), {rate}); });
    }
    connect(d.animation, &AnimationController::rateChanged, d.rateMenu, [menu = d.rateMenu](double rate) {
        for (QAction* a : menu->actions()) {
            a->setChecked(a->text() == QStringLiteral("x%1").arg(rate));
        }
    });
}

// ---- following the active worksheet -----------------------------------------------------------------------

void MainWindow::followWorksheetM3(int index) {
    Impl& d = *impl_;
    qt::WorksheetViewModel* ws = index >= 0 && static_cast<std::size_t>(index) < d.worksheets.size()
                                     ? d.worksheets[static_cast<std::size_t>(index)].get()
                                     : nullptr;
    d.zoomBar->setWorksheet(ws);
    d.animation->setWorksheet(ws); // a running animation moves to the new worksheet (06 section 4.5)
    disconnect(d.trackSpanFollow);
    if (ws != nullptr) {
        d.trackSpanFollow =
            connect(ws, &qt::WorksheetViewModel::changed, this, [this] { syncTrackEditorSpan(); });
    }
    syncTrackEditorSpan();
    d.trackMap->setContext(index >= 0 && static_cast<std::size_t>(index) < d.contexts.size()
                               ? d.contexts[static_cast<std::size_t>(index)].get()
                               : nullptr);
    if (ws != nullptr) {
        // Alignment offsets end as one undoable sidecar edit per range (04 section 3.3 item 4, D-015).
        const QPointer<MainWindow> self(this);
        ws->setOffsetCommit([self](const model::RangeRef& range, plot::Domain domain, double offset) {
            if (self.isNull() || self->impl_ == nullptr) {
                return;
            }
            Impl& i = *self->impl_;
            const auto src = i.selection->source(range.src);
            const model::Range* r = i.selection->resolve(range);
            if (src == nullptr || r == nullptr) {
                return;
            }
            Micros us = r->offsetUs;
            double m = r->offsetM;
            if (domain == plot::Domain::Time) {
                us = static_cast<Micros>(std::llround(offset));
            } else {
                m = offset;
            }
            if (us == r->offsetUs && m == r->offsetM) {
                return;
            }
            i.session->undoStack().push(
                new qt::OffsetRangeCommand(*i.loader, *src, range.layer, range.groupPath, *r, us, m));
        });
    }
    syncLayoutLock();
}

void MainWindow::syncLayoutLock() {
    Impl& d = *impl_;
    if (d.canvas != nullptr) {
        d.canvas->setLayoutLocked(d.session->projectFile().layoutLocked);
    }
    if (d.animation != nullptr) {
        const double rate = projectOptionNumber(*d.session, "animation", "rate", d.animation->rate());
        d.animation->setRate(rate);
    }
    // The reports' "project" time format is options.lapTimeFormat (04 section 13 item 6, 02 section 6.2;
    // D-202).
    if (d.canvas != nullptr) {
        const QString format = projectOptionString(*d.session, "lapTimeFormat", QStringLiteral("m:ss.fff"));
        for (ComponentWidget* c : d.canvas->components()) {
            if (auto* report = qobject_cast<ChannelReport*>(c)) {
                report->model().setProjectTimeFormat(format);
            } else if (auto* times = qobject_cast<TimeReport*>(c)) {
                times->model().setProjectTimeFormat(format);
            }
        }
    }
}

// ---- the Track Editor's cursor span and auto-generation (06 sections 9, 13.5; D-203 (u)) ------------------

namespace {

bool beaconAt(const model::DataLayer& layer, Micros t) {
    bool any = false;
    for (const model::Marker& m : layer.markers) {
        if (m.cls == model::MarkerClass::BCN && m.enabled) {
            any = true;
            if (m.t == t) {
                return true;
            }
        }
    }
    return !any; // a layer without beacons: its laps start and end at "the line"
}

bool projectOptionBool(const qt::ProjectSession& session, const char* group, const char* name,
                       bool fallback) {
    const JsonDoc& options = session.projectFile().options;
    if (!options.isObject()) {
        return fallback;
    }
    const QJsonValue v = QJsonDocument::fromJson(QByteArray::fromStdString(options.compact()))
                             .object()
                             .value(QLatin1String(group))
                             .toObject()
                             .value(QLatin1String(name));
    return v.isBool() ? v.toBool() : fallback;
}

// The lap a map is generated from: the fastest trusted flying lap, else the longest trusted lap.
std::optional<model::RangeRef> generationLap(const model::DataSource& src) {
    const model::DataLayer* layer = src.layer("Normal");
    const model::RangeGroup* laps = layer == nullptr ? nullptr : layer->ranges.find("Outings:Laps");
    if (laps == nullptr) {
        return std::nullopt;
    }
    const model::Range* best = nullptr;
    for (const model::Range& r : laps->ranges) {
        if (!r.trusted || !r.enabled) {
            continue;
        }
        const bool flying = r.lapClass == model::LapClass::Flying;
        const bool bestFlying = best != nullptr && best->lapClass == model::LapClass::Flying;
        if (best == nullptr || (flying && !bestFlying) ||
            (flying == bestFlying &&
             (flying ? r.duration() < best->duration() : r.duration() > best->duration()))) {
            best = &r;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    model::RangeRef ref;
    ref.src = src.id;
    ref.layer = std::string(layer->name);
    ref.groupPath = "Outings:Laps";
    ref.range = best->id;
    ref.name = best->name;
    return ref;
}

} // namespace

void MainWindow::syncTrackEditorSpan() {
    Impl& d = *impl_;
    if (d.trackEditor.isNull()) {
        return;
    }
    // The span between the datum and the cursor, as track distances of the Main lap through the origin rule
    // of the Track Sections (D-195 (d)); none without a datum, a venue map or the Main lap's distance index.
    std::optional<std::pair<double, double>> span;
    const qt::WorksheetViewModel* ws = activeWorksheet();
    const qt::DataSelectionModel& sel = *d.selection;
    if (ws != nullptr && sel.hasMain() && ws->state().datumT.has_value()) {
        const model::RangeRef main = sel.snapshot().main;
        const auto src = sel.source(main.src);
        const model::Range* r = sel.resolve(main);
        const model::DataLayer* layer = src == nullptr ? nullptr : src->layer(main.layer);
        const std::shared_ptr<const analysis::TrackMap> map =
            src == nullptr ? nullptr : d.trackMaps->mapFor(*src);
        const std::shared_ptr<const analysis::DistanceIndex> index =
            r == nullptr ? nullptr : d.distance->index(main, Priority::Interactive);
        if (r != nullptr && layer != nullptr && map != nullptr && index != nullptr &&
            index->lengthM() > 0.0 && map->lengthM > 0.0) {
            const double stretch = index->stretch() > 0.0 ? index->stretch() : 1.0;
            const double lapLength = index->lengthM() * stretch;
            const bool starts = beaconAt(*layer, r->start);
            const bool ends = beaconAt(*layer, r->end);
            const auto trackOf = [&](Micros t) {
                const double lapM = index->distAt(r->start + t) * stretch;
                return std::clamp(
                    analysis::trackDistanceOfLapDistance(lapM, lapLength, map->lengthM, starts, ends), 0.0,
                    map->lengthM);
            };
            const double a = trackOf(ws->state().cursorT);
            const double b = trackOf(*ws->state().datumT);
            span = std::make_pair(std::min(a, b), std::max(a, b));
        }
    }
    d.trackEditor->setCursorSpan(span);
}

void MainWindow::autoGenerateTrackMap(const std::shared_ptr<const model::DataSource>& source, int attempt) {
    Impl& d = *impl_;
    if (source == nullptr || !projectOptionBool(*d.session, "trackMaps", "autoGenerateOnOpen", true)) {
        return;
    }
    const std::string venue = qt::TrackMapService::venueOf(*source);
    if (venue.empty() || d.trackMaps->mapFor(*source) != nullptr ||
        (attempt == 0 && d.autoTrackVenues.count(venue) != 0)) {
        return; // no venue to name the map, a map already, or this session tried it
    }
    const std::optional<model::RangeRef> lap = generationLap(*source);
    if (!lap) {
        return;
    }
    d.autoTrackVenues.insert(venue);
    // GPS when the log has it, else Speed+LatG; an input still evaluating is waited for (a few tries).
    qt::TrackGenerationRequest request;
    request.lap = *lap;
    request.method = attempt % 2 == 0 ? analysis::TrackMethod::Gps : analysis::TrackMethod::SpeedLatG;
    const QPointer<MainWindow> self(this);
    const std::weak_ptr<const model::DataSource> weak = source;
    const qt::TrackGenerationStatus status =
        d.trackMaps->generate(request, this, [self, weak, venue](Result<analysis::GeneratedTrack> result) {
            const std::shared_ptr<const model::DataSource> src = weak.lock();
            if (self.isNull() || self->impl_ == nullptr || src == nullptr) {
                return;
            }
            Impl& i = *self->impl_;
            if (!result) {
                post(*i.messages, qt::Message::Severity::Warning, qt::Message::Origin::Project,
                     tr("Track maps"),
                     tr("No track map could be generated for %1: %2")
                         .arg(QString::fromStdString(venue),
                              QString::fromStdString(result.error().toString())));
                return;
            }
            if (i.trackMaps->mapFor(*src) != nullptr) {
                return; // the user made one meanwhile (the Track Editor): theirs stays
            }
            analysis::TrackMap map = std::move(result).value().map;
            static_cast<void>(analysis::SectionDetector::defaults(map));
            // An automatic step, not the user's: written without an undo command, announced in Messages.
            if (Result<void> written =
                    i.trackMaps->setMap(venue, std::make_shared<const analysis::TrackMap>(map));
                !written) {
                post(*i.messages, qt::Message::Severity::Warning, qt::Message::Origin::Project,
                     tr("Track maps"), QString::fromStdString(written.error().toString()));
                return;
            }
            post(*i.messages, qt::Message::Severity::Info, qt::Message::Origin::Project, tr("Track maps"),
                 tr("Generated the track map of %1 (%2 m) from the opened log")
                     .arg(QString::fromStdString(venue))
                     .arg(map.lengthM, 0, 'f', 0));
        });
    if (status.state == qt::TrackGenerationState::Pending && attempt < 40) {
        // An input (Corr Speed, the GPS chain) is being evaluated: try again when a channel is ready.
        auto* once = new QObject(this);
        connect(d.maths.get(), &qt::MathsService::channelReady, once, [this, once, weak, attempt] {
            once->deleteLater();
            if (const std::shared_ptr<const model::DataSource> src = weak.lock()) {
                autoGenerateTrackMap(src, attempt + 2);
            }
        });
        return;
    }
    if (status.state == qt::TrackGenerationState::Failed && request.method == analysis::TrackMethod::Gps &&
        attempt < 40) {
        autoGenerateTrackMap(source, attempt + 1); // no GPS: Speed+LatG
    }
}

// ---- full screen (06 section 4.3 item 7) ------------------------------------------------------------------

void MainWindow::setFullScreenMode(bool on) {
    Impl& d = *impl_;
    if (on == d.fullScreen) {
        return;
    }
    d.fullScreen = on;
    if (on) {
        d.fullScreenPerspective = d.docks->savePerspective();
        for (const QString& id : d.docks->dockIds()) {
            if (d.docks->state(id) != DockState::Closed) {
                d.docks->setState(id, DockState::Closed);
            }
        }
        showFullScreen();
        if (d.fullScreenHint == nullptr) {
            d.fullScreenHint = new QLabel(tr("Full screen: press Esc to leave"), this);
            d.fullScreenHint->setObjectName(QStringLiteral("fullScreenHint"));
            d.fullScreenHint->setStyleSheet(QStringLiteral(
                "background: rgba(0, 0, 0, 160); color: white; padding: 6px; border-radius: 4px;"));
        }
        d.fullScreenHint->adjustSize();
        d.fullScreenHint->move((width() - d.fullScreenHint->width()) / 2, 40);
        d.fullScreenHint->show();
        d.fullScreenHint->raise();
        QTimer::singleShot(3000, d.fullScreenHint, &QWidget::hide);
    } else {
        showNormal();
        if (!d.fullScreenPerspective.isEmpty() && !d.docks->restorePerspective(d.fullScreenPerspective)) {
            d.docks->resetToDefault();
        }
        d.fullScreenPerspective.clear();
        if (d.fullScreenHint != nullptr) {
            d.fullScreenHint->hide();
        }
    }
    d.actions->refresh(QStringLiteral("view.fullscreen"));
}

// ---- the component clipboard (06 section 4.3 item 10, AC-M3-11) -------------------------------------------

bool MainWindow::copyComponents() {
    Impl& d = *impl_;
    const QList<ComponentWidget*> list = d.canvas->selectedOrFocused();
    if (list.isEmpty()) {
        hint(tr("No component to copy"));
        return false;
    }
    ComponentClipboard::Payload payload;
    for (ComponentWidget* c : list) {
        payload.components.push_back(c->spec());
    }
    const qt::WorksheetViewModel* ws = activeWorksheet();
    payload.sourceDomain = ws != nullptr && ws->domain() == plot::Domain::Distance
                               ? QStringLiteral("distance")
                               : QStringLiteral("time");
    QGuiApplication::clipboard()->setMimeData(ComponentClipboard::toMimeData(payload).release());
    return true;
}

bool MainWindow::cutComponents() {
    Impl& d = *impl_;
    if (d.canvas->layoutLocked()) {
        hint(tr("The layout is locked"));
        return false;
    }
    if (!copyComponents()) {
        return false;
    }
    std::vector<Uuid> ids;
    for (ComponentWidget* c : d.canvas->selectedOrFocused()) {
        ids.push_back(c->spec().id);
    }
    d.session->undoStack().push(new qt::RemoveComponentsCommand(
        *d.session, d.session->activeWorkbook(), d.activeWorksheet, std::move(ids), tr("Cut components")));
    return true;
}

bool MainWindow::deleteComponents() {
    Impl& d = *impl_;
    if (d.canvas->layoutLocked()) {
        hint(tr("The layout is locked"));
        return false;
    }
    std::vector<Uuid> ids;
    for (ComponentWidget* c : d.canvas->selectedOrFocused()) {
        ids.push_back(c->spec().id);
    }
    if (ids.empty()) {
        return false;
    }
    // Undoable, no confirmation (06 section 4.3 item 10).
    d.session->undoStack().push(new qt::RemoveComponentsCommand(
        *d.session, d.session->activeWorkbook(), d.activeWorksheet, std::move(ids), tr("Delete components")));
    return true;
}

bool MainWindow::pasteComponents() {
    Impl& d = *impl_;
    if (d.canvas->layoutLocked()) {
        hint(tr("The layout is locked"));
        return false;
    }
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    const std::optional<ComponentClipboard::Payload> payload =
        mime == nullptr ? std::nullopt : ComponentClipboard::fromMimeData(*mime);
    if (!payload) {
        hint(tr("The clipboard holds no components"));
        return false;
    }
    const int wb = d.session->activeWorkbook();
    const auto& books = d.session->workbooks();
    if (wb < 0 || static_cast<std::size_t>(wb) >= books.size() || d.activeWorksheet < 0) {
        return false;
    }
    // One grid cell further per successive paste of the same clipboard into the same worksheet.
    const project::WorksheetDef& sheet =
        books[static_cast<std::size_t>(wb)].worksheets[static_cast<std::size_t>(d.activeWorksheet)];
    const QByteArray text = mime->hasFormat(QString::fromLatin1(ComponentClipboard::kMimeType))
                                ? mime->data(QString::fromLatin1(ComponentClipboard::kMimeType))
                                : mime->text().toUtf8();
    const std::string key = sheet.id.str() + ":" + std::to_string(qHash(text));
    const int count = d.pasteCounts[key]++;
    const QList<project::ComponentSpec> placed =
        ComponentClipboard::placeForPaste(*payload, count, d.canvas->grid());
    std::set<Uuid> before;
    for (const project::ComponentSpec& c : sheet.components) {
        before.insert(c.id);
    }
    d.session->undoStack().push(new qt::AddComponentsCommand(
        *d.session, wb, d.activeWorksheet, std::vector<project::ComponentSpec>(placed.begin(), placed.end()),
        tr("Paste components")));
    std::vector<Uuid> added;
    for (const project::ComponentSpec& c : d.session->workbooks()[static_cast<std::size_t>(wb)]
                                               .worksheets[static_cast<std::size_t>(d.activeWorksheet)]
                                               .components) {
        if (before.count(c.id) == 0) {
            added.push_back(c.id);
        }
    }
    d.canvas->selectIds(added);
    if (!added.empty()) {
        d.canvas->focusComponent(d.canvas->componentById(added.front()));
    }
    return true;
}

// ---- files and projects -----------------------------------------------------------------------------------

void MainWindow::openLogDialogNow() {
    Impl& d = *impl_;
    if (!d.openLog.isNull()) {
        d.openLog->raise();
        d.openLog->activateWindow();
        return;
    }
    const QString folder = d.settings->value(QStringLiteral("openDialog.lastFolder")).toString();
    auto* dialog = new OpenLogDialog(&d.loader->registry(), folder, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    d.openLog = dialog;
    connect(dialog, &QFileDialog::filesSelected, this, [this](const QStringList& files) {
        for (const QString& f : files) {
            openLog(f.toStdWString());
        }
        if (!files.isEmpty()) {
            impl_->settings->setValue(QStringLiteral("openDialog.lastFolder"),
                                      QFileInfo(files.front()).absolutePath());
        }
    });
    dialog->open(); // window-modal; never blocks the caller
}

bool MainWindow::newProjectNow() {
    Impl& d = *impl_;
    const QString folder = d.prompts.folder(tr("New Project: choose an empty folder"));
    if (folder.isEmpty()) {
        return false;
    }
    const std::optional<QString> name =
        d.prompts.text(tr("New Project"), tr("Project name"), QFileInfo(folder).fileName());
    if (!name) {
        return false;
    }
    // The project is written by a session of its own, then opened here like any other project.
    qt::ProjectSession maker(d.paths, nullptr);
    if (const Result<void> r = maker.loadDefaults(); !r) {
        post(*d.messages, qt::Message::Severity::Error, qt::Message::Origin::Project, folder,
             tr("The project could not be created: %1").arg(QString::fromStdString(r.error().toString())));
        return false;
    }
    if (const Result<void> r = maker.newProject(folder.toStdWString(), name->toStdString()); !r) {
        post(*d.messages, qt::Message::Severity::Error, qt::Message::Origin::Project, folder,
             tr("The project could not be created: %1").arg(QString::fromStdString(r.error().toString())));
        return false;
    }
    maker.closeProject();
    return openProject(folder.toStdWString());
}

bool MainWindow::closeProjectNow() {
    Impl& d = *impl_;
    if (!d.session->isOpen() || !confirmLeaveProject()) {
        return false;
    }
    saveLayout();
    d.session->closeProject();
    rebuildWorksheets();
    restoreLayout();
    updateTitle();
    updateEditCommands();
    d.actions->refresh();
    return true;
}

// ---- the commands -----------------------------------------------------------------------------------------

void MainWindow::registerM3Commands() {
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
    const auto focused = [](MainWindow& w) -> ComponentWidget* {
        ComponentWidget* c = w.impl_->canvas->focusedComponent();
        return c != nullptr ? c : w.impl_->lastComponent.data();
    };
    const auto hasFocused = [focused](MainWindow& w) { return focused(w) != nullptr; };
    const auto hasWorksheet = [](MainWindow& w) { return w.activeWorksheet() != nullptr; };
    const auto hasMain = [](MainWindow& w) {
        return w.impl_->selection->hasMain() && w.activeWorksheet() != nullptr;
    };

    // ---- views and windows (06 section 14.4) ----
    add("view.select_worksheet", tr("Select Worksheet..."), "View", C::Global, {QStringLiteral("F7")},
        [](MainWindow& w, const QVariantList&) {
            Impl& i = *w.impl_;
            if (!i.selectWorksheet.isNull()) {
                i.selectWorksheet->raise();
                return;
            }
            auto* dialog =
                new SelectWorksheetDialog(*i.session, i.session->activeWorkbook(), i.activeWorksheet, &w);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            i.selectWorksheet = dialog;
            connect(dialog, &SelectWorksheetDialog::chosen, &w, [&w](int book, int sheet) {
                w.selectWorkbook(book);
                w.selectWorksheet(sheet);
            });
            dialog->open();
            dialog->filterEdit().setFocus(Qt::PopupFocusReason);
        });
    add("layout.editor", tr("Layout Editor..."), "Layout", C::Global, {QStringLiteral("Ctrl+F7")},
        [](MainWindow& w, const QVariantList&) {
            w.showEditor(w.layoutEditor(), LayoutEditor::kGeometryKey);
        });
    add(
        "view.track_map_window", tr("Track Map Window"), "View", C::Global, {},
        [](MainWindow& w, const QVariantList&) {
            w.impl_->docks->toggle(QString::fromLatin1(kTrackMapDock));
        },
        {}, [](MainWindow& w) { return w.impl_->docks->isVisible(QString::fromLatin1(kTrackMapDock)); });
    add("view.command_palette", tr("Command Palette..."), "View", C::Global, {QStringLiteral("Ctrl+Shift+P")},
        [](MainWindow& w, const QVariantList&) {
            const QList<C> chain = w.impl_->router->chainFor(QApplication::focusWidget());
            w.impl_->palette->popup(chain.isEmpty() ? C::Global : chain.front());
        });
    // Help > Keyboard Shortcuts (06 section 2.2 and section 15 item 5): the palette with the query '?' (every
    // command, its keys in a widened column). A menu command without a keymap row, as Component > Add: 06
    // section 14.4 gives it no id (D-203 (ah)).
    add("help.keyboard_shortcuts", tr("Keyboard Shortcuts"), "Help", C::Global, {},
        [](MainWindow& w, const QVariantList&) { w.impl_->palette->popup(C::Global, QStringLiteral("?")); });
    add(
        "view.fullscreen", tr("Full Screen"), "View", C::Global, {},
        [](MainWindow& w, const QVariantList&) { w.setFullScreenMode(!w.fullScreenMode()); }, {},
        [](MainWindow& w) { return w.fullScreenMode(); });

    // ---- file and general ----
    add("file.open_log", tr("Open Log File..."), "File", C::Global, {QStringLiteral("Ctrl+O")},
        [](MainWindow& w, const QVariantList&) { w.openLogDialogNow(); });
    add(
        "file.close_all", tr("Close All Log Files"), "File", C::Global, {QStringLiteral("Ctrl+A")},
        [](MainWindow& w, const QVariantList&) { w.impl_->loader->closeAll(); },
        [](MainWindow& w) { return !w.impl_->selection->sources().empty(); });
    add("file.new_project", tr("New Project..."), "File", C::Global, {},
        [](MainWindow& w, const QVariantList&) { w.newProjectNow(); });
    add("file.open_project", tr("Open Project..."), "File", C::Global, {},
        [](MainWindow& w, const QVariantList&) {
            const QString folder = w.impl_->prompts.folder(tr("Open Project"));
            if (!folder.isEmpty()) {
                w.openProject(folder.toStdWString());
            }
        });
    add(
        "file.close_project", tr("Close Project"), "File", C::Global, {},
        [](MainWindow& w, const QVariantList&) { w.closeProjectNow(); },
        [](MainWindow& w) { return w.impl_->session->isOpen(); });
    add("file.import_data", tr("Import Data..."), "File", C::Global, {},
        [](MainWindow& w, const QVariantList&) {
            const QString file = w.impl_->prompts.file(
                tr("Import Data"), tr("Text logs (*.csv *.txt *.tsv);;All files (*)"), false);
            if (!file.isEmpty()) {
                w.openLog(file.toStdWString()); // text profiles; unknown units go to the Mapping panel (05
                                                // section 8)
            }
        });
    const auto exportFocused = [focused](MainWindow& w) {
        ComponentWidget* c = focused(w);
        if (c == nullptr) {
            w.hint(tr("No component to export"));
            return;
        }
        if (c->handleCommand(QStringLiteral("component.export_data"), {})) {
            return;
        }
        const std::optional<int> range = w.impl_->prompts.choose(
            tr("Export Data"), {tr("Current zoom"), tr("Selected ranges padded by 2 s"), tr("All data")});
        if (!range) {
            return;
        }
        const QString file = w.impl_->prompts.file(tr("Export Data"), tr("CSV files (*.csv)"), true);
        if (file.isEmpty()) {
            return;
        }
        ExportOptions options;
        options.range = *range == 1   ? ExportOptions::Range::SelectedPadded
                        : *range == 2 ? ExportOptions::Range::AllData
                                      : ExportOptions::Range::CurrentZoom;
        // A drawn component's export runs on the pool and the file is written when it completes: the GUI
        // thread never waits for it and the component may go meanwhile (D-203 (x)).
        if (const ComponentWidget::ExportJob job = c->exportJob(options)) {
            Impl& i = *w.impl_;
            auto produced = std::make_shared<std::optional<QByteArray>>();
            const QString key = QStringLiteral("export/") + QString::fromStdString(c->spec().id.str());
            w.hint(tr("Exporting to %1...").arg(QFileInfo(file).fileName()));
            i.render->submitBuild(
                c->context().worksheetId, key, Priority::Interactive,
                [job, produced](const CancelToken& cancel) -> Result<plot::ComponentBuild> {
                    *produced = job(cancel);
                    return plot::ComponentBuild{};
                },
                &w,
                [self = QPointer<MainWindow>(&w), produced, file](Result<plot::ComponentBuild>) {
                    if (self.isNull()) {
                        return;
                    }
                    FileSink sink(file);
                    if (*produced && sink.write(**produced)) {
                        self->hint(tr("Exported to %1").arg(QFileInfo(file).fileName()));
                    } else {
                        self->hint(tr("This component has no data to export"));
                    }
                });
            return;
        }
        FileSink sink(file);
        if (c->exportData(sink, options)) {
            w.hint(tr("Exported to %1").arg(QFileInfo(file).fileName()));
        } else {
            w.hint(tr("This component has no data to export"));
        }
    };
    add(
        "file.export_data", tr("Export Data..."), "File", C::Global, {},
        [exportFocused](MainWindow& w, const QVariantList&) { exportFocused(w); }, hasFocused);

    // ---- tools ----
    add("tools.track_editor", tr("Track Editor..."), "Tools", C::Global, {},
        [](MainWindow& w, const QVariantList&) {
            w.showEditor(w.trackEditor(), "trackEditor");
            w.syncTrackEditorSpan(); // "Add from Cursor Span" follows the worksheet from now on
        });
    add("tools.corrected_speed", tr("Corrected Speed and Distance..."), "Tools", C::Global, {},
        [](MainWindow& w, const QVariantList&) {
            w.showEditor(w.correctedSpeedDialog(), CorrectedSpeedDialog::kGeometryKey);
        });
    add("tools.units_list", tr("Units List..."), "Tools", C::Global, {},
        [](MainWindow& w, const QVariantList&) { w.showEditor(w.unitsList(), "unitsList"); });

    // ---- data ----
    add(
        "data.animate", tr("Animate"), "Data", C::Worksheet, {QStringLiteral("A")},
        [](MainWindow& w, const QVariantList&) { w.impl_->animation->toggle(); }, hasWorksheet,
        [](MainWindow& w) { return w.impl_->animation->animating(); });
    {
        qt::CommandSpec s;
        s.id = QStringLiteral("data.animation_rate");
        s.title = tr("Animation Rate");
        s.context = C::Global;
        s.invoke = [self](const QVariantList& args) {
            if (self.isNull() || self->impl_ == nullptr || args.isEmpty()) {
                return;
            }
            const double rate = args.front().toDouble();
            if (!(rate > 0.0)) {
                return;
            }
            Impl& i = *self->impl_;
            i.animation->setRate(rate);
            // options.animation.rate (02 section 6.2).
            if (projectOptionNumber(*i.session, "animation", "rate", 1.0) != rate) {
                project::ProjectFile next = i.session->projectFile();
                QJsonObject options =
                    QJsonDocument::fromJson(QByteArray::fromStdString(next.options.compact())).object();
                QJsonObject animation = options.value(QStringLiteral("animation")).toObject();
                animation.insert(QStringLiteral("rate"), rate);
                options.insert(QStringLiteral("animation"), animation);
                if (Result<JsonDoc> doc =
                        JsonDoc::parse(QJsonDocument(options).toJson(QJsonDocument::Compact).toStdString())) {
                    next.options = std::move(doc).value();
                    i.session->undoStack().push(
                        new qt::SetProjectOptionsCommand(*i.session, std::move(next), tr("Animation rate")));
                }
            }
        };
        d.commands->add(std::move(s));
    }

    // ---- components general (06 section 14.4) ----
    add(
        "component.properties", tr("Properties..."), "Component", C::Component, {QStringLiteral("F5")},
        [focused](MainWindow& w, const QVariantList&) {
            if (ComponentWidget* c = focused(w)) {
                c->openProperties(0);
            }
        },
        hasFocused);
    add(
        "channel.properties", tr("Channel Properties..."), "Component/Active Channel", C::Component,
        {QStringLiteral("Ctrl+N")},
        [focused](MainWindow& w, const QVariantList&) {
            ComponentWidget* c = focused(w);
            if (c == nullptr) {
                return;
            }
            PropertiesDialog* dialog = PropertiesDialog::openFor(*c, 0);
            if (dialog != nullptr) {
                dialog->tabs().setCurrentIndex(dialog->channelsTabIndex());
                if (const std::optional<QString> active = c->activeChannel()) {
                    dialog->focusChannel(*active);
                }
            }
        },
        hasFocused);
    add(
        "component.focus_next", tr("Next Component"), "Component", C::Worksheet, {QStringLiteral("Tab")},
        [](MainWindow& w, const QVariantList&) { w.impl_->canvas->focusNext(false); },
        [](MainWindow& w) { return !w.impl_->canvas->components().isEmpty(); });
    add(
        "component.focus_prev", tr("Previous Component"), "Component", C::Worksheet,
        {QStringLiteral("Shift+Tab")},
        [](MainWindow& w, const QVariantList&) { w.impl_->canvas->focusNext(true); },
        [](MainWindow& w) { return !w.impl_->canvas->components().isEmpty(); });
    add(
        "component.maximise", tr("Maximise"), "Component", C::Component, {},
        [focused](MainWindow& w, const QVariantList&) { w.impl_->canvas->toggleMaximise(focused(w)); },
        hasFocused, [](MainWindow& w) { return w.impl_->canvas->maximisedComponent() != nullptr; });
    add(
        "component.send_to_background", tr("Send to Background"), "Component", C::Component, {},
        [focused](MainWindow& w, const QVariantList&) {
            if (w.impl_->canvas->layoutLocked()) {
                w.hint(tr("The layout is locked"));
            } else {
                w.impl_->canvas->sendToBackground(focused(w));
            }
        },
        hasFocused);
    add(
        "component.bring_to_front", tr("Bring to Front"), "Component", C::Component, {},
        [focused](MainWindow& w, const QVariantList&) {
            if (w.impl_->canvas->layoutLocked()) {
                w.hint(tr("The layout is locked"));
            } else {
                w.impl_->canvas->bringToFront(focused(w));
            }
        },
        hasFocused);
    add(
        "component.export_data", tr("Export Data..."), "Component", C::Component, {},
        [exportFocused](MainWindow& w, const QVariantList&) { exportFocused(w); }, hasFocused);
    add(
        "component.print_clipboard", tr("Print to Clipboard"), "Component", C::Component, {},
        [focused](MainWindow& w, const QVariantList&) {
            ComponentWidget* c = focused(w);
            if (c == nullptr) {
                return;
            }
            // The current layers at the screen's DPR (04 section 7.2 item 9): never blocks.
            QGuiApplication::clipboard()->setImage(c->renderToImage(c->devicePixelRatioF()));
            w.hint(tr("Copied an image of the component"));
        },
        hasFocused);

    // ---- Component > Add > type (06 section 4.3 item 3; D-203 (t)): menu and canvas only, args [type] ----
    add(
        "component.add", tr("Add Component"), "", C::Worksheet, {},
        [](MainWindow& w, const QVariantList& args) {
            Impl& i = *w.impl_;
            const QString type = args.isEmpty() ? QString() : args.front().toString();
            if (i.canvas->layoutLocked()) {
                w.hint(tr("The layout is locked"));
                return;
            }
            QString title;
            for (const auto& [t, name] : WorksheetCanvas::addableTypes()) {
                if (t == type) {
                    title = name;
                }
            }
            if (title.isEmpty() || !ComponentWidget::isRegistered(type)) {
                w.hint(tr("No component type %1").arg(type));
                return;
            }
            const int wb = i.session->activeWorkbook();
            const auto& books = i.session->workbooks();
            if (wb < 0 || static_cast<std::size_t>(wb) >= books.size() || i.activeWorksheet < 0 ||
                static_cast<std::size_t>(i.activeWorksheet) >=
                    books[static_cast<std::size_t>(wb)].worksheets.size()) {
                return;
            }
            const project::WorksheetDef& sheet =
                books[static_cast<std::size_t>(wb)].worksheets[static_cast<std::size_t>(i.activeWorksheet)];
            project::ComponentSpec spec;
            spec.id = uuidRandom();
            spec.type = type.toStdString();
            spec.version = 1;
            spec.body = JsonDoc::parse("{}").value();
            spec.rect = WorksheetCanvas::placeForAdd(sheet, i.canvas->grid());
            int z = 0;
            std::set<Uuid> before;
            for (const project::ComponentSpec& c : sheet.components) {
                z = std::max(z, c.z + 1);
                before.insert(c.id);
            }
            spec.z = z;
            i.session->undoStack().push(new qt::AddComponentsCommand(*i.session, wb, i.activeWorksheet,
                                                                     {spec}, tr("Add %1").arg(title)));
            for (const project::ComponentSpec& c :
                 i.session->workbooks()[static_cast<std::size_t>(wb)]
                     .worksheets[static_cast<std::size_t>(i.activeWorksheet)]
                     .components) {
                if (before.count(c.id) == 0) {
                    i.canvas->selectIds({c.id});
                    i.canvas->focusComponent(i.canvas->componentById(c.id)); // it receives focus
                }
            }
        },
        hasWorksheet);

    // ---- the edit rows of components (06 section 4.3 item 10) ----
    add(
        "component.cut", tr("Cut Components"), "Edit", C::Worksheet, {QStringLiteral("Ctrl+X")},
        [](MainWindow& w, const QVariantList&) { w.cutComponents(); }, hasFocused);
    add(
        "component.copy", tr("Copy Components"), "Edit", C::Worksheet, {QStringLiteral("Ctrl+C")},
        [](MainWindow& w, const QVariantList&) { w.copyComponents(); }, hasFocused);
    add(
        "component.paste", tr("Paste Components"), "Edit", C::Worksheet, {QStringLiteral("Ctrl+V")},
        [](MainWindow& w, const QVariantList&) { w.pasteComponents(); }, hasWorksheet);
    add(
        "component.delete", tr("Delete Components"), "Edit", C::Worksheet, {QStringLiteral("Delete")},
        [](MainWindow& w, const QVariantList&) { w.deleteComponents(); }, hasFocused);
    add(
        "component.select_all", tr("Select All Components"), "Edit", C::Worksheet, {},
        [](MainWindow& w, const QVariantList&) { w.impl_->canvas->selectAll(); },
        [](MainWindow& w) { return !w.impl_->canvas->components().isEmpty(); });

    // ---- L, K, G, S, ALT+n, Y and the graph display toggles: the component first, else genericComponentEdit
    const auto generic = [focused](const QString& id) {
        return [focused, id](MainWindow& w, const QVariantList& args) {
            ComponentWidget* c = focused(w);
            if (c == nullptr || c->handleCommand(id, args)) {
                return;
            }
            QString undoText;
            const std::optional<project::ComponentSpec> next =
                genericComponentEdit(c->spec(), id, args, &undoText);
            if (!next) {
                w.hint(tr("Not available for this component"));
                return;
            }
            Q_EMIT c->specEditRequested(*next, undoText);
        };
    };
    struct GenericRow {
        const char* id;
        const char* title;
        const char* menu;
        C context;
        QStringList keys;
    };
    const std::vector<GenericRow> genericRows{
        {"component.legend", "Legend", "Component/Display", C::Component, {QStringLiteral("L")}},
        {"component.colour_legend",
         "Colour Legend",
         "Component/Display",
         C::Component,
         {QStringLiteral("K")}},
        {"component.cycle_layout", "Cycle Layout", "Component/Display", C::Component, {QStringLiteral("G")}},
        {"component.cycle_style", "Cycle Style", "Component/Display", C::Component, {QStringLiteral("S")}},
        {"component.colour_band", "Colour Band", "Component/Display", C::Component, {}},
        {"cursor.style", "Cursor Style", "Component/Cursor", C::Graph, {QStringLiteral("Y")}},
        {"graph.status_errors",
         "Status and Errors Strip",
         "Component/Display",
         C::Graph,
         {QStringLiteral("E")}},
        {"graph.device_events", "Device Events", "Component/Display", C::Graph, {QStringLiteral("Shift+E")}},
        {"graph.variance", "Variance Panel", "Component/Display", C::Graph, {QStringLiteral("F3")}},
        {"graph.measurements", "Measurements", "Component/Display", C::Graph, {QStringLiteral("M")}}};
    for (const GenericRow& r : genericRows) {
        add(r.id, tr(r.title), r.menu, r.context, r.keys, generic(QString::fromLatin1(r.id)), hasFocused);
    }

    // ---- commands only the component itself can carry out: offered to it, planned until it does --------
    struct DispatchRow {
        const char* id;
        const char* title;
        const char* menu;
        std::vector<C> contexts; // the focused component's key contexts that can carry it out
        QStringList keys;
    };
    // The mouse rows' programmatic forms (D-201: the graph takes them with arguments, the menu without) are
    // offered the same way; the mouse itself goes to the component directly.
    const std::vector<DispatchRow> dispatchRows{
        {"zoom.v_in", "Vertical Zoom In", "Component/Zoom", {C::Graph}, {QStringLiteral("Alt+Up")}},
        {"zoom.v_out", "Vertical Zoom Out", "Component/Zoom", {C::Graph}, {QStringLiteral("Alt+Down")}},
        {"zoom.v_full", "Vertical Full Scale", "Component/Zoom", {C::Graph}, {QStringLiteral("Alt+F2")}},
        {"pan.up", "Pan Up", "Component/Pan", {C::Graph}, {QStringLiteral("Shift+Up")}},
        {"pan.down", "Pan Down", "Component/Pan", {C::Graph}, {QStringLiteral("Shift+Down")}},
        {"graph.copy_cursor_values",
         "Copy Cursor Values",
         "Component/Display",
         {C::Graph},
         {QStringLiteral("Ctrl+Shift+C")}},
        {"track.zoom_in",
         "Zoom Track In",
         "Component/Track",
         {C::Track},
         {QStringLiteral("+"), QStringLiteral("=")}},
        {"track.zoom_out", "Zoom Track Out", "Component/Track", {C::Track}, {QStringLiteral("-")}},
        {"track.fit", "Fit Track", "Component/Track", {C::Track}, {}},
        {"zoom.rubber_band", "Rubber Band Zoom", "Component/Zoom", {C::Graph}, {}},
        {"zoom.full_axis", "Full Zoom of an Axis", "Component/Zoom", {C::Graph}, {}},
        {"zoom.wheel", "Wheel Zoom", "Component/Zoom", {C::Graph, C::Summary, C::Track}, {}},
        {"zoom.outing_rect", "Outing Zoom Rectangle", "Component/Zoom", {C::Graph}, {}},
        {"pan.drag_axis", "Pan by Axis", "Component/Pan", {C::Graph}, {}},
        {"cursor.place", "Place Cursor", "Component/Cursor", {C::Graph}, {}},
        {"offset.drag", "Drag Offset", "Component/Offset", {C::Graph}, {}}};
    for (const DispatchRow& r : dispatchRows) {
        const QString id = QString::fromLatin1(r.id);
        const std::vector<C> contexts = r.contexts;
        add(
            r.id, tr(r.title), r.menu, r.contexts.front(), r.keys,
            [focused, id](MainWindow& w, const QVariantList& args) {
                ComponentWidget* c = focused(w);
                if (c != nullptr && !c->handleCommand(id, args)) {
                    w.hint(tr("Not available for this component"));
                }
            },
            [focused, contexts, id](MainWindow& w) {
                const ComponentWidget* c = focused(w);
                if (c == nullptr ||
                    std::find(contexts.begin(), contexts.end(), c->commandContext()) == contexts.end()) {
                    return false;
                }
                // The outing graph's zoom rectangle is the outing graph's alone (D-203 (w)).
                return id != QLatin1String("zoom.outing_rect") ||
                       QLatin1String(c->type()) == QLatin1String("outing");
            });
    }

    // A menu-only command a component lists in contextCommands() (the scatter's Copy Equation, 04
    // section 10.1 item 9; no keymap row): offered to the focused component.
    add(
        "component.copy_equation", tr("Copy Equation"), "Component", C::Summary, {},
        [focused](MainWindow& w, const QVariantList& args) {
            ComponentWidget* c = focused(w);
            if (c != nullptr && !c->handleCommand(QStringLiteral("component.copy_equation"), args)) {
                w.hint(tr("Not available for this component"));
            }
        },
        [focused](MainWindow& w) {
            const ComponentWidget* c = focused(w);
            return c != nullptr && c->contextCommands().contains(QStringLiteral("component.copy_equation"));
        });

    // The worksheet rows a summary or track component may carry out its own way (the scatter's cursor steps
    // to the next Main point, 06 section 14.4): offered to the focused component first while the keyboard
    // is in the worksheet, else the worksheet's handler of hotkey batch 1.
    for (const char* id :
         {"zoom.in", "zoom.out", "zoom.default_range", "zoom.datum_main", "pan.left", "pan.right", "pan.back",
          "pan.forward", "pan.centre_cursor", "cursor.left", "cursor.right", "cursor.left_fast",
          "cursor.right_fast", "cursor.home", "cursor.end", "datum.toggle", "datum.place", "datum.swap"}) {
        const qt::CommandSpec* spec = d.commands->find(QString::fromLatin1(id));
        if (spec == nullptr || !spec->invoke) {
            continue;
        }
        qt::CommandSpec wrapped = *spec;
        const QString commandId = wrapped.id;
        wrapped.invoke = [self, commandId, original = spec->invoke](const QVariantList& args) {
            if (!self.isNull() && self->impl_ != nullptr) {
                WorksheetCanvas& canvas = *self->impl_->canvas;
                QWidget* keyboard = QApplication::focusWidget();
                ComponentWidget* c = canvas.focusedComponent();
                if (c != nullptr && keyboard != nullptr &&
                    (keyboard == &canvas || canvas.isAncestorOf(keyboard)) &&
                    c->handleCommand(commandId, args)) {
                    return;
                }
            }
            original(args);
        };
        d.commands->add(std::move(wrapped));
    }

    // ---- alignment offsets (04 section 3.3, 06 section 14.4) ----
    add(
        "offset.axis_toggle", tr("Offset Axis"), "Component/Offset", C::Graph, {QStringLiteral("O")},
        [](MainWindow& w, const QVariantList&) {
            qt::WorksheetViewModel* ws = w.activeWorksheet();
            const bool on = !ws->state().offsetAxis;
            // Turning the axis off zeroes every selected range's offsets as one undo command (04 section 3.3
            // item 2), although the view model commits each range and domain on its own.
            bool offsets = false;
            const plot::SelectionSnapshot& snap = w.selection().snapshot();
            std::vector<model::RangeRef> ranges{snap.main}; // the overlays too while F4 hides them
            if (const std::optional<model::RangeRef>& o0 = snap.overlay0) {
                ranges.push_back(*o0);
            }
            ranges.insert(ranges.end(), snap.overlays.begin(), snap.overlays.end());
            for (const model::RangeRef& range : ranges) {
                const model::Range* r = w.selection().resolve(range);
                offsets = offsets || (r != nullptr && (r->offsetUs != 0 || r->offsetM != 0.0));
            }
            QUndoStack& stack = w.session().undoStack();
            const bool macro = !on && offsets;
            if (macro) {
                stack.beginMacro(MainWindow::tr("Clear alignment offsets"));
            }
            ws->setOffsetAxis(on);
            if (macro) {
                stack.endMacro();
            }
        },
        hasWorksheet,
        [](MainWindow& w) {
            return w.activeWorksheet() != nullptr && w.activeWorksheet()->state().offsetAxis;
        });
    struct OffsetRow {
        const char* id;
        const char* title;
        QString key;
        bool overlay;
        int steps;
    };
    const std::vector<OffsetRow> offsetRows{
        {"offset.main_left", "Offset Main Back", QStringLiteral("Ctrl+Shift+Left"), false, -1},
        {"offset.main_right", "Offset Main Forward", QStringLiteral("Ctrl+Shift+Right"), false, 1},
        {"offset.main_down", "Offset Main Back x10", QStringLiteral("Ctrl+Shift+Down"), false, -10},
        {"offset.main_up", "Offset Main Forward x10", QStringLiteral("Ctrl+Shift+Up"), false, 10},
        {"offset.overlay_left", "Offset Reference Overlay Back", QStringLiteral("Alt+Shift+Left"), true, -1},
        {"offset.overlay_right", "Offset Reference Overlay Forward", QStringLiteral("Alt+Shift+Right"), true,
         1},
        {"offset.overlay_down", "Offset Reference Overlay Back x10", QStringLiteral("Alt+Shift+Down"), true,
         -10},
        {"offset.overlay_up", "Offset Reference Overlay Forward x10", QStringLiteral("Alt+Shift+Up"), true,
         10}};
    for (const OffsetRow& r : offsetRows) {
        const bool overlay = r.overlay;
        const int steps = r.steps;
        add(
            r.id, tr(r.title), "Component/Offset", C::Graph, {r.key},
            [overlay, steps](MainWindow& w, const QVariantList&) {
                qt::WorksheetViewModel* ws = w.activeWorksheet();
                const qt::DataSelectionModel& sel = *w.impl_->selection;
                if (overlay) {
                    if (const std::optional<model::RangeRef> ref = sel.referenceOverlay()) {
                        ws->offsetRange(*ref, steps);
                    } else {
                        w.hint(tr("No reference overlay to offset"));
                    }
                } else {
                    ws->offsetRange(sel.snapshot().main, steps);
                }
            },
            hasMain);
    }

    // ---- annotations and the zoom summary bar ----
    add(
        "cursor.add_annotation", tr("Add Annotation..."), "Component/Cursor", C::Graph, {},
        [focused](MainWindow& w, const QVariantList& args) {
            Impl& i = *w.impl_;
            // The focused graph adds it itself (with its active channel, as its marker band does), so both
            // Add paths store the same record (D-203 (q)).
            if (ComponentWidget* c = focused(w);
                c != nullptr && c->handleCommand(QStringLiteral("cursor.add_annotation"), args)) {
                return;
            }
            const model::RangeRef main = i.selection->snapshot().main;
            const auto src = i.selection->source(main.src);
            const model::Range* r = i.selection->resolve(main);
            if (src == nullptr || r == nullptr) {
                return;
            }
            const std::optional<QString> text =
                i.prompts.text(tr("Add Annotation"), tr("Annotation"), QString());
            if (!text || text->trimmed().isEmpty()) {
                return;
            }
            model::Annotation a;
            a.id = uuidRandom();
            a.tUs = r->start + w.activeWorksheet()->state().cursorT; // Main-source absolute time (rule 6)
            a.text = text->trimmed().toStdString();
            if (ComponentWidget* c = focused(w)) {
                if (const std::optional<QString> active = c->activeChannel()) {
                    a.channel = active->toStdString();
                }
            }
            i.session->undoStack().push(
                new qt::SetAnnotationCommand(*i.loader, *src, main.layer, std::move(a)));
        },
        hasMain);
    add(
        "zoom.extend", tr("Extend Zoom to the Cursor"), "Component/Zoom", C::Global, {},
        [](MainWindow& w, const QVariantList&) {
            // The menu form of SHIFT+click in the zoom summary bar: the span grows to the cursor.
            if (const std::optional<double> x = w.impl_->zoomBar->summary().cursor) {
                w.activeWorksheet()->extendZoom(*x);
            }
        },
        hasMain);
}

} // namespace esses::widgets
