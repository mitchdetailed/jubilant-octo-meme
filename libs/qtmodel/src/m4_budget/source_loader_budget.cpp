// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// SourceLoader's open-time budget gate (source_loader.hpp, "M4 budget"; 07 section 7.1, AC-M4-02; 01 section
// 7.2 "a warning at open, with continue or cancel"; D-206 (c)-(d)). Root of the M4 budget package in
// SourceLoader (docs/milestones/M4.md section 3); its state lives in SourceLoader::M4Budget, created on first
// use into m4Budget_, so the package edits neither source_loader.hpp nor source_loader.cpp.
//
// published() asks budgetHolds() before anything else is done with an opened source: with a gate set, an
// estimate (formats::estimateResident: every logged column in the chunk layout) above the cache's budget
// keeps the source here instead, so nothing decodes a chunk of it, registers its maths or starts its summary
// pass, and budgetExceeded(pendingId, check) announces it. Continue hands the same snapshot back to
// published(), which this time lets it through (M4Budget::continuing); Cancel and cancelOpens() drop it with
// an Import Log row (open.budget_cancelled). A held open is not counted by pendingOpens().

#include <esses/base/utf8.hpp>
#include <esses/formats/import_log.hpp>
#include <esses/formats/registry.hpp>
#include <esses/formats/resident_estimate.hpp>
#include <esses/qtmodel/source_loader.hpp>
#include <esses/store/chunk_cache.hpp>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace esses::qt {

namespace {

// The package's state, created on first use (the type is SourceLoader's private M4Budget).
template <class State> State& ensure(std::shared_ptr<State>& state) {
    if (state == nullptr) {
        state = std::make_shared<State>();
    }
    return *state;
}

formats::ImportLogEntry cancelledEntry(std::string message) {
    formats::ImportLogEntry e;
    e.level = formats::ImportLogEntry::Level::Info;
    e.code = "open.budget_cancelled";
    e.message = std::move(message);
    return e;
}

} // namespace

// The opens waiting at the gate, by pending id, and the snapshot Continue is publishing right now.
struct SourceLoader::M4Budget {
    struct Pending {
        std::filesystem::path path;
        std::shared_ptr<const model::DataSource> source;
        std::vector<formats::ImportLogEntry> log;
        OpenBudgetCheck check;
    };
    std::map<int, Pending> pending;
    int nextId = 1;
    const model::DataSource* continuing = nullptr; // published() lets this snapshot through once
};

void SourceLoader::setBudgetGate(const store::ChunkCache* cache) {
    budgetGate_ = cache; // the opens already waiting stay until Continue or Cancel
}

void SourceLoader::continueOverBudget(int pendingId) {
    if (m4Budget_ == nullptr) {
        return;
    }
    const auto it = m4Budget_->pending.find(pendingId);
    if (it == m4Budget_->pending.end()) {
        return; // answered already, or dropped by cancelOpens()
    }
    M4Budget::Pending p = std::move(it->second);
    m4Budget_->pending.erase(it);
    m4Budget_->continuing = p.source.get();
    // As the open's own publication (a second open of the same file meanwhile: open.already_open there).
    published(p.path, p.source, std::move(p.log));
    m4Budget_->continuing = nullptr;
}

void SourceLoader::cancelOverBudget(int pendingId) {
    if (m4Budget_ == nullptr) {
        return;
    }
    const auto it = m4Budget_->pending.find(pendingId);
    if (it == m4Budget_->pending.end()) {
        return;
    }
    M4Budget::Pending p = std::move(it->second);
    m4Budget_->pending.erase(it);
    const model::SourceId id = p.source->id;
    bool shared = this->source(id) != nullptr; // the same file opened meanwhile keeps its decode log
    for (const auto& [other, waiting] : m4Budget_->pending) {
        shared = shared || waiting.source->id == id;
    }
    if (!shared) {
        registry_->release(id);
    }
    Q_EMIT importLog(
        p.check.path,
        {cancelledEntry("the open was cancelled: the file needs about " +
                        formatBytes(p.check.estimateBytes).toStdString() + " once loaded, above the " +
                        formatBytes(p.check.budgetBytes).toStdString() + " memory budget")});
}

int SourceLoader::pendingBudgetOpens() const {
    return m4Budget_ == nullptr ? 0 : static_cast<int>(m4Budget_->pending.size());
}

const OpenBudgetCheck* SourceLoader::pendingBudgetCheck(int pendingId) const {
    if (m4Budget_ == nullptr) {
        return nullptr;
    }
    const auto it = m4Budget_->pending.find(pendingId);
    return it == m4Budget_->pending.end() ? nullptr : &it->second.check;
}

bool SourceLoader::budgetHolds(const std::filesystem::path& path,
                               const std::shared_ptr<const model::DataSource>& source,
                               const std::vector<formats::ImportLogEntry>& log) {
    if (m4Budget_ != nullptr && m4Budget_->continuing != nullptr && m4Budget_->continuing == source.get()) {
        m4Budget_->continuing = nullptr;
        return false; // Continue: published as any open
    }
    if (budgetGate_ == nullptr || source == nullptr) {
        return false; // the gate is off: headless opens never wait
    }
    OpenBudgetCheck check;
    check.path = QString::fromStdString(pathToUtf8(path));
    check.estimateBytes = formats::estimateResident(*source).total();
    check.budgetBytes = budgetGate_->budgetBytes();
    check.residentBytes = budgetGate_->stats().residentBytes;
    if (!check.exceeds()) {
        return false;
    }
    M4Budget& b = ensure(m4Budget_);
    const int id = b.nextId++;
    b.pending.emplace(id, M4Budget::Pending{path, source, log, check});
    Q_EMIT budgetExceeded(id, check); // a copy: a receiver may answer it at once
    return true;
}

void SourceLoader::budgetDropPending() {
    if (m4Budget_ == nullptr || m4Budget_->pending.empty()) {
        return;
    }
    std::map<int, M4Budget::Pending> waiting = std::move(m4Budget_->pending);
    m4Budget_->pending.clear();
    for (const auto& [id, p] : waiting) {
        if (this->source(p.source->id) == nullptr) {
            registry_->release(p.source->id);
        }
        Q_EMIT importLog(p.check.path, {cancelledEntry("the open waiting at the memory budget warning was "
                                                       "cancelled (the project changed or the files were "
                                                       "closed)")});
    }
}

void SourceLoader::budgetForget(const model::SourceId& id) {
    // The opens of the closed file that wait at the gate are opens of their own and stay (Continue publishes
    // them as new); only a Continue under way for that source lets go of its snapshot.
    if (m4Budget_ != nullptr && m4Budget_->continuing != nullptr && m4Budget_->continuing->id == id) {
        m4Budget_->continuing = nullptr;
    }
}

} // namespace esses::qt
