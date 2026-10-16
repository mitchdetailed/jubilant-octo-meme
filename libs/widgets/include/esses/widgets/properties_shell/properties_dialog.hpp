// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// PropertiesDialog (06 section 12; 04 section 7.2 item 2; D-027, D-044 first appearance; AC-M3-08): one
// modeless tabbed dialog per component instance, opened by component.properties (F5) or channel.properties
// (CTRL+N, on the Channels tab with the active channel selected). ComponentWidget::openProperties calls
// openFor().
//   1. OK, Cancel, Apply: Apply/OK emit the component's specEditRequested(newSpec, "Edit <type> properties")
//      -> one QUndoCommand each; Cancel restores the spec at open time (one command when Apply was pressed)
//   2. it tracks its component: deleting the component closes it; undo/redo while open refreshes the pages;
//      switching worksheets keeps it open; at most one per component (F5 again raises it)
//   3. hand-built tabs per type (06 section 12 item 3); a field or tab without a body field is omitted
//   4. inline validation; OK/Apply disabled while a page is invalid
//   5. geometry persisted per component type in layout.json "dialogs"
//   6. pages are layouts plus FieldBinder tables (one binder per dialog)
// As built (M3): the tabs and binding tables are src/properties_shell/property_pages.cpp; the body schema is
// read from schemas/components/<type>-<version>.schema.json (project::SchemaRegistry::findSchemasDir) for the
// inline validation; the header (caption show/auto/text, links, legend) is the "Component" group of the
// Display tab, bound by a second, internal binder (binder() binds the body only, so its pointers are the
// body schema's). The canvas announces every spec it applies (WorksheetCanvas::componentSpecApplied), which
// is how undo/redo reaches refresh(). Geometry goes through setGeometryStore (the shell keeps it in
// layout.json dialogs."properties.<type>"). Channel pickers offer the open sources' channel names.
#pragma once

#include <esses/project/component_spec.hpp>

#include <QByteArray>
#include <QDialog>
#include <QStringList>
#include <functional>
#include <memory>

class QTabWidget;

namespace esses::widgets {

class ComponentWidget;
class FieldBinder;

class PropertiesDialog final : public QDialog {
    Q_OBJECT
public:
    // The component's dialog, created on first use and raised on later calls, at tab `tabIndex`.
    static PropertiesDialog* openFor(ComponentWidget& component, int tabIndex = 0);
    // The open dialog of a component, or nullptr.
    static PropertiesDialog* find(const ComponentWidget& component);

    explicit PropertiesDialog(ComponentWidget& component, QWidget* parent = nullptr);
    ~PropertiesDialog() override;

    ComponentWidget& component() const noexcept { return component_; }
    QStringList tabTitles() const;
    FieldBinder& binder();
    // The spec as the pages currently describe it (header plus the stored body).
    project::ComponentSpec editedSpec() const;
    bool isValid() const;
    // Apply: one specEditRequested (no-op when nothing changed).
    void apply();
    // Reloads the pages from the component's spec (after undo/redo).
    void refresh();

    // Added in M3 (as built). The tab widget; the first validation failure ("" when valid); the Channels
    // tab's index; CTRL+N: shows the Channels tab with the record of `channel` selected (false when the pages
    // list no such channel); the header binder (caption, links, legend); whether Apply was pressed since the
    // dialog opened.
    QTabWidget& tabs() const;
    QString validationMessage() const;
    int channelsTabIndex() const;
    bool focusChannel(const QString& channel);
    FieldBinder& headerBinder();
    bool wasApplied() const noexcept;
    // Where dialog geometries are kept (per component type); unset: not kept.
    using GeometryLoad = std::function<QByteArray(const QString& type)>;
    using GeometrySave = std::function<void(const QString& type, const QByteArray& geometry)>;
    static void setGeometryStore(GeometryLoad load, GeometrySave save);
    // The report headings the heading pickers offer: the active profile's reportHeadings (D-203 (y)); unset:
    // the built-in circuit profile's.
    using HeadingSource = std::function<QStringList()>;
    static void setHeadingSource(HeadingSource source);
    // The body schema of a component type ("" JSON null when there is none), cached.
    static JsonDoc bodySchema(const QString& type, int version = 1);

    void accept() override; // OK: apply, then close when valid
    void reject() override; // Cancel: back to the spec at open time when Apply was pressed

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct Impl;
    void build();
    void updateButtons();
    void refreshUntouched();      // the rows the user has not changed follow the component's spec
    void refreshChannelChoices(); // the channel pickers offer the open sources' channels and outputs
    void saveGeometryNow();

    ComponentWidget& component_;
    FieldBinder* binder_ = nullptr; // child
    std::unique_ptr<Impl> d_;
};

} // namespace esses::widgets
