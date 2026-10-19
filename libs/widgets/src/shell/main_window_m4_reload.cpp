// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ReloadShell (reload_shell.hpp): the M4 reload package's shell wiring (docs/milestones/M4.md section 3;
// D-208): the activation check, the Data window's stale query and badge refresh, the Messages row and the
// selection restore (qt::connectSourceReload), and the command file.reload_source. The window's state is
// reached with `auto& d = impl();`.

#include "main_window_impl.hpp"

#include <esses/base/utf8.hpp>
#include <esses/qtmodel/command_registry.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/data_window_model.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/qtmodel/source_reload.hpp>
#include <esses/widgets/docks/data_window.hpp>
#include <esses/widgets/shell/command_actions.hpp>
#include <esses/widgets/shell/reload_shell.hpp>

#include <QGuiApplication>
#include <QPointer>
#include <QVariantList>

namespace esses::widgets {

ReloadShell::ReloadShell(MainWindow& window) : ShellExtension(window, kKey) {}

ReloadShell::~ReloadShell() = default;

std::vector<model::SourceId> ReloadShell::checkNow() {
    auto& d = impl();
    return d.loader == nullptr ? std::vector<model::SourceId>{} : d.loader->checkChangedFiles();
}

bool ReloadShell::anyStale() const {
    auto& d = impl();
    return d.loader != nullptr && !d.loader->staleSources().empty();
}

bool ReloadShell::reloadSource(const std::optional<model::SourceId>& source) {
    auto& d = impl();
    if (d.loader == nullptr) {
        return false;
    }
    std::optional<model::SourceId> target = source;
    if (!target && d.dataWindow != nullptr) {
        if (const std::optional<model::SourceId> highlighted = d.dataWindow->highlightedSource();
            highlighted && d.loader->isStale(*highlighted)) {
            target = highlighted;
        }
    }
    if (!target) {
        for (const std::shared_ptr<const model::DataSource>& s : d.selection->sources()) {
            if (d.loader->isStale(s->id)) {
                target = s->id;
                break;
            }
        }
    }
    if (!target || !d.loader->isStale(*target)) {
        return false;
    }
    const std::shared_ptr<const model::DataSource> src = d.loader->source(*target);
    const QString name =
        src == nullptr ? QString() : QString::fromStdString(pathToUtf8(src->prov.path.filename()));
    if (const Result<void> r = d.loader->reload(*target); !r) {
        // The source stays open and stale (D-217): say why.
        hint(tr("%1 cannot be reloaded now: %2").arg(name, QString::fromStdString(r.error().message)));
        return false;
    }
    hint(tr("Reloading %1").arg(name));
    return true;
}

void ReloadShell::buildWidgets(QWidget& /*central*/) {
    auto& d = impl();
    if (d.dataWindow == nullptr) {
        return;
    }
    // The badge (06 section 6 item 1): the Data window asks the loader.
    d.dataWindow->model().setStaleQuery(
        [loader = QPointer<qt::SourceLoader>(d.loader.get())](const model::SourceId& id) {
            return !loader.isNull() && loader->isStale(id);
        });
}

void ReloadShell::registerCommands() {
    auto& d = impl();
    const QPointer<ReloadShell> self(this);
    qt::CommandSpec s;
    s.id = QString::fromLatin1(kCommand);
    s.title = tr("Reload File");
    s.menuPath = QStringLiteral("File");
    s.context = qt::CommandContext::DataWindow;
    s.enabled = [self] { return !self.isNull() && self->anyStale(); };
    s.invoke = [self](const QVariantList& args) {
        if (self.isNull()) {
            return;
        }
        std::optional<model::SourceId> source;
        if (!args.isEmpty()) {
            source = qt::parseSourceIdArgument(args.front().toString());
            if (!source) {
                return; // not a source id: nothing to reload
            }
        }
        static_cast<void>(self->reloadSource(source));
    };
    d.commands->add(std::move(s));
}

void ReloadShell::windowReady() {
    auto& d = impl();
    if (d.loader == nullptr || d.selection == nullptr) {
        return;
    }
    qt::connectSourceReload(*d.loader, *d.selection, d.messages.get());
    // The badge and the command follow the stale state.
    connections_.push_back(connect(d.loader.get(), &qt::SourceLoader::staleChanged, this,
                                   [this](const model::SourceId& id, bool /*stale*/) {
                                       auto& w = impl();
                                       if (w.dataWindow != nullptr) {
                                           w.dataWindow->model().refreshStale(id);
                                       }
                                       if (w.actions != nullptr) {
                                           w.actions->refresh(QString::fromLatin1(kCommand));
                                       }
                                   }));
    // The check on application focus (06 section 6 "Changed files").
    if (qGuiApp != nullptr) {
        connections_.push_back(connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
                                       [this](Qt::ApplicationState state) {
                                           if (state == Qt::ApplicationActive) {
                                               // The re-stats on the pool: a share that dropped never
                                               // freezes the window (D-217).
                                               if (auto& w = impl(); w.loader != nullptr) {
                                                   w.loader->checkChangedFilesInBackground();
                                               }
                                           }
                                       }));
    }
}

void ReloadShell::teardown() {
    for (const QMetaObject::Connection& c : connections_) {
        disconnect(c);
    }
    connections_.clear();
}

} // namespace esses::widgets
