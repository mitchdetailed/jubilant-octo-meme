// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The KEYMAP_TEST entries of the M3 rows the properties/shell work implements (AC-M3-01; 06 section 14.4;
// libs/widgets/README.md "M3 properties and shell"), included by tst_keymap.cpp inside its anonymous
// namespace after Fixture, Case and Before (so it is not a separate translation unit and adds nothing to the
// other tests/ui executables). Each entry arranges what the row needs (a project, a summary component, a
// gauge with ten colour bands, an overlay, a prompt answer), the harness sends the key (or triggers the menu
// action, or runs the entry's mouse gesture) and the entry checks the documented post-condition through the
// models; rows that edit a document undo back to reset()'s mark. The summary rows of zoom.in/zoom.out/
// zoom.default_range/cursor.* share their graph entries; the harness focuses the summary component for them.
// The rows a component carries out itself (06 section 14.4: the shell offers them to the focused component)
// press their key on that component: the graph's vertical zoom and pan and CTRL+SHIFT+C, the track
// component's PLUS/MINUS (a track row focuses a track_position showing the track-1 example map). Their mouse
// rows send the pointer events to the component (through the canvas's application filter, as a real pointer's
// are) at positions taken from the component's own layout: a click places the cursor, the wheel zooms (graph,
// scatter, track), double-click/move/click draws a rubber band, a double-click on the scrollbar shows the
// whole axis, a drag of the x axis pans, a drag of the offset axis offsets Main, a drag of the outing's zoom
// rectangle end sets the zoom.
#pragma once

// ---- pointer events for the mouse rows ----
void sendMouse(QWidget* w, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent e(type, pos, w->mapToGlobal(pos), button, buttons, mods);
    QApplication::sendEvent(w, &e);
}
void clickAt(QWidget* w, QPointF pos) {
    sendMouse(w, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}
void doubleClickAt(QWidget* w, QPointF pos) {
    clickAt(w, pos);
    sendMouse(w, QEvent::MouseButtonDblClick, pos, Qt::LeftButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}
void dragMouse(QWidget* w, QPointF from, QPointF to) {
    sendMouse(w, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseMove, 0.5 * (from + to), Qt::NoButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}
void wheelAt(QWidget* w, QPointF pos, int deltaY) {
    QWheelEvent e(pos, w->mapToGlobal(pos), QPoint(), QPoint(0, deltaY), Qt::NoButton, Qt::NoModifier,
                  Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &e);
}
TimeDistanceGraph* tdGraph(const Fixture& f) {
    return qobject_cast<TimeDistanceGraph*>(f.graph());
}
// The graph's Static layer shows the current inputs: its layout and hit regions are current.
bool graphQuiet(const TimeDistanceGraph& g) {
    return QTest::qWaitFor([&g] { return g.staticUpToDate(); }, 20000);
}
// The widget-logical point at fractions (fracX, fracY) of a device-px rectangle.
QPointF atRect(const plot::RectPx& r, double fracX, double fracY, double dpr) {
    return {(static_cast<double>(r.x) + fracX * static_cast<double>(r.w)) / dpr,
            (static_cast<double>(r.y) + fracY * static_cast<double>(r.h)) / dpr};
}
// A point at x fraction `fracX` of `r` whose hit is `kind` (scanning down `r`); nullopt when there is none.
std::optional<QPointF> hitPoint(const ComponentWidget& c, const plot::RectPx& r, double fracX, double dpr,
                                plot::HitKind kind) {
    for (int i = 1; i < 20; ++i) {
        const QPointF p = atRect(r, fracX, i / 20.0, dpr);
        if (c.hitAt(p).kind == kind) {
            return p;
        }
    }
    return std::nullopt;
}
TrackPosition* focusedTrack(const Fixture& f) {
    return qobject_cast<TrackPosition*>(f.w->canvas().focusedComponent());
}

void addM3Cases(std::map<QString, Case>& table) {
    using F = Fixture;
    const auto undoStep = [](F& f, const Before& b) {
        QCOMPARE(f.w->session().undoStack().index(), b.undoIndex + 1);
    };
    const auto activate = [](F& f) {
        f.w->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(f.w.get()));
    };
    // The focused component's body member after the key, against the one before.
    const auto bodyChanged = [](F& f, const Before& b, const char* pointer) {
        ComponentWidget* c = f.w->canvas().focusedComponent();
        QVERIFY(c != nullptr && b.focusedSpec.has_value());
        const QJsonValue before = jsonptr::get(jsonptr::fromDoc(b.focusedSpec->body), QLatin1String(pointer));
        const QJsonValue after = jsonptr::get(jsonptr::fromDoc(c->spec().body), QLatin1String(pointer));
        QVERIFY2(after != before, pointer);
        QCOMPARE(f.w->session().undoStack().index(), b.undoIndex + 1);
    };
    const auto add = [&table](const QString& id, Case c) { table.emplace(id, std::move(c)); };

    // ---- views and windows ----
    add(KEYMAP_TEST("view.select_worksheet"),
        {{},
         {},
         [](F& f, const Before&, int) {
             SelectWorksheetDialog* d = f.w->selectWorksheetDialog();
             QVERIFY(d != nullptr && d->isVisible());
             QCOMPARE(d->current(), (std::pair<int, int>{0, 0})); // the active entry preselected
         },
         [activate](F& f) {
             if (SelectWorksheetDialog* d = f.w->selectWorksheetDialog()) {
                 d->reject();
             }
             QTRY_VERIFY(f.w->selectWorksheetDialog() == nullptr);
             activate(f);
         }});
    add(KEYMAP_TEST("layout.editor"),
        {{},
         {},
         [](F& f, const Before&, int) { QVERIFY(f.w->layoutEditor().isVisible()); },
         [](F& f) { closeEditor(f, f.w->layoutEditor()); }});
    add(KEYMAP_TEST("view.track_map_window"), // menu only
        {{},
         {},
         [](F& f, const Before& b, int) {
             QCOMPARE(f.w->docks().isVisible(QStringLiteral("trackmap")), !b.trackMapVisible);
         },
         {}});
    add(KEYMAP_TEST("view.command_palette"),
        {{},
         {},
         [](F& f, const Before& b, int) {
             QVERIFY(f.w->commandPalette().isVisible());
             QCOMPARE(f.w->commandPalette().capturedContext(),
                      b.inDock ? qt::CommandContext::DataWindow : qt::CommandContext::Graph);
         },
         [activate](F& f) {
             f.w->commandPalette().hide();
             activate(f);
         }});
    add(KEYMAP_TEST("view.fullscreen"), // menu only
        {{},
         {},
         [](F& f, const Before&, int) {
             QVERIFY(f.w->fullScreenMode());
             QVERIFY(!f.w->docks().isVisible(kData));
         },
         [activate](F& f) {
             f.w->setFullScreenMode(false);
             activate(f);
         }});

    // ---- file and general ----
    add(KEYMAP_TEST("file.open_log"), {{},
                                       {},
                                       [](F& f, const Before&, int) {
                                           OpenLogDialog* d = f.w->openLogDialog();
                                           QVERIFY(d != nullptr && d->isVisible());
                                       },
                                       [activate](F& f) {
                                           if (OpenLogDialog* d = f.w->openLogDialog()) {
                                               d->reject();
                                           }
                                           QTRY_VERIFY(f.w->openLogDialog() == nullptr);
                                           activate(f);
                                       }});
    add(KEYMAP_TEST("file.close_all"),
        {{},
         {},
         [](F& f, const Before&, int) { QTRY_VERIFY(f.sel().sources().empty()); },
         [](F& f) { QVERIFY(f.reopenLogs()); }});
    add(KEYMAP_TEST("file.save_project"), {[](F& f, int) {
                                               QVERIFY(f.openProject());
                                               pushProbeEdit(f);
                                               QVERIFY(f.w->session().isDirty());
                                           },
                                           {},
                                           [](F& f, const Before&, int) {
                                               QVERIFY(!f.w->session().isDirty());
                                               QCOMPARE(probeSecOnDisk(f.projectDir), kProbe);
                                           },
                                           [](F& f) { QVERIFY(f.closeProject()); }});
    add(KEYMAP_TEST("file.new_project"), // menu only: the folder and the name through the prompts
        {[](F& f, int) { f.nextFolder = f.freshFolder(); },
         {},
         [](F& f, const Before&, int) {
             QVERIFY(f.w->session().isOpen());
             QCOMPARE(f.w->session().projectFile().name, std::string("Keymap"));
             QVERIFY(std::filesystem::exists(f.nextFolder / "project.json"));
         },
         [](F& f) { QVERIFY(f.closeProject()); }});
    add(KEYMAP_TEST("file.open_project"), // menu only
        {[](F& f, int) { f.nextFolder = f.projectDir; },
         {},
         [](F& f, const Before&, int) {
             QVERIFY(f.w->session().isOpen());
             QVERIFY(f.w->session().folder() == f.projectDir);
         },
         [](F& f) { QVERIFY(f.closeProject()); }});
    add(KEYMAP_TEST("file.close_project"), // menu only
        {[](F& f, int) { QVERIFY(f.openProject()); },
         {},
         [](F& f, const Before&, int) { QVERIFY(!f.w->session().isOpen()); },
         {}});
    add(KEYMAP_TEST("file.import_data"), // menu only: the file through the prompt
        {[](F& f, int) { f.nextFile = f.ldC; },
         {},
         [](F& f, const Before&, int) {
             QTRY_COMPARE_WITH_TIMEOUT(f.sel().sources().size(), std::size_t{3}, 30000);
         },
         [](F& f) {
             f.w->loader().close(f.sourceOf(f.ldC));
             QTRY_COMPARE(f.sel().sources().size(), std::size_t{2});
         }});
    const auto exportRow =
        Case{[](F& f, int) {
                 f.w->clearHint();
                 f.nextFile = f.exportFile();
             },
             {},
             [](F& f, const Before&, int) {
                 // The graph's CSV, or the hint that it has none to give.
                 QVERIFY(std::filesystem::exists(f.nextFile) || !f.w->lastHint().isEmpty());
             },
             {}};
    add(KEYMAP_TEST("file.export_data"), exportRow); // menu only
    add(KEYMAP_TEST("component.export_data"), exportRow);

    // ---- edit rows of components ----
    add(KEYMAP_TEST("component.cut"),
        {[](F&, int) { QGuiApplication::clipboard()->clear(); },
         {},
         [undoStep](F& f, const Before& b, int) {
             QVERIFY(f.graph() == nullptr);
             undoStep(f, b);
             const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
             QVERIFY(mime != nullptr && ComponentClipboard::fromMimeData(*mime).has_value());
         },
         [](F& f) {
             undoToMark(f);
             QVERIFY(f.graph() != nullptr);
         }});
    add(KEYMAP_TEST("component.copy"),
        {[](F&, int) { QGuiApplication::clipboard()->clear(); },
         {},
         [](F& f, const Before& b, int) {
             const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
             QVERIFY(mime != nullptr);
             const std::optional<ComponentClipboard::Payload> p = ComponentClipboard::fromMimeData(*mime);
             QVERIFY(p.has_value() && p->components.size() == 1);
             QVERIFY(b.graphSpec && p->components.front() == *b.graphSpec);
             QCOMPARE(f.w->session().undoStack().index(), b.undoIndex); // not an edit
         },
         {}});
    add(KEYMAP_TEST("component.paste"),
        {[](F& f, int) {
             f.w->canvas().focusComponent(f.graph());
             QVERIFY(f.w->copyComponents());
             f.componentsBefore = static_cast<int>(f.w->canvas().components().size());
         },
         {},
         [undoStep](F& f, const Before& b, int) {
             QCOMPARE(static_cast<int>(f.w->canvas().components().size()), f.componentsBefore + 1);
             undoStep(f, b);
         },
         [](F& f) { undoToMark(f); }});
    add(KEYMAP_TEST("component.delete"), {{},
                                          {},
                                          [undoStep](F& f, const Before& b, int) {
                                              QVERIFY(f.graph() == nullptr);
                                              undoStep(f, b);
                                          },
                                          [](F& f) {
                                              undoToMark(f);
                                              QVERIFY(f.graph() != nullptr);
                                          }});
    add(KEYMAP_TEST("component.select_all"), // menu only
        {{},
         {},
         [](F& f, const Before&, int) {
             QCOMPARE(f.w->canvas().selection().size(), f.w->canvas().components().size());
         },
         [](F& f) { f.w->canvas().clearSelection(); }});

    // ---- data and tools ----
    add(KEYMAP_TEST("data.animate"), {{},
                                      {},
                                      [](F& f, const Before&, int) {
                                          QVERIFY(f.w->animation().animating());
                                          QVERIFY(f.ws().animating());
                                      },
                                      [](F& f) { f.w->animation().setAnimating(false); }});
    add(KEYMAP_TEST("tools.track_editor"), // menu only
        {{},
         {},
         [](F& f, const Before&, int) { QVERIFY(f.w->trackEditor().isVisible()); },
         [](F& f) { closeEditor(f, f.w->trackEditor()); }});
    add(KEYMAP_TEST("tools.corrected_speed"), // menu only
        {{},
         {},
         [](F& f, const Before&, int) { QVERIFY(f.w->correctedSpeedDialog().isVisible()); },
         [](F& f) { closeEditor(f, f.w->correctedSpeedDialog()); }});
    add(KEYMAP_TEST("tools.units_list"), // menu only
        {{},
         {},
         [](F& f, const Before&, int) {
             QVERIFY(f.w->unitsList().isVisible());
             QVERIFY(f.w->unitsList().visibleRows() > 0);
         },
         [](F& f) { closeEditor(f, f.w->unitsList()); }});

    // ---- components general ----
    const auto closeProperties = [activate](F& f) {
        if (ComponentWidget* g = f.graph()) {
            if (PropertiesDialog* d = PropertiesDialog::find(*g)) {
                d->reject();
            }
            QTRY_VERIFY(PropertiesDialog::find(*g) == nullptr);
        }
        activate(f);
    };
    add(KEYMAP_TEST("component.properties"), {{},
                                              {},
                                              [](F& f, const Before&, int) {
                                                  PropertiesDialog* d = PropertiesDialog::find(*f.graph());
                                                  QVERIFY(d != nullptr && d->isVisible());
                                                  QVERIFY(!d->isModal());
                                              },
                                              closeProperties});
    add(KEYMAP_TEST("channel.properties"), {{},
                                            {},
                                            [](F& f, const Before&, int) {
                                                PropertiesDialog* d = PropertiesDialog::find(*f.graph());
                                                QVERIFY(d != nullptr && d->isVisible());
                                                QCOMPARE(d->tabs().currentIndex(), d->channelsTabIndex());
                                            },
                                            closeProperties});
    const auto neighbour = [](F& f, int step) {
        const QList<ComponentWidget*> order = f.w->canvas().focusOrder();
        const qsizetype i = order.indexOf(f.graph());
        return order[((i + step) % order.size() + order.size()) % order.size()];
    };
    add(KEYMAP_TEST("component.focus_next"), {{},
                                              {},
                                              [neighbour](F& f, const Before&, int) {
                                                  QVERIFY(f.w->canvas().components().size() >= 2);
                                                  QCOMPARE(f.w->canvas().focusedComponent(), neighbour(f, 1));
                                              },
                                              {}});
    add(KEYMAP_TEST("component.focus_prev"), {{},
                                              {},
                                              [neighbour](F& f, const Before&, int) {
                                                  QCOMPARE(f.w->canvas().focusedComponent(),
                                                           neighbour(f, -1));
                                              },
                                              {}});
    add(KEYMAP_TEST("component.legend"),
        {{},
         {},
         [undoStep](F& f, const Before& b, int) {
             QVERIFY(b.focusedSpec.has_value());
             QCOMPARE(f.w->canvas().focusedComponent()->spec().legend.channels,
                      !b.focusedSpec->legend.channels);
             undoStep(f, b);
         },
         [](F& f) { undoToMark(f); }});
    add(KEYMAP_TEST("component.colour_legend"), // a scatter: its colour-channel legend
        {{},
         [](F& f, int) { f.focusExtra("scatter"); },
         [undoStep](F& f, const Before& b, int) {
             QVERIFY(b.focusedSpec.has_value() && b.focusedSpec->type == "scatter");
             QCOMPARE(f.w->canvas().focusedComponent()->spec().legend.colour, !b.focusedSpec->legend.colour);
             undoStep(f, b);
         },
         [](F& f) { undoToMark(f); }});
    add(KEYMAP_TEST("component.cycle_layout"),
        {{},
         {},
         [bodyChanged](F& f, const Before& b, int) { bodyChanged(f, b, "/display/overlapMode"); },
         [](F& f) { undoToMark(f); }});
    add(KEYMAP_TEST("component.cycle_style"),
        {{},
         {},
         [bodyChanged](F& f, const Before& b, int) { bodyChanged(f, b, "/display/traceStyle"); },
         [](F& f) { undoToMark(f); }});
    add(KEYMAP_TEST("component.colour_band"), // ALT+n: band n of a gauge with ten colour bands
        {{},
         [](F& f, int) { f.focusExtra("gauge_numeric"); },
         [](F& f, const Before& b, int n) {
             const QString pointer = QStringLiteral("/colourBands/%1/visible").arg(n);
             QVERIFY(b.focusedSpec.has_value());
             const bool before = jsonptr::get(jsonptr::fromDoc(b.focusedSpec->body), pointer).toBool(true);
             const bool after =
                 jsonptr::get(jsonptr::fromDoc(f.w->canvas().focusedComponent()->spec().body), pointer)
                     .toBool(true);
             QCOMPARE(after, !before);
             QCOMPARE(f.w->session().undoStack().index(), b.undoIndex + 1);
         },
         [](F& f) { undoToMark(f); }});
    add(KEYMAP_TEST("component.maximise"), // menu only
        {{},
         {},
         [](F& f, const Before&, int) { QCOMPARE(f.w->canvas().maximisedComponent(), f.graph()); },
         [](F& f) { f.w->canvas().restoreMaximised(); }});
    add(KEYMAP_TEST("component.send_to_background"), // menu only
        {[](F& f, int) { f.w->canvas().bringToFront(f.graph()); },
         {},
         [](F& f, const Before&, int) {
             QCOMPARE(f.graph()->spec().z, 0);
             QCOMPARE(f.w->canvas().components().front(), f.graph());
         },
         [](F& f) { undoToMark(f); }});
    add(KEYMAP_TEST("component.bring_to_front"), // menu only
        {[](F& f, int) { f.w->canvas().sendToBackground(f.graph()); },
         {},
         [](F& f, const Before&, int) {
             QCOMPARE(f.graph()->spec().z, static_cast<int>(f.w->canvas().components().size()) - 1);
             QCOMPARE(f.w->canvas().components().back(), f.graph());
         },
         [](F& f) { undoToMark(f); }});
    add(KEYMAP_TEST("component.print_clipboard"), // menu only
        {[](F&, int) { QGuiApplication::clipboard()->clear(); },
         {},
         [](F& f, const Before&, int) {
             const QImage img = QGuiApplication::clipboard()->image();
             QVERIFY(!img.isNull());
             const qreal dpr = f.graph()->devicePixelRatioF();
             QCOMPARE(img.width(), static_cast<int>(std::lround(f.graph()->width() * dpr)));
         },
         {}});

    // ---- graph display (the time_distance body; the graph draws them) ----
    add(KEYMAP_TEST("graph.status_errors"),
        {{},
         {},
         [bodyChanged](F& f, const Before& b, int) { bodyChanged(f, b, "/display/showStatusStrip"); },
         [](F& f) { undoToMark(f); }});
    add(KEYMAP_TEST("graph.device_events"),
        {{},
         {},
         [bodyChanged](F& f, const Before& b, int) { bodyChanged(f, b, "/display/showDeviceEvents"); },
         [](F& f) { undoToMark(f); }});
    add(KEYMAP_TEST("graph.variance"),
        {{},
         {},
         [bodyChanged](F& f, const Before& b, int) { bodyChanged(f, b, "/display/variance/show"); },
         [](F& f) { undoToMark(f); }});
    add(KEYMAP_TEST("graph.measurements"),
        {{},
         {},
         [bodyChanged](F& f, const Before& b, int) { bodyChanged(f, b, "/display/key/stats"); },
         [](F& f) { undoToMark(f); }});
    // Y: the graph's or the scatter's cursor style.
    add(KEYMAP_TEST("cursor.style"), {{},
                                      {},
                                      [bodyChanged](F& f, const Before& b, int) {
                                          bodyChanged(f, b,
                                                      b.focusedSpec && b.focusedSpec->type == "scatter"
                                                          ? "/cursorStyle"
                                                          : "/display/cursorStyle");
                                      },
                                      [](F& f) { undoToMark(f); }});
    add(KEYMAP_TEST("cursor.add_annotation"), // menu only: the text through the prompt
        {{},
         {},
         [undoStep](F& f, const Before& b, int) {
             undoStep(f, b);
             const auto src = f.sel().source(f.sel().snapshot().main.src);
             QVERIFY(src != nullptr);
             const auto& notes = src->sidecar.layer(f.sel().snapshot().main.layer).annotations;
             QVERIFY(std::any_of(notes.begin(), notes.end(),
                                 [](const model::Annotation& a) { return a.text == "Keymap note"; }));
         },
         [](F& f) { undoToMark(f); }});

    // ---- the zoom summary bar: SHIFT+click extends the span (a mouse row) ----
    {
        Case c;
        c.gesture = [](F& f, int) {
            ZoomSummaryBar& bar = f.w->zoomSummaryBar();
            const double x = f.ws().state().zoom.b + 20.0 * kSec;
            QTest::mouseClick(&bar, Qt::LeftButton, Qt::ShiftModifier,
                              QPoint(static_cast<int>(std::lround(bar.pxOf(x))), bar.height() / 2));
        };
        c.verify = [](F& f, const Before& b, int) {
            QCOMPARE(f.ws().state().zoom.a, b.view.zoom.a);
            const double pixel = f.w->zoomSummaryBar().summary().extent.length() /
                                 std::max(1, f.w->zoomSummaryBar().width() - 8);
            QVERIFY(std::abs(f.ws().state().zoom.b - (b.view.zoom.b + 20.0 * kSec)) <= pixel);
        };
        add(KEYMAP_TEST("zoom.extend"), c);
    }

    // ---- alignment offsets (04 section 3.3; the commit to the sidecar is tst_shell_m3's) ----
    add(KEYMAP_TEST("offset.axis_toggle"),
        {{},
         {},
         [](F& f, const Before& b, int) { QCOMPARE(f.ws().state().offsetAxis, !b.view.offsetAxis); },
         [](F& f) { f.ws().setOffsetAxis(false); }});
    struct OffsetRow {
        const char* id;
        bool overlay;
        int steps;
    };
    for (const OffsetRow& r :
         {OffsetRow{"offset.main_left", false, -1}, OffsetRow{"offset.main_right", false, 1},
          OffsetRow{"offset.main_down", false, -10}, OffsetRow{"offset.main_up", false, 10},
          OffsetRow{"offset.overlay_left", true, -1}, OffsetRow{"offset.overlay_right", true, 1},
          OffsetRow{"offset.overlay_down", true, -10}, OffsetRow{"offset.overlay_up", true, 10}}) {
        const bool overlay = r.overlay;
        const int steps = r.steps;
        add(QString::fromLatin1(r.id),
            {[overlay](F& f, int) {
                 if (overlay) {
                     f.sel().toggleOverlay0(f.lapsA[1]); // the reference overlay
                 }
             },
             {},
             [overlay, steps](F& f, const Before&, int) {
                 const model::RangeRef range = overlay ? f.lapsA[1] : f.sel().snapshot().main;
                 const auto it = f.ws().pendingOffsets().find(range);
                 QVERIFY(it != f.ws().pendingOffsets().end());
                 QCOMPARE(it->second, static_cast<double>(steps * f.ws().activeSamplePeriod()));
                 QVERIFY(f.ws().state().offsetAxis);
             },
             [](F& f) { f.ws().setOffsetAxis(false); }});
    }
    // ---- the graph's own rows (time_distance; 04 section 8.3 item 6, section 8.6) ----
    add(KEYMAP_TEST("graph.copy_cursor_values"),
        {[](F&, int) { QApplication::clipboard()->setText(QString()); },
         {},
         [](F& f, const Before&, int) {
             TimeDistanceGraph* g = tdGraph(f);
             QVERIFY(g != nullptr);
             // A line per configured channel, its name first (the values may still be arriving).
             const QStringList got =
                 QApplication::clipboard()->text().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
             const QStringList want = g->cursorValuesText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
             QVERIFY(!got.isEmpty());
             QCOMPARE(got.size(), want.size());
             for (qsizetype i = 0; i < got.size(); ++i) {
                 QCOMPARE(got[i].section(QLatin1Char('\t'), 0, 0), want[i].section(QLatin1Char('\t'), 0, 0));
             }
             QVERIFY(g->channelNames().contains(got.front().section(QLatin1Char('\t'), 0, 0)));
         },
         {}});
    // ALT+UP/DOWN/F2 and SHIFT+UP/DOWN: the shown fraction of the active group's scale (local view state).
    const auto vertical = [](plot::TdGroupZoom start, plot::TdGroupZoom expected) {
        Case c;
        c.arrange = [start](F& f, int) {
            TimeDistanceGraph* g = tdGraph(f);
            QVERIFY(g != nullptr);
            g->setVerticalZoom(std::max(0, g->layout().activeGroup), start);
        };
        c.verify = [expected](F& f, const Before&, int) {
            TimeDistanceGraph* g = tdGraph(f);
            QVERIFY(g != nullptr);
            const plot::TdGroupZoom z = g->verticalZoomOf(std::max(0, g->layout().activeGroup));
            QVERIFY2(std::abs(z.lo - expected.lo) < 1e-9 && std::abs(z.hi - expected.hi) < 1e-9,
                     qPrintable(QStringLiteral("[%1, %2]").arg(z.lo).arg(z.hi)));
        };
        c.after = [](F& f) {
            if (TimeDistanceGraph* g = tdGraph(f)) {
                for (std::size_t i = 0; i < g->layout().groups.size(); ++i) {
                    g->setVerticalZoom(static_cast<int>(i), plot::TdGroupZoom{});
                }
            }
        };
        return c;
    };
    add(KEYMAP_TEST("zoom.v_in"), vertical({0.0, 1.0}, {0.25, 0.75}));
    add(KEYMAP_TEST("zoom.v_out"), vertical({0.25, 0.75}, {0.0, 1.0}));
    add(KEYMAP_TEST("zoom.v_full"), vertical({0.25, 0.5}, {0.0, 1.0}));
    add(KEYMAP_TEST("pan.up"), vertical({0.25, 0.75}, {0.30, 0.80}));
    add(KEYMAP_TEST("pan.down"), vertical({0.25, 0.75}, {0.20, 0.70}));

    // ---- the graph's mouse rows ----
    {
        Case c; // a click in a panel: the cursor there
        c.gesture = [](F& f, int) {
            TimeDistanceGraph* g = tdGraph(f);
            QVERIFY(g != nullptr && graphQuiet(*g));
            const plot::TdLayout& L = g->layout();
            const double dpr = static_cast<double>(L.dpr);
            const double target = static_cast<double>(f.ws().state().cursorT) + 5.0 * kSec;
            const double fracX = (g->logicalXOf(target) * dpr - static_cast<double>(L.traceArea.x)) /
                                 static_cast<double>(L.traceArea.w);
            std::optional<QPointF> p = hitPoint(*g, L.traceArea, fracX, dpr, plot::HitKind::Panel);
            if (!p) {
                p = hitPoint(*g, L.traceArea, fracX, dpr, plot::HitKind::Trace);
            }
            QVERIFY(p.has_value());
            f.expect = {target, f.ws().state().zoom.length() / std::max(1, L.traceWidthPx) * dpr};
            clickAt(g, *p);
        };
        c.verify = [](F& f, const Before&, int) {
            QVERIFY(std::abs(static_cast<double>(f.ws().state().cursorT) - f.expect[0]) <= 3.0 * f.expect[1]);
        };
        add(KEYMAP_TEST("cursor.place"), c);
    }
    {
        Case c; // double-click, move, click in a panel: the band becomes the zoom
        c.gesture = [](F& f, int) {
            TimeDistanceGraph* g = tdGraph(f);
            QVERIFY(g != nullptr && graphQuiet(*g));
            const plot::TdLayout& L = g->layout();
            const double dpr = static_cast<double>(L.dpr);
            const std::optional<QPointF> from = hitPoint(*g, L.traceArea, 0.3, dpr, plot::HitKind::Panel);
            QVERIFY(from.has_value());
            const QPointF to(atRect(L.traceArea, 0.6, 0.5, dpr).x(), from->y());
            const std::optional<double> a = g->domainXAt(from->x());
            const std::optional<double> b = g->domainXAt(to.x());
            QVERIFY(a.has_value() && b.has_value());
            f.expect = {*a, *b, f.ws().state().zoom.length() / std::max(1, L.traceWidthPx) * dpr};
            doubleClickAt(g, *from);
            QVERIFY(g->rubberBandActive());
            sendMouse(g, QEvent::MouseMove, to, Qt::NoButton, Qt::NoButton);
            clickAt(g, to);
            QVERIFY(!g->rubberBandActive());
        };
        c.verify = [](F& f, const Before&, int) {
            const plot::XSpan z = f.ws().state().zoom;
            QVERIFY(std::abs(z.a - f.expect[0]) <= f.expect[2]);
            QVERIFY(std::abs(z.b - f.expect[1]) <= f.expect[2]);
        };
        add(KEYMAP_TEST("zoom.rubber_band"), c);
    }
    {
        Case c; // a double-click on the horizontal scrollbar: the whole outing
        c.arrange = [](F& f, int) { zoomInsideMain(f); };
        c.gesture = [](F& f, int) {
            TimeDistanceGraph* g = tdGraph(f);
            QVERIFY(g != nullptr && graphQuiet(*g));
            const plot::TdLayout& L = g->layout();
            QVERIFY(L.scrollbar.h > 0.0F);
            const QPointF p = atRect(L.scrollbar, 0.5, 0.5, static_cast<double>(L.dpr));
            QCOMPARE(g->hitAt(p).kind, plot::HitKind::ScrollBar);
            const plot::XSpan extent = g->staticInfo().scrollExtent;
            f.expect = {extent.a, extent.b};
            doubleClickAt(g, p);
        };
        c.verify = [](F& f, const Before& b, int) {
            const plot::XSpan z = f.ws().state().zoom;
            QVERIFY(z.length() > b.view.zoom.length());
            QVERIFY(std::abs(z.a - f.expect[0]) < 1.0 && std::abs(z.b - f.expect[1]) < 1.0);
        };
        add(KEYMAP_TEST("zoom.full_axis"), c);
    }
    {
        Case c; // a drag of the x axis: the data follows the pointer
        c.gesture = [](F& f, int) {
            TimeDistanceGraph* g = tdGraph(f);
            QVERIFY(g != nullptr && graphQuiet(*g));
            const plot::TdLayout& L = g->layout();
            const double dpr = static_cast<double>(L.dpr);
            QVERIFY(L.xAxis.h > 0.0F);
            const QPointF from = atRect(L.xAxis, 0.5, 0.5, dpr);
            const plot::HitResult hit = g->hitAt(from);
            QVERIFY(hit.kind == plot::HitKind::Axis && hit.group < 0);
            f.expect = {-0.1 * f.ws().state().zoom.length()};
            dragMouse(g, from, from + QPointF(0.1 * L.traceWidthPx / dpr, 0.0));
        };
        c.verify = [](F& f, const Before& b, int) {
            const plot::XSpan z = f.ws().state().zoom;
            QVERIFY(std::abs(z.length() - b.view.zoom.length()) < 1.0);
            QVERIFY(std::abs((z.a - b.view.zoom.a) - f.expect[0]) < 0.01 * b.view.zoom.length());
        };
        add(KEYMAP_TEST("pan.drag_axis"), c);
    }
    {
        Case c; // a drag of Main's offset sub-axis: one offset edit at the release (AC-M3-09)
        c.arrange = [](F& f, int) { f.ws().setOffsetAxis(true); };
        c.gesture = [](F& f, int) {
            TimeDistanceGraph* g = tdGraph(f);
            QVERIFY(g != nullptr && graphQuiet(*g));
            const plot::TdLayout& L = g->layout();
            const double dpr = static_cast<double>(L.dpr);
            QVERIFY(L.offsetMain.h > 0.0F);
            const QPointF from = atRect(L.offsetMain, 0.4, 0.5, dpr);
            QCOMPARE(g->hitAt(from).kind, plot::HitKind::OffsetAxis);
            f.expect = {0.1 * f.ws().state().zoom.length()};
            dragMouse(g, from, from + QPointF(0.1 * static_cast<double>(L.traceArea.w) / dpr, 0.0));
        };
        c.verify = [undoStep](F& f, const Before& b, int) {
            const auto offset = [&f] {
                const model::Range* r = f.sel().resolve(f.sel().snapshot().main);
                return r == nullptr ? 0.0 : static_cast<double>(r->offsetUs);
            };
            QTRY_VERIFY_WITH_TIMEOUT(std::abs(offset() - f.expect[0]) < 0.02 * f.expect[0], 5000);
            undoStep(f, b);
        };
        c.after = [](F& f) {
            undoToMark(f);
            f.ws().setOffsetAxis(false);
        };
        add(KEYMAP_TEST("offset.drag"), c);
    }
    {
        Case c; // a drag of the outing graph's zoom rectangle end: that end of the worksheet zoom
        c.focus = [](F& f, int) { f.focusExtra("outing"); };
        c.gesture = [](F& f, int) {
            auto* o = qobject_cast<OutingGraph*>(f.w->canvas().focusedComponent());
            QVERIFY(o != nullptr);
            QTRY_VERIFY_WITH_TIMEOUT(o->staticUpToDate() && o->zoomRectLogical().width() > 0.0, 20000);
            const QRectF r = o->zoomRectLogical();
            const QPointF from(r.right(), r.center().y());
            QCOMPARE(o->hitAt(from).handleKind, plot::HandleKind::ZoomEnd);
            const QPointF to(std::min(r.right() + 30.0, o->width() - 10.0), from.y());
            QVERIFY(to.x() > from.x() + 5.0);
            const std::optional<double> x = o->worksheetXAt(to.x());
            QVERIFY(x.has_value());
            f.expect = {*x};
            dragMouse(o, from, to);
        };
        c.verify = [](F& f, const Before& b, int) {
            const plot::XSpan z = f.ws().state().zoom;
            QCOMPARE(z.a, b.view.zoom.a);
            QVERIFY(z.b > b.view.zoom.b);
            QVERIFY(std::abs(z.b - f.expect[0]) < 1.0);
        };
        add(KEYMAP_TEST("zoom.outing_rect"), c);
    }
    {
        Case c; // the wheel: the graph's zoom about the pointer, the scatter's own axes, the track view
        c.gesture = [](F& f, int) {
            ComponentWidget* focused = f.w->canvas().focusedComponent();
            QVERIFY(focused != nullptr);
            f.expect.clear();
            if (auto* g = qobject_cast<TimeDistanceGraph*>(focused)) {
                QVERIFY(graphQuiet(*g));
                const std::optional<QPointF> p =
                    hitPoint(*g, g->layout().traceArea, 0.5, static_cast<double>(g->layout().dpr),
                             plot::HitKind::Panel);
                QVERIFY(p.has_value());
                f.expect = {0.8 * f.ws().state().zoom.length()}; // x0.8 per notch (04 section 8.6)
                wheelAt(g, *p, 120);
            } else if (auto* s = qobject_cast<ScatterPlot*>(focused)) {
                // The whole width of the canvas (an undoable layout edit, undone after): room for a plot
                // area.
                QVERIFY(f.w->canvas().setRects({{s->spec().id, project::Rect{0.0, 0.3, 1.0, 1.0}}},
                                               QStringLiteral("Enlarge for a keymap row")));
                s->setAxisZoom(std::nullopt);
                QTRY_VERIFY_WITH_TIMEOUT(s->staticUpToDate() && !s->axes().empty() &&
                                             s->axes().front().plot.w > 0.0F &&
                                             s->axes().front().xMax > s->axes().front().xMin,
                                         20000);
                const plot::ScatterAxes a = s->axes().front();
                const QPointF p = atRect(a.plot, 0.5, 0.5, s->devicePixelRatioF());
                QTRY_VERIFY_WITH_TIMEOUT(s->hitAt(p).kind == plot::HitKind::Panel, 5000);
                f.expect = {ScatterPlot::kWheelZoomIn * (a.xMax - a.xMin)};
                wheelAt(s, p, 120);
            } else if (auto* tp = qobject_cast<TrackPosition*>(focused)) {
                f.expect = {(tp->body().scaleMode == "manual" ? tp->body().zoom : 1.0) *
                            TrackPosition::kZoomStep};
                wheelAt(tp, QPointF(0.5 * tp->width(), 0.5 * tp->height() + 9.0), 120);
            } else {
                QFAIL(focused->type());
            }
        };
        c.verify = [](F& f, const Before&, int) {
            ComponentWidget* focused = f.w->canvas().focusedComponent();
            QCOMPARE(f.expect.size(), std::size_t{1});
            if (qobject_cast<TimeDistanceGraph*>(focused) != nullptr) {
                QVERIFY(std::abs(f.ws().state().zoom.length() - f.expect[0]) < 1e-6 * f.expect[0]);
            } else if (auto* s = qobject_cast<ScatterPlot*>(focused)) {
                QVERIFY(s->axisZoom().has_value());
                const double width = s->axisZoom()->xMax - s->axisZoom()->xMin;
                QVERIFY(std::abs(width - f.expect[0]) < 1e-6 * std::abs(f.expect[0]));
            } else if (auto* tp = qobject_cast<TrackPosition*>(focused)) {
                QCOMPARE(tp->body().scaleMode, std::string("manual"));
                QVERIFY(std::abs(tp->body().zoom - f.expect[0]) < 1e-9);
            }
        };
        c.after = [](F& f) {
            ComponentWidget* focused = f.w->canvas().focusedComponent();
            if (auto* s = qobject_cast<ScatterPlot*>(focused)) {
                s->setAxisZoom(std::nullopt);
            } else if (auto* tp = qobject_cast<TrackPosition*>(focused)) {
                tp->commitView();
            }
            undoToMark(f);
        };
        add(KEYMAP_TEST("zoom.wheel"), c);
    }

    // ---- the track component's view (04 section 14 item 4): PLUS/"="/MINUS about the centre, fit ----
    const auto trackZoom = [](double expected) {
        Case c;
        c.verify = [expected](F& f, const Before&, int) {
            TrackPosition* tp = focusedTrack(f);
            QVERIFY(tp != nullptr);
            QCOMPARE(tp->body().scaleMode, std::string("manual"));
            QVERIFY(std::abs(tp->body().zoom - expected) < 1e-9);
        };
        c.after = [](F& f) {
            if (TrackPosition* tp = focusedTrack(f)) {
                tp->commitView(); // the gesture's one edit now, then undone
            }
            undoToMark(f);
        };
        return c;
    };
    add(KEYMAP_TEST("track.zoom_in"), trackZoom(TrackPosition::kZoomStep));
    add(KEYMAP_TEST("track.zoom_out"), trackZoom(1.0 / TrackPosition::kZoomStep));
    add(KEYMAP_TEST("track.fit"), // menu only (the double-click's command)
        {{},
         [](F& f, int) {
             f.focusTrack();
             TrackPosition* tp = focusedTrack(f);
             QVERIFY(tp != nullptr);
             tp->zoomBy(2.0);
             tp->commitView();
         },
         [](F& f, const Before& b, int) {
             TrackPosition* tp = focusedTrack(f);
             QVERIFY(tp != nullptr);
             QCOMPARE(tp->body().scaleMode, std::string("fit"));
             QCOMPARE(tp->body().zoom, 1.0);
             QCOMPARE(f.w->session().undoStack().index(), b.undoIndex + 1); // one edit
         },
         [](F& f) { undoToMark(f); }});

    // The KEYMAP_TEST spellings of the offset rows (tools/check_keymap_coverage.py reads them here):
    // KEYMAP_TEST("offset.main_left") KEYMAP_TEST("offset.main_right") KEYMAP_TEST("offset.main_down")
    // KEYMAP_TEST("offset.main_up") KEYMAP_TEST("offset.overlay_left") KEYMAP_TEST("offset.overlay_right")
    // KEYMAP_TEST("offset.overlay_down") KEYMAP_TEST("offset.overlay_up")
}
