// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// StatCache (04 section 12 item 1, 01 section 7.1). Owner: the reports work (src/reports/).
//
// An LRU of report cells behind one mutex. A key is stored as its 128-bit digest (xxh3-128 of the key's
// fields), so an entry is a flat slot of digest, value and two list links in one vector (no node per cell);
// the slots form a doubly linked recency list, most recent first. The capacity is budget / kBytesPerCell
// cells: the 64 bytes per cell of 01 section 7.1 are the accounting unit, the slot itself is 48 bytes plus
// one hash-map entry.

#include <esses/analysis/stat_cache.hpp>
#include <esses/base/hash.hpp>

#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace esses::analysis {

namespace {

constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

template <class T> void put(std::byte*& out, const T& v) noexcept {
    std::memcpy(out, &v, sizeof(T));
    out += sizeof(T);
}

Hash128 digestOf(const StatKey& k) noexcept {
    std::array<std::byte, 16 + 8 + 16 + 1 + 8 + 8 + 8> bytes{};
    std::byte* out = bytes.data();
    std::memcpy(out, k.channel.v.b.data(), 16);
    out += 16;
    put(out, k.version);
    std::memcpy(out, k.range.v.b.data(), 16);
    out += 16;
    put(out, static_cast<std::uint8_t>(k.stat));
    put(out, k.clipStart);
    put(out, k.clipEnd);
    put(out, k.optionsHash);
    return hash128(std::span<const std::byte>(bytes.data(), bytes.size()));
}

} // namespace

struct StatCache::Impl {
    struct Slot {
        Hash128 key;
        StatValue value;
        std::uint32_t prev = kNone; // towards the most recent
        std::uint32_t next = kNone; // towards the least recent
    };

    std::size_t capacity = 0;
    std::mutex mutex;
    std::vector<Slot> slots;
    std::unordered_map<Hash128, std::uint32_t> index;
    std::uint32_t head = kNone; // most recent
    std::uint32_t tail = kNone; // least recent

    void unlink(std::uint32_t i) noexcept {
        Slot& s = slots[i];
        if (s.prev != kNone) {
            slots[s.prev].next = s.next;
        } else {
            head = s.next;
        }
        if (s.next != kNone) {
            slots[s.next].prev = s.prev;
        } else {
            tail = s.prev;
        }
        s.prev = kNone;
        s.next = kNone;
    }
    void pushFront(std::uint32_t i) noexcept {
        Slot& s = slots[i];
        s.prev = kNone;
        s.next = head;
        if (head != kNone) {
            slots[head].prev = i;
        }
        head = i;
        if (tail == kNone) {
            tail = i;
        }
    }
    void touch(std::uint32_t i) noexcept {
        if (head != i) {
            unlink(i);
            pushFront(i);
        }
    }
};

StatCache::StatCache(std::size_t budgetBytes) : impl_(std::make_unique<Impl>()), budget_(budgetBytes) {
    impl_->capacity = budgetBytes / kBytesPerCell;
}

StatCache::~StatCache() = default;

std::optional<StatValue> StatCache::find(const StatKey& key) const {
    const Hash128 digest = digestOf(key);
    Impl& m = *impl_; // the recency list changes on a hit (const lookup, mutable state)
    const std::lock_guard lock(m.mutex);
    const auto it = m.index.find(digest);
    if (it == m.index.end()) {
        return std::nullopt;
    }
    m.touch(it->second);
    return m.slots[it->second].value;
}

void StatCache::insert(const StatKey& key, const StatValue& value) {
    const Hash128 digest = digestOf(key);
    Impl& m = *impl_;
    const std::lock_guard lock(m.mutex);
    if (m.capacity == 0) {
        return;
    }
    if (const auto it = m.index.find(digest); it != m.index.end()) {
        m.slots[it->second].value = value;
        m.touch(it->second);
        return;
    }
    std::uint32_t slot = kNone;
    if (m.index.size() >= m.capacity && m.tail != kNone) {
        slot = m.tail; // evict the least recently used and reuse its slot
        m.unlink(slot);
        m.index.erase(m.slots[slot].key);
    } else {
        slot = static_cast<std::uint32_t>(m.slots.size());
        m.slots.emplace_back();
    }
    m.slots[slot].key = digest;
    m.slots[slot].value = value;
    m.pushFront(slot);
    m.index.emplace(digest, slot);
}

void StatCache::clear() {
    Impl& m = *impl_;
    const std::lock_guard lock(m.mutex);
    m.slots.clear();
    m.slots.shrink_to_fit();
    m.index.clear();
    m.head = kNone;
    m.tail = kNone;
}

std::size_t StatCache::size() const {
    Impl& m = *impl_;
    const std::lock_guard lock(m.mutex);
    return m.index.size();
}

std::size_t StatCache::bytes() const {
    return size() * kBytesPerCell;
}

} // namespace esses::analysis
