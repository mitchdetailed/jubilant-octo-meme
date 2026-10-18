// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// StatCache (04 section 12 item 1, 01 section 7.1). Owner: the reports work (src/reports/).
//
// An LRU of report cells behind one mutex. A key is stored as its 128-bit digest (xxh3-128 of the key's
// fields), so an entry is a flat slot of digest, value and two list links in one vector (no node per cell);
// the slots form a doubly linked recency list, most recent first, and an open-addressing index of slot
// numbers finds them. The capacity is budget / kBytesPerCell cells and a cell costs at most kBytesPerCell
// bytes (a 48-byte slot and at most four 4-byte index entries), so bytes() never exceeds the budget.

#include <esses/analysis/stat_cache.hpp>
#include <esses/base/hash.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace esses::analysis {

namespace {

constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

template <class T> void put(std::byte*& out, const T& v) noexcept {
    std::memcpy(out, &v, sizeof(T));
    out += sizeof(T);
}

Hash128 digestOf(const StatKey& k) noexcept {
    std::array<std::byte, 16 + 8 + 16 + 1 + 8 + 8 + 8 + 8> bytes{};
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
    put(out, k.piecesHash);
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
    // The memory rule of 01 section 7.1 holds by construction: a cell is one 48-byte slot plus at most four
    // index entries (the index is a power of two of at least twice the cells, so at most four times the
    // capacity), and neither vector grows past what the capacity needs.
    static_assert(sizeof(Slot) + 4 * sizeof(std::uint32_t) <= kBytesPerCell);

    std::size_t capacity = 0;
    std::mutex mutex;
    std::vector<Slot> slots;
    std::vector<std::uint32_t> index; // open addressing, linear probing: slot numbers, kNone for empty
    std::size_t live = 0;             // entries in the index
    std::uint32_t head = kNone;       // most recent
    std::uint32_t tail = kNone;       // least recent

    std::size_t home(const Hash128& key) const noexcept {
        return static_cast<std::size_t>(key.lo) & (index.size() - 1); // xxh3 digests are well mixed
    }
    // The index position holding `key`, or nullopt.
    std::optional<std::size_t> position(const Hash128& key) const noexcept {
        if (index.empty()) {
            return std::nullopt;
        }
        const std::size_t mask = index.size() - 1;
        for (std::size_t i = home(key);; i = (i + 1) & mask) {
            const std::uint32_t s = index[i];
            if (s == kNone) {
                return std::nullopt;
            }
            if (slots[s].key == key) {
                return i;
            }
        }
    }
    void place(std::uint32_t slot) noexcept {
        const std::size_t mask = index.size() - 1;
        std::size_t i = home(slots[slot].key);
        while (index[i] != kNone) {
            i = (i + 1) & mask;
        }
        index[i] = slot;
    }
    void growIndex() {
        std::size_t size = index.empty() ? 2 : index.size();
        while (size < 2 * (live + 1)) {
            size *= 2;
        }
        if (size == index.size()) {
            return;
        }
        index.assign(size, kNone);
        for (std::uint32_t s = head; s != kNone; s = slots[s].next) {
            place(s);
        }
    }
    // Backward-shift deletion: the entries after the hole that may move into it do.
    void erase(std::size_t hole) noexcept {
        const std::size_t mask = index.size() - 1;
        index[hole] = kNone;
        for (std::size_t j = (hole + 1) & mask; index[j] != kNone; j = (j + 1) & mask) {
            const std::size_t k = home(slots[index[j]].key);
            // The entry at j stays when its home lies cyclically in (hole, j].
            const bool stays = hole <= j ? (hole < k && k <= j) : (hole < k || k <= j);
            if (!stays) {
                index[hole] = index[j];
                index[j] = kNone;
                hole = j;
            }
        }
        --live;
    }
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
    const std::optional<std::size_t> at = m.position(digest);
    if (!at) {
        return std::nullopt;
    }
    const std::uint32_t slot = m.index[*at];
    m.touch(slot);
    return m.slots[slot].value;
}

void StatCache::insert(const StatKey& key, const StatValue& value) {
    const Hash128 digest = digestOf(key);
    Impl& m = *impl_;
    const std::lock_guard lock(m.mutex);
    if (m.capacity == 0) {
        return;
    }
    if (const std::optional<std::size_t> at = m.position(digest)) {
        const std::uint32_t slot = m.index[*at];
        m.slots[slot].value = value;
        m.touch(slot);
        return;
    }
    std::uint32_t slot = kNone;
    if (m.live >= m.capacity && m.tail != kNone) {
        slot = m.tail; // evict the least recently used and reuse its slot
        if (const std::optional<std::size_t> old = m.position(m.slots[slot].key)) {
            m.erase(*old);
        }
        m.unlink(slot);
    } else {
        if (m.slots.size() == m.slots.capacity()) {
            m.slots.reserve(std::min(m.capacity, std::max<std::size_t>(16, 2 * m.slots.size())));
        }
        slot = static_cast<std::uint32_t>(m.slots.size());
        m.slots.emplace_back();
    }
    m.slots[slot].key = digest;
    m.slots[slot].value = value;
    m.growIndex(); // rehashes the linked slots; the new one is placed next
    m.place(slot);
    m.pushFront(slot);
    ++m.live;
}

void StatCache::clear() {
    Impl& m = *impl_;
    const std::lock_guard lock(m.mutex);
    m.slots.clear();
    m.slots.shrink_to_fit();
    m.index.clear();
    m.index.shrink_to_fit();
    m.live = 0;
    m.head = kNone;
    m.tail = kNone;
}

std::size_t StatCache::size() const {
    Impl& m = *impl_;
    const std::lock_guard lock(m.mutex);
    return m.live;
}

std::size_t StatCache::bytes() const {
    Impl& m = *impl_;
    const std::lock_guard lock(m.mutex);
    return m.slots.capacity() * sizeof(Impl::Slot) + m.index.capacity() * sizeof(std::uint32_t);
}

} // namespace esses::analysis
