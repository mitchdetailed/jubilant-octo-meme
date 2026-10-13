// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Pass 2 of the TextEngine (05 section 7.1): one chunk of one row set, every requested column in one sweep.
#include "text/text_layout.hpp"

#include <esses/base/mapped_file.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace esses::formats::text {
namespace {

struct Member {
    const ColumnPlan* col = nullptr;
    Conversion conv;
    std::size_t carryIx = 0; // position in the set's members (ChunkStart::carry)
    store::ChunkBuilder* builder = nullptr;
    std::byte* values = nullptr;
    double held = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t parseErrors = 0;
    std::string firstBad;
};

bool isSentinel(std::string_view cell, const std::vector<std::string>& sentinels) {
    for (const std::string& s : sentinels) {
        if (cell == s) {
            return true;
        }
    }
    return false;
}

void writeSample(Member& m, std::uint32_t i, double v) {
    const store::Dtype dt = m.conv.dtype;
    bool valid = !std::isnan(v);
    if (valid && (m.conv.a != 1.0 || m.conv.b != 0.0)) {
        v = m.conv.a * v + m.conv.b;
    }
    switch (dt) {
    case store::Dtype::F32: {
        const float f = valid ? static_cast<float>(v) : std::numeric_limits<float>::quiet_NaN();
        std::memcpy(m.values + std::size_t{i} * 4, &f, 4);
        break;
    }
    case store::Dtype::F64: {
        const double d = valid ? v : std::numeric_limits<double>::quiet_NaN();
        std::memcpy(m.values + std::size_t{i} * 8, &d, 8);
        break;
    }
    case store::Dtype::I32: {
        std::int32_t x = 0;
        if (valid) {
            const double r = std::round(v);
            if (r >= -2147483648.0 && r <= 2147483647.0) {
                x = static_cast<std::int32_t>(r);
            } else {
                valid = false;
            }
        }
        std::memcpy(m.values + std::size_t{i} * 4, &x, 4);
        break;
    }
    case store::Dtype::I64:
        valid = false;
        break;
    }
    if (!valid) {
        m.builder->setValid(i, false);
    }
}

} // namespace

Result<void> decodeChunk(const Layout& layout, const std::vector<Conversion>& conversions,
                         std::uint32_t setIx, std::uint64_t chunkNo, std::span<MemberOut> members,
                         store::ChunkBuilder* times, DecodeLog* log, const CancelToken& cancel) {
    if (setIx >= layout.rowSets.size()) {
        return fail(ErrorCode::NotFound, "text decode: no row set " + std::to_string(setIx));
    }
    const RowSet& set = layout.rowSets[setIx];
    const std::uint32_t count =
        store::chunkSampleCount(static_cast<SampleIdx>(set.samples), static_cast<SampleIdx>(chunkNo));
    if (count == 0 || chunkNo >= set.chunks.size()) {
        return fail(ErrorCode::Invalid,
                    "text decode: chunk " + std::to_string(chunkNo) + " is outside the set");
    }
    const ChunkStart& start = set.chunks[chunkNo];
    // The mapping the text lies in (none for a transcoded copy or an in-memory source), held for the decode.
    const std::shared_ptr<const MappedFile> mapping =
        layout.owned.empty() && layout.source != nullptr ? layout.source->mappedFile() : nullptr;
    if (mapping != nullptr && mapping->changedOnDisk()) {
        if (log != nullptr) {
            log->add(ImportLogEntry::Level::Warning, std::string(code::kSourceChanged),
                     "the file changed on disk after it was opened; reopen it", {});
        }
        ErrorContext ctx;
        ctx.path = mapping->path();
        return fail(ErrorCode::Io, "the text file changed on disk after it was opened", std::move(ctx));
    }
    std::vector<Member> ms(members.size());
    for (std::size_t i = 0; i < members.size(); ++i) {
        Member& m = ms[i];
        if (members[i].column >= layout.columns.size() || members[i].builder == nullptr) {
            return fail(ErrorCode::Invalid, "text decode: bad member");
        }
        m.col = &layout.columns[members[i].column];
        m.conv = members[i].column < conversions.size() ? conversions[members[i].column]
                                                        : Conversion{m.col->dtype, 1.0, 0.0};
        const auto pos = std::lower_bound(set.members.begin(), set.members.end(), members[i].column);
        m.carryIx = static_cast<std::size_t>(pos - set.members.begin());
        if (m.col->hold && m.carryIx < start.carry.size()) {
            m.held = start.carry[m.carryIx];
        }
        m.builder = members[i].builder;
        if (auto r = m.builder->allocate(count, m.conv.dtype, true); !r) {
            return r;
        }
        m.values = m.builder->sampleBytes().data();
    }
    std::int64_t* timesOut = nullptr;
    if (times != nullptr) {
        if (auto r = times->allocate(count, store::Dtype::I64, false); !r) {
            return r;
        }
        timesOut = times->samples<std::int64_t>().data();
    }

    const Dialect& d = layout.dialect;
    const bool ts = set.timestamped;
    const std::int64_t sparseField =
        set.sparseColumn >= 0
            ? static_cast<std::int64_t>(layout.columns[static_cast<std::size_t>(set.sparseColumn)].field)
            : -1;
    GuardedLines cur(layout.text, static_cast<std::size_t>(start.offset), layout.crOnly, mapping.get());
    std::string_view line;
    std::uint64_t lineNo = start.line;
    std::uint32_t emitted = 0;
    std::uint64_t rows = 0;
    std::vector<Cell> cells;
    std::vector<double> curVals(ms.size());
    std::vector<double> pendVals(ms.size());
    bool pending = false;
    Micros pendTime = 0;
    bool emittedAny = start.prevTime != std::numeric_limits<Micros>::min();
    Micros lastEmitted = start.prevTime;
    std::uint64_t ragged = 0;
    std::uint64_t firstRaggedLine = 0;
    std::string label;

    const auto emit = [&](const std::vector<double>& vals, Micros t) {
        for (std::size_t i = 0; i < ms.size(); ++i) {
            writeSample(ms[i], emitted, vals[i]);
        }
        if (timesOut != nullptr) {
            timesOut[emitted] = t;
        }
        ++emitted;
    };

    while (emitted < count && cur.next(line)) {
        const std::uint64_t thisLine = lineNo++;
        if (isBlank(line)) {
            continue;
        }
        if ((rows++ & 0xFFF) == 0 && cancel.cancelled()) {
            return fail(ErrorCode::Cancelled, "text decode cancelled");
        }
        splitCells(line, d, cells, layout.headerFields + 2); // enough to tell a ragged row
        if (layout.trailingDelimiter && cells.size() == layout.headerFields + 1 &&
            cells.back().text.empty()) {
            cells.pop_back();
        }
        if (cells.size() != layout.headerFields) {
            if (ragged++ == 0) {
                firstRaggedLine = thisLine;
            }
        }
        for (std::size_t i = 0; i < ms.size(); ++i) {
            Member& m = ms[i];
            const ColumnPlan& col = *m.col;
            double v = std::numeric_limits<double>::quiet_NaN();
            if (col.field < cells.size()) {
                const Cell& cell = cells[col.field];
                if (cell.text.empty()) {
                    v = col.hold ? m.held : v;
                } else if (isSentinel(cell.text, col.sentinels)) {
                    if (col.hold) {
                        m.held = v;
                    }
                } else if (col.kind == store::SampleKind::Enum) {
                    label = toUtf8(cellText(cell, d), layout.enc);
                    const auto it = col.enumIds.find(label);
                    if (it != col.enumIds.end()) {
                        v = static_cast<double>(it->second);
                    } else if (m.parseErrors++ == 0) {
                        m.firstBad = label.substr(0, 32);
                    }
                    if (col.hold) {
                        m.held = v;
                    }
                } else {
                    if (const auto x = parseNumber(cell.text, d.decimal)) {
                        v = *x;
                    } else if (m.parseErrors++ == 0) {
                        m.firstBad = std::string(cell.text.substr(0, 32));
                    }
                    if (col.hold) {
                        m.held = v;
                    }
                }
            }
            curVals[i] = v;
        }
        if (!ts) {
            emit(curVals, 0);
            continue;
        }
        const std::optional<Micros> t = rowTime(layout, cells);
        if (!t) {
            continue;
        }
        // A sparse set drops the rows whose cell is empty; a missing cell (a ragged row) is an invalid
        // sample, the rule of pass 1 (text_analyse.cpp fullScan).
        if (sparseField >= 0 && static_cast<std::size_t>(sparseField) < cells.size() &&
            cells[static_cast<std::size_t>(sparseField)].text.empty()) {
            continue;
        }
        if (!pending) {
            if (emittedAny && *t <= lastEmitted) {
                continue;
            }
        } else if (*t > pendTime) {
            emit(pendVals, pendTime);
            lastEmitted = pendTime;
            emittedAny = true;
        } else if (*t < pendTime) {
            continue;
        }
        pending = true;
        pendTime = *t;
        std::swap(curVals, pendVals);
    }
    if (cur.error()) {
        return Failure{*cur.error()}; // a page fault: the file was truncated under the mapping
    }
    if (ts && pending && emitted < count) {
        emit(pendVals, pendTime);
    }
    if (emitted < count) {
        // The file holds fewer rows than pass 1 indexed (it changed under the mapping): the rest is invalid.
        for (Member& m : ms) {
            for (std::uint32_t k = emitted; k < count; ++k) {
                writeSample(m, k, std::numeric_limits<double>::quiet_NaN());
            }
        }
        if (timesOut != nullptr) {
            const Micros last = emitted > 0 ? timesOut[emitted - 1] : start.prevTime;
            for (std::uint32_t k = emitted; k < count; ++k) {
                timesOut[k] = last + static_cast<Micros>(k - emitted + 1);
            }
        }
        if (log != nullptr) {
            ImportLogEntry::Context ctx;
            ctx.detail = std::to_string(count - emitted);
            log->add(ImportLogEntry::Level::Warning, std::string(code::kSourceChanged),
                     "the file has fewer rows than when it was opened; the missing samples are invalid",
                     std::move(ctx));
        }
    }
    if (log != nullptr) {
        if (ragged > 0) {
            ImportLogEntry::Context ctx;
            ctx.line = firstRaggedLine;
            ctx.repeats = static_cast<std::uint32_t>(std::min<std::uint64_t>(ragged, 0xFFFFFFFFu));
            log->add(ImportLogEntry::Level::Warning, std::string(code::kRaggedRow),
                     "rows with a different number of cells than the header; missing cells are invalid",
                     std::move(ctx));
        }
        for (const Member& m : ms) {
            if (m.parseErrors > 0) {
                ImportLogEntry::Context ctx;
                ctx.channel = m.col->loggedName;
                ctx.detail = m.firstBad;
                ctx.repeats = static_cast<std::uint32_t>(std::min<std::uint64_t>(m.parseErrors, 0xFFFFFFFFu));
                log->add(ImportLogEntry::Level::Warning, std::string(code::kParseError),
                         "cells that are neither a number nor a sentinel are invalid", std::move(ctx));
            }
        }
    }
    return ok();
}

} // namespace esses::formats::text
