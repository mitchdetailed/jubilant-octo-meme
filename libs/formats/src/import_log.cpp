// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/formats/import_log.hpp>

#include <algorithm>
#include <utility>

namespace esses::formats {

std::string_view importLogLevelName(ImportLogEntry::Level level) noexcept {
    switch (level) {
    case ImportLogEntry::Level::Info:
        return "Info";
    case ImportLogEntry::Level::Warning:
        return "Warning";
    case ImportLogEntry::Level::Error:
        return "Error";
    }
    return "Info";
}

ImportLog::ImportLog(const ImportLog& other) {
    std::lock_guard lock(other.m_);
    items_ = other.items_;
    seen_ = other.seen_;
    version_ = other.version_;
}

ImportLog& ImportLog::operator=(const ImportLog& other) {
    if (this == &other) {
        return *this;
    }
    std::scoped_lock lock(m_, other.m_);
    items_ = other.items_;
    seen_ = other.seen_;
    version_ = other.version_;
    return *this;
}

void ImportLog::add(ImportLogEntry e) {
    std::lock_guard lock(m_);
    Key key{e.code, e.ctx.layer.value_or(std::string()), e.ctx.channel.value_or(std::string())};
    auto [it, inserted] = seen_.try_emplace(std::move(key), 0u, 0u);
    auto& [occurrences, latest] = it->second;
    ++version_;
    if (occurrences >= 3) {
        items_[latest].ctx.repeats += e.ctx.repeats;
        ++occurrences;
        return;
    }
    ++occurrences;
    latest = items_.size();
    items_.push_back(std::move(e));
}

void ImportLog::add(ImportLogEntry::Level level, std::string code, std::string message,
                    ImportLogEntry::Context ctx) {
    ImportLogEntry e;
    e.level = level;
    e.code = std::move(code);
    e.message = std::move(message);
    e.ctx = std::move(ctx);
    add(std::move(e));
}

std::vector<ImportLogEntry> ImportLog::snapshot() const {
    std::vector<ImportLogEntry> out = entries();
    std::stable_sort(out.begin(), out.end(), [](const ImportLogEntry& a, const ImportLogEntry& b) {
        const auto key = [](const ImportLogEntry& e) {
            return std::make_tuple(std::string_view(e.code), e.ctx.layer.value_or(std::string()),
                                   e.ctx.channel.value_or(std::string()), e.ctx.offset.value_or(0));
        };
        return key(a) < key(b);
    });
    return out;
}

std::vector<ImportLogEntry> ImportLog::entries() const {
    std::lock_guard lock(m_);
    return items_;
}

std::uint64_t ImportLog::version() const {
    std::lock_guard lock(m_);
    return version_;
}

bool ImportLog::hasErrors() const {
    std::lock_guard lock(m_);
    return std::any_of(items_.begin(), items_.end(),
                       [](const ImportLogEntry& e) { return e.level == ImportLogEntry::Level::Error; });
}

bool ImportLog::contains(std::string_view code) const {
    return count(code) != 0;
}

std::size_t ImportLog::count(std::string_view code) const {
    std::lock_guard lock(m_);
    return static_cast<std::size_t>(
        std::count_if(items_.begin(), items_.end(), [&](const ImportLogEntry& e) { return e.code == code; }));
}

std::size_t ImportLog::size() const {
    std::lock_guard lock(m_);
    return items_.size();
}

} // namespace esses::formats
