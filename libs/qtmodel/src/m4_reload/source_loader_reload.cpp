// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SourceLoader's changed files and Reload (source_loader.hpp, "M4 reload"; 06 section 6 "Changed files", 02
// section 1 behaviour 5, AC-M4-10; D-208). Root of the M4 reload package in SourceLoader
// (docs/milestones/M4.md section 3): the package's private state lives in SourceLoader::M4Reload (defined
// here, created on first use into m4Reload_), so the package never edits source_loader.hpp or
// source_loader.cpp.
//   * checkChangedFiles(): formats::checkSourceFile for every open source not yet stale; a Changed or Missing
//     file marks it stale (sticky until it is reloaded or closed) and staleChanged(id, true) follows, once
//     per source, after the pass. The Messages row with the Reload action is connectSourceReload's (an Import
//     Log entry carries no action; source_reload.hpp).
//   * reload(id): close(id) (its sidecar written first; staleChanged(id, false)), then open(path) through the
//     pipeline (the Mapping panel and the budget gate included). The reopen is matched by its path when it
//     publishes (reloadPublished: sourceReloaded(previous id, source)); an open of that path that fails, is
//     cancelled or finds the file already open drops the reload (pendingReloads() counts the rest).

#include <esses/base/utf8.hpp>
#include <esses/formats/source_file_check.hpp>
#include <esses/qtmodel/source_loader.hpp>

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

} // namespace

// The stale sources, the reloads in flight and the connections that drop a reload whose open ends unpublished
// (a member of SourceLoader: it reaches the loader's private members).
struct SourceLoader::M4Reload {
    struct Pending {
        model::SourceId previous;
        std::string key; // pathKey of the file
    };
    std::map<model::SourceId, formats::SourceFileState> stale;
    std::vector<Pending> reloads; // in request order
    bool wired = false;

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
};

std::vector<model::SourceId> SourceLoader::checkChangedFiles() {
    std::vector<model::SourceId> fresh;
    for (const std::shared_ptr<const model::DataSource>& src : open_) {
        if (src->prov.path.empty() || isStale(src->id)) {
            continue;
        }
        const formats::SourceFileCheck check = formats::checkSourceFile(src->prov);
        if (check.state == formats::SourceFileState::Unchanged) {
            continue;
        }
        M4Reload::of(*this).stale.insert_or_assign(src->id, check.state);
        fresh.push_back(src->id);
    }
    // After the pass: a listener may close or reload a source, which changes open_.
    for (const model::SourceId& id : fresh) {
        Q_EMIT staleChanged(id, true);
    }
    return fresh;
}

bool SourceLoader::isStale(const model::SourceId& source) const {
    return m4Reload_ != nullptr && m4Reload_->stale.contains(source);
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
    M4Reload::wire(*this);
    close(source); // the sidecar is written first (D-190); the stale mark goes with the source
    M4Reload::of(*this).reloads.push_back(M4Reload::Pending{source, pathKey(path)});
    open(path);
    return ok();
}

int SourceLoader::pendingReloads() const {
    return m4Reload_ == nullptr ? 0 : static_cast<int>(m4Reload_->reloads.size());
}

void SourceLoader::reloadPublished(const std::shared_ptr<const model::DataSource>& source) {
    if (m4Reload_ == nullptr || source == nullptr) {
        return;
    }
    std::vector<M4Reload::Pending>& reloads = m4Reload_->reloads;
    const std::string key = pathKey(source->prov.path);
    const auto it = std::find_if(reloads.begin(), reloads.end(),
                                 [&](const M4Reload::Pending& p) { return p.key == key; });
    if (it == reloads.end()) {
        return;
    }
    const model::SourceId previous = it->previous;
    reloads.erase(it);
    Q_EMIT sourceReloaded(previous, source);
}

void SourceLoader::reloadForget(const model::SourceId& id) {
    if (m4Reload_ != nullptr && m4Reload_->stale.erase(id) != 0) {
        Q_EMIT staleChanged(id, false);
    }
}

} // namespace esses::qt
