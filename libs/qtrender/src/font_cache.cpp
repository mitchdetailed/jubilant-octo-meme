// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include "font_cache.hpp"

#include <QCoreApplication>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <unordered_map>
#include <utility>

namespace esses::qt::detail {

namespace {

struct FontKey {
    std::uint64_t setId = 0;
    std::uint32_t slot = 0;
    friend bool operator==(const FontKey&, const FontKey&) = default;
};

struct FontKeyHash {
    std::size_t operator()(const FontKey& k) const noexcept {
        return std::hash<std::uint64_t>{}((k.setId * 0x9E3779B97F4A7C15ULL) ^ k.slot);
    }
};

// Fonts of superseded snapshots accumulate as schemes change; the map is simply dropped past this size.
constexpr std::size_t kMaxFonts = 512;

using StaticTextList = std::list<std::pair<std::string, QStaticText>>;

struct ThreadCaches {
    std::unordered_map<FontKey, std::unique_ptr<FontEntry>, FontKeyHash> fonts;
    StaticTextList lru; // most recently used first
    std::unordered_map<std::string, StaticTextList::iterator> index;

    void clear() {
        index.clear();
        lru.clear();
        fonts.clear();
    }
};

ThreadCaches& rawCaches() {
    thread_local ThreadCaches caches;
    return caches;
}

// The application whose post routine is registered (written on the application's thread only; read anywhere).
std::atomic<QCoreApplication*> gCleanupRegisteredFor{nullptr};

void clearApplicationThreadCaches() {
    rawCaches().clear();
    gCleanupRegisteredFor.store(nullptr);
}

ThreadCaches& caches() {
    ThreadCaches& c = rawCaches();
    QCoreApplication* app = QCoreApplication::instance();
    if (app != nullptr && gCleanupRegisteredFor.load() != app && QThread::currentThread() == app->thread()) {
        gCleanupRegisteredFor.store(app);
        qAddPostRoutine(&clearApplicationThreadCaches);
    }
    return c;
}

} // namespace

std::string_view roleForStyle(plot::TextStyleId style, bool rotated) noexcept {
    switch (style) {
    case plot::TextStyleId::Axis:
        return rotated ? "axisRotated" : "axis";
    case plot::TextStyleId::Caption:
    case plot::TextStyleId::Banner:
        return "caption";
    case plot::TextStyleId::Key:
    case plot::TextStyleId::KeyValue:
        return "key";
    case plot::TextStyleId::Band:
    case plot::TextStyleId::Measurement:
    case plot::TextStyleId::GaugeUnits:
        return "small";
    case plot::TextStyleId::Gauge:
        return "gauge";
    case plot::TextStyleId::Table:
        return "reportTable";
    case plot::TextStyleId::Equation:
        return "equation";
    case plot::TextStyleId::ColourBand:
        return "colourBands";
    case plot::TextStyleId::TrackLabel:
        return "trackLabel";
    }
    return "caption";
}

QFont makeFont(const std::string& family, double pointSize, int weight, bool italic) {
    QFont f(QString::fromStdString(family));
    f.setPointSizeF(pointSize > 0.0 ? pointSize : 9.0);
    f.setWeight(static_cast<QFont::Weight>(std::clamp(weight, 1, 1000)));
    f.setItalic(italic);
    f.setStyleStrategy(QFont::NoSubpixelAntialias);
    return f;
}

std::uint64_t nextFontSetId() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

const FontEntry* findThreadFont(std::uint64_t setId, std::uint32_t slot) {
    ThreadCaches& c = caches();
    const auto it = c.fonts.find(FontKey{setId, slot});
    return it == c.fonts.end() ? nullptr : it->second.get();
}

const FontEntry& insertThreadFont(std::uint64_t setId, std::uint32_t slot, const QFont& font) {
    ThreadCaches& c = caches();
    if (c.fonts.size() >= kMaxFonts) {
        c.fonts.clear();
    }
    auto entry = std::make_unique<FontEntry>(FontEntry{font, QFontMetricsF(font)});
    const FontEntry& ref = *entry;
    c.fonts[FontKey{setId, slot}] = std::move(entry);
    return ref;
}

QStaticText& threadStaticText(const std::string& key, const QString& text, bool* created) {
    ThreadCaches& c = caches();
    const auto it = c.index.find(key);
    if (created != nullptr) {
        *created = it == c.index.end();
    }
    if (it != c.index.end()) {
        c.lru.splice(c.lru.begin(), c.lru, it->second);
        return it->second->second;
    }
    QStaticText st(text);
    st.setTextFormat(Qt::PlainText);
    c.lru.emplace_front(key, std::move(st));
    c.index.emplace(key, c.lru.begin());
    while (c.lru.size() > kStaticTextCapacity) {
        c.index.erase(c.lru.back().first);
        c.lru.pop_back();
    }
    return c.lru.front().second;
}

std::size_t threadStaticTextCount() {
    return caches().lru.size();
}

} // namespace esses::qt::detail
