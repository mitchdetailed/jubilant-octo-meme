// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SourceLoader's changed files and Reload (source_loader.hpp, "M4 reload"; 06 section 6 "Changed files", 02
// section 1 behaviour 5, AC-M4-10; D-208, D-217). Root of the M4 reload package in SourceLoader
// (docs/milestones/M4.md section 3): the package's private state lives in SourceLoader::M4Reload (defined
// here, created on first use into m4Reload_).
//   * checkChangedFiles(): formats::checkSourceFile for every open source not yet stale; a Changed or Missing
//     file marks it stale (sticky until it is reloaded or closed) and staleChanged(id, true) follows, once
//     per source, after the pass. checkChangedFilesInBackground() runs the same re-stats as one TaskPool task
//     and applies them when the Dispatcher brings the result back (the shell's activation check, D-217). The
//     Messages row with the Reload action is connectSourceReload's (an Import Log entry carries no action;
//     source_reload.hpp).
//   * The DecodeLog of every open source, read on a GUI timer: new entries go out through importLog; a fault
//   or
//     a change the providers met while decoding marks the source stale (D-217).
//   * reload(id) (D-217): refused while the file cannot be read or the source's edits cannot be written; else
//     the sidecar is flushed and the path opened again through open() (the Mapping panel and the budget gate
//     included) while the stale source stays. The reopen is matched by its path when it publishes
//     (reloadPublished): the stale source is closed then (its selection parked by the close) and
//     sourceReloaded(previous id, source) hands the selection on. A reopen with the stale source's own
//     content id replaces it in published() (reloadSameContent). An open of that path that fails or is
//     cancelled drops the reload and leaves the stale source as it was (pendingReloads() counts the rest).

#include <esses/base/utf8.hpp>
#include <esses/formats/source_file_check.hpp>
#include <esses/qtmodel/dispatcher.hpp>
#include <esses/qtmodel/source_loader.hpp>

#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace esses::qt {

namespace {

// The key a reload is found by: the path the open pipeline reports in its signals, lexically normal.
std::string pathKey(const std::filesystem::path& path) {
    return pathToUtf8(path.lexically_normal());
}

std::string pathKey(const QString& path) {
    return pathKey(pathFromUtf8(path.toStdString()));
}

// Import Log codes that end an open without a publication (source_loader.cpp, loader/source_loader_m2.cpp,
// m4_budget/source_loader_budget.cpp).
constexpr std::array<std::string_view, 4> kEndedCodes{"open.cancelled", "open.mapping_cancelled",
                                                      "open.budget_cancelled", "open.already_open"};

// Decode-time codes that say the file is no longer what was opened (D-217): a fault reads as Missing (the
// bytes could not be read: a share dropped, a truncation), a change as Changed.
constexpr std::string_view kIoFault = "ld.io_fault";
constexpr std::array<std::string_view, 2> kChangedCodes{"ld.source_changed", "text.source_changed"};

constexpr int kDecodeLogPollMs = 1000;

using Found = std::vector<std::pair<model::SourceId, formats::SourceFileState>>;

} // namespace

// The stale sources, the reloads in flight, the background checks and the decode logs read so far (a member
// of SourceLoader: it reaches the loader's private members).
struct SourceLoader::M4Reload {
    struct Pending {
        model::SourceId previous;
        std::string key; // pathKey of the file
    };
    struct Seen {
        std::size_t entries = 0;
        std::uint64_t version = 0;
    };
    std::map<model::SourceId, formats::SourceFileState> stale;
    std::vector<Pending> reloads; // in request order
    bool wired = false;
    int checksRunning = 0;
    std::function<formats::SourceFileCheck(const model::SourceProvenance&)> check;
    std::map<model::SourceId, Seen> decodeSeen;
    QTimer* decodeTimer = nullptr; // a child of the loader

    static M4Reload& of(SourceLoader& loader) {
        if (loader.m4Reload_ == nullptr) {
            loader.m4Reload_ = std::make_shared<M4Reload>();
        }
        return *loader.m4Reload_;
    }

    void drop(const std::string& key) {
        const auto it =
            std::find_if(reloads.begin(), reloads.end(), [&](const Pending& p) { return p.key == key; });
        if (it != reloads.end()) {
            reloads.erase(it);
        }
    }

    // The loader's own signals tell when an open of a reloaded path ends without publishing.
    static void wire(SourceLoader& loader) {
        M4Reload& state = of(loader);
        if (state.wired) {
            return;
        }
        state.wired = true;
        SourceLoader* self = &loader;
        QObject::connect(self, &SourceLoader::openFailed, self,
                         [self](const QString& path, const QString&) { of(*self).drop(pathKey(path)); });
        QObject::connect(self, &SourceLoader::importLog, self,
                         [self](const QString& path, const std::vector<formats::ImportLogEntry>& entries) {
                             for (const formats::ImportLogEntry& e : entries) {
                                 if (std::find(kEndedCodes.begin(), kEndedCodes.end(), e.code) !=
                                     kEndedCodes.end()) {
                                     of(*self).drop(pathKey(path));
                                     return;
                                 }
                             }
                         });
    }

    // Marks the sources `found` names stale (those still open and not stale yet); staleChanged after the
    // pass.
    static std::vector<model::SourceId> markStale(SourceLoader& loader, const Found& found) {
        std::vector<model::SourceId> fresh;
        for (const auto& [id, state] : found) {
            if (state == formats::SourceFileState::Unchanged || loader.source(id) == nullptr ||
                loader.isStale(id)) {
                continue;
            }
            of(loader).stale.insert_or_assign(id, state);
            fresh.push_back(id);
        }
        // After the pass: a listener may close or reload a source, which changes open_.
        for (const model::SourceId& id : fresh) {
            Q_EMIT loader.staleChanged(id, true);
        }
        return fresh;
    }

    // One read of every open source's DecodeLog (D-217).
    static void readDecodeLogs(SourceLoader& loader) {
        M4Reload& state = of(loader);
        Found found;
        // A copy: importLog's listeners may close a source.
        const std::vector<std::shared_ptr<const model::DataSource>> open = loader.open_;
        for (const std::shared_ptr<const model::DataSource>& src : open) {
            const std::shared_ptr<formats::DecodeLog> log = loader.registry_->decodeLog(src->id);
            if (log == nullptr) {
                continue;
            }
            Seen& seen = state.decodeSeen[src->id];
            const std::uint64_t version = log->version();
            if (version == seen.version) {
                continue;
            }
            seen.version = version;
            const std::vector<formats::ImportLogEntry> entries = log->entries();
            if (entries.size() <= seen.entries) {
                continue; // a repeat of an entry already shown (ImportLog coalesces)
            }
            const std::vector<formats::ImportLogEntry> added(
                entries.begin() + static_cast<std::ptrdiff_t>(seen.entries), entries.end());
            seen.entries = entries.size();
            formats::SourceFileState mark = formats::SourceFileState::Unchanged;
            for (const formats::ImportLogEntry& e : added) {
                if (e.code == kIoFault) {
                    mark = formats::SourceFileState::Missing;
                } else if (std::find(kChangedCodes.begin(), kChangedCodes.end(), e.code) !=
                               kChangedCodes.end() &&
                           mark == formats::SourceFileState::Unchanged) {
                    mark = formats::SourceFileState::Changed;
                }
            }
            Q_EMIT loader.importLog(QString::fromStdString(pathToUtf8(src->prov.path)), added);
            if (mark != formats::SourceFileState::Unchanged) {
                found.emplace_back(src->id, mark);
            }
        }
        static_cast<void>(markStale(loader, found));
        if (loader.open_.empty() && state.decodeTimer != nullptr) {
            state.decodeTimer->stop();
        }
    }

    static void watchDecodeLogs(SourceLoader& loader) {
        M4Reload& state = of(loader);
        if (state.decodeTimer == nullptr) {
            state.decodeTimer = new QTimer(&loader);
            state.decodeTimer->setInterval(kDecodeLogPollMs);
            SourceLoader* self = &loader;
            QObject::connect(state.decodeTimer, &QTimer::timeout, self, [self] { readDecodeLogs(*self); });
        }
        if (!state.decodeTimer->isActive()) {
            state.decodeTimer->start();
        }
    }
};

std::vector<model::SourceId> SourceLoader::checkChangedFiles() {
    Found found;
    for (const std::shared_ptr<const model::DataSource>& src : open_) {
        if (src->prov.path.empty() || isStale(src->id)) {
            continue;
        }
        found.emplace_back(src->id, formats::checkSourceFile(src->prov).state);
    }
    return M4Reload::markStale(*this, found);
}

void SourceLoader::checkChangedFilesInBackground() {
    struct Item {
        model::SourceId id;
        model::SourceProvenance prov;
    };
    std::vector<Item> items;
    for (const std::shared_ptr<const model::DataSource>& src : open_) {
        if (!src->prov.path.empty() && !isStale(src->id)) {
            model::SourceProvenance prov;
            prov.path = src->prov.path;
            prov.size = src->prov.size;
            prov.mtimeUnixUs = src->prov.mtimeUnixUs;
            items.push_back(Item{src->id, std::move(prov)});
        }
    }
    M4Reload& state = M4Reload::of(*this);
    if (items.empty() || state.checksRunning > 0) {
        return; // nothing to check, or a pass is in flight (one at a time: an activation burst coalesces)
    }
    ++state.checksRunning;
    Future<Found> future =
        pool_.submit(Priority::Visible, TaskKey{}, CancelToken{},
                     [items = std::move(items), check = state.check](const CancelToken&) {
                         Found found;
                         for (const Item& item : items) {
                             const formats::SourceFileCheck c =
                                 check ? check(item.prov) : formats::checkSourceFile(item.prov);
                             found.emplace_back(item.id, c.state);
                         }
                         return found;
                     });
    Dispatcher* dispatcher = &dispatcher_;
    const QPointer<SourceLoader> guard(this);
    future.onReady([dispatcher, guard](const Result<Found>& result) {
        Found found = result ? result.value() : Found{};
        dispatcher->post(dispatcher, [guard, found = std::move(found)] {
            SourceLoader* self = guard.data();
            if (self == nullptr) {
                return;
            }
            M4Reload& s = M4Reload::of(*self);
            s.checksRunning = std::max(0, s.checksRunning - 1);
            static_cast<void>(M4Reload::markStale(*self, found));
        });
    });
}

int SourceLoader::fileChecksPending() const {
    return m4Reload_ == nullptr ? 0 : m4Reload_->checksRunning;
}

void SourceLoader::setFileCheckForTest(
    std::function<formats::SourceFileCheck(const model::SourceProvenance&)> check) {
    M4Reload::of(*this).check = std::move(check);
}

bool SourceLoader::isStale(const model::SourceId& source) const {
    return m4Reload_ != nullptr && m4Reload_->stale.contains(source);
}

formats::SourceFileState SourceLoader::staleState(const model::SourceId& source) const {
    if (m4Reload_ == nullptr) {
        return formats::SourceFileState::Unchanged;
    }
    const auto it = m4Reload_->stale.find(source);
    return it == m4Reload_->stale.end() ? formats::SourceFileState::Unchanged : it->second;
}

std::vector<model::SourceId> SourceLoader::staleSources() const {
    std::vector<model::SourceId> out;
    for (const std::shared_ptr<const model::DataSource>& src : open_) {
        if (isStale(src->id)) {
            out.push_back(src->id);
        }
    }
    return out;
}

Result<void> SourceLoader::reload(const model::SourceId& source) {
    const std::shared_ptr<const model::DataSource> src = this->source(source);
    if (src == nullptr) {
        return fail(ErrorCode::NotFound, "reload: the source is not open");
    }
    if (src->prov.path.empty()) {
        return fail(ErrorCode::Invalid, "reload: the source has no file to open again");
    }
    const std::filesystem::path path = src->prov.path;
    M4Reload& state = M4Reload::of(*this);
    const std::string key = pathKey(path);
    if (std::any_of(state.reloads.begin(), state.reloads.end(),
                    [&](const M4Reload::Pending& p) { return p.key == key; })) {
        return ok(); // already being opened again
    }
    // Nothing of the open source goes before its file is open again (D-217).
    if (formats::checkSourceFile(src->prov).state == formats::SourceFileState::Missing) {
        return fail(ErrorCode::NotFound,
                    "the file cannot be read now (deleted, moved, or its share "
                    "disconnected); the source stays open, Reload it when the file is back");
    }
    if (!flushSidecar(source, true, true)) {
        return fail(ErrorCode::Io, "its edits could not be written next to the log; the source stays open");
    }
    M4Reload::wire(*this);
    state.reloads.push_back(M4Reload::Pending{source, key});
    open(path);
    return ok();
}

int SourceLoader::pendingReloads() const {
    return m4Reload_ == nullptr ? 0 : static_cast<int>(m4Reload_->reloads.size());
}

bool SourceLoader::reloadSameContent(const std::filesystem::path& path, const model::SourceId& source) {
    if (m4Reload_ == nullptr) {
        return false;
    }
    const std::string key = pathKey(path);
    const auto it =
        std::find_if(m4Reload_->reloads.begin(), m4Reload_->reloads.end(),
                     [&](const M4Reload::Pending& p) { return p.key == key && p.previous == source; });
    if (it == m4Reload_->reloads.end()) {
        return false;
    }
    // The stale source goes first (its selection parked); the reopen's decode log, registered under the same
    // id, stays. reloadPublished then hands the selection on.
    keepDecodeLogOnClose_ = true;
    close(source);
    keepDecodeLogOnClose_ = false;
    return true;
}

void SourceLoader::reloadPublished(const std::shared_ptr<const model::DataSource>& source) {
    if (source == nullptr) {
        return;
    }
    M4Reload::watchDecodeLogs(*this); // D-217: this source's decode-time diagnostics reach Messages
    std::vector<M4Reload::Pending>& reloads = m4Reload_->reloads;
    const std::string key = pathKey(source->prov.path);
    const auto it = std::find_if(reloads.begin(), reloads.end(),
                                 [&](const M4Reload::Pending& p) { return p.key == key; });
    if (it == reloads.end()) {
        return;
    }
    const model::SourceId previous = it->previous;
    reloads.erase(it);
    if (previous != source->id) {
        close(previous); // the file is open again: the stale source goes now (its selection parked)
    }
    Q_EMIT sourceReloaded(previous, source);
}

void SourceLoader::reloadForget(const model::SourceId& id) {
    if (m4Reload_ == nullptr) {
        return;
    }
    m4Reload_->decodeSeen.erase(id); // a reopen with the same id has a new decode log, read from its start
    if (m4Reload_->stale.erase(id) != 0) {
        Q_EMIT staleChanged(id, false);
    }
}

} // namespace esses::qt
