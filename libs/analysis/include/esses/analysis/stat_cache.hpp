// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// StatCache (04 section 12 item 1; 01 sections 4.2 and 7.1; ARCHITECTURE_OUTLINE section 4 esses_analysis):
// the report cells already computed, keyed (channel id, column version, range id, statistic) plus what else
// changes the value (the interval, the options that affect a statistic). At most 64 bytes per cell, LRU with
// a 64 MB cap (01 section 7.1): the capacity is budgetBytes / kBytesPerCell cells, and bytes() (the heap the
// cache holds) never exceeds the budget. Thread-safe: report workers
// look up and insert concurrently; one cache per workspace, owned by esses_qtmodel (ReportModel's service).
// Keys are held as 128-bit digests of their fields.
#pragma once

#include <esses/analysis/report_spec.hpp>
#include <esses/base/time.hpp>
#include <esses/model/ids.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace esses::analysis {

struct StatKey {
    model::ChannelId channel;
    // The channel's column: Column::version(); ReportEngine mixes in Column::id(), so a maths output that is
    // a new column under the same ChannelId never hits an older value.
    std::uint64_t version = 0;
    model::RangeId range;
    ReportStat stat = ReportStat::Avg;
    // The interval [clipStart, clipEnd) the value covers; both 0 = the whole range. ReportEngine always sets
    // it, since a range keeps its id when its bounds move (beacon offsets) and a zoom-linked report clips it.
    Micros clipStart = 0;
    Micros clipEnd = 0;
    std::uint64_t optionsHash = 0; // probeSeconds, interpolateBoundaries (what changes the value)
    // The trusted pieces scanned when untrusted laps were cut out of the interval (D-197 (b)); 0: the whole.
    std::uint64_t piecesHash = 0;
    friend bool operator==(const StatKey&, const StatKey&) = default;
};

struct StatValue {
    double value = 0.0; // in base units
    Micros at = 0;      // the time of a min/max/start/end sample (key column hover, track_report later)
    bool valid = false; // false: no valid sample in the range (a blank cell)
    friend bool operator==(const StatValue&, const StatValue&) = default;
};

class StatCache {
public:
    static constexpr std::size_t kDefaultBudgetBytes = std::size_t{64} * 1024 * 1024;
    static constexpr std::size_t kBytesPerCell = 64;

    explicit StatCache(std::size_t budgetBytes = kDefaultBudgetBytes);
    ~StatCache();
    StatCache(const StatCache&) = delete;
    StatCache& operator=(const StatCache&) = delete;

    std::optional<StatValue> find(const StatKey& key) const; // a hit counts as a use (LRU)
    void insert(const StatKey& key, const StatValue& value); // evicts the least recently used over budget
    void clear();
    std::size_t size() const;  // cells held
    std::size_t bytes() const; // the slots and index the cache holds (<= budgetBytes)
    std::size_t budgetBytes() const noexcept { return budget_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::size_t budget_ = kDefaultBudgetBytes;
};

} // namespace esses::analysis
