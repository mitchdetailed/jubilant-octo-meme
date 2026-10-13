// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// TextEngine (05 section 7.1, D-029): the IImporter over a TextProfile, its IReader, and the shared slab
// decoder that sweeps one chunk of a row set for every column at once. The decoder hands each sibling's
// chunk to the thread already waiting for it, or seals it into the ChunkCache with insertGroup (05 section
// 7.1 "one sweep per slab, shared by all columns"; the share key slab/<sourceId>/<set>/<chunk> is the
// decoder's in-flight table rather than a TaskPool key).
#include "text/text_layout.hpp"

#include <esses/base/mapped_file.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/text_engine.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace esses::formats {
namespace {

using text::Conversion;
using text::Layout;

// The values of column `index`, or the times of row set `index`.
struct Target {
    bool times = false;
    std::uint32_t index = 0;
    friend bool operator<(const Target& a, const Target& b) {
        return a.times != b.times ? a.times < b.times : a.index < b.index;
    }
    friend bool operator==(const Target&, const Target&) = default;
};

class SlabDecoder {
public:
    SlabDecoder(std::shared_ptr<const Layout> layout,
                std::shared_ptr<const std::vector<Conversion>> conversions, std::shared_ptr<DecodeLog> log)
        : layout_(std::move(layout)), conv_(std::move(conversions)), log_(std::move(log)),
          values_(layout_->columns.size()), times_(layout_->rowSets.size()) {}

    void attach(std::uint32_t column, const store::Column& values, const store::Column* times,
                store::ChunkCache& cache) {
        std::lock_guard lock(m_);
        cache_ = &cache;
        if (column < values_.size()) {
            values_[column] = store::WeakColumn(values);
            const std::uint32_t set = layout_->columns[column].rowSet;
            if (times != nullptr && set < times_.size() && times_[set].expired()) {
                times_[set] = store::WeakColumn(*times);
            }
        }
    }

    Result<void> materialise(Target t, std::uint64_t chunkNo, store::ChunkBuilder& out,
                             const CancelToken& cancel) {
        const std::uint32_t set = t.times ? t.index : layout_->columns[t.index].rowSet;
        const auto key = std::make_pair(set, chunkNo);
        {
            std::unique_lock lock(m_);
            auto it = flights_.find(key);
            if (it != flights_.end() && it->second.decoding) {
                it->second.waiters.insert(t);
                cv_.wait(lock, [&] {
                    const auto f = flights_.find(key);
                    return f == flights_.end() || !f->second.decoding;
                });
                it = flights_.find(key);
                if (it != flights_.end()) {
                    const auto h = it->second.handoff.find(t);
                    if (h != it->second.handoff.end()) {
                        out = std::move(h->second);
                        it->second.handoff.erase(h);
                        eraseIfDone(it);
                        return ok();
                    }
                    it->second.waiters.erase(t);
                }
            }
            if (it == flights_.end()) {
                it = flights_.emplace(key, Flight{}).first;
            }
            it->second.decoding = true;
        }

        // One sweep for every member of the set (and its time column when it has one).
        const text::RowSet& rs = layout_->rowSets[set];
        std::vector<store::ChunkBuilder> builders(rs.members.size());
        std::vector<text::MemberOut> outs(rs.members.size());
        for (std::size_t i = 0; i < rs.members.size(); ++i) {
            outs[i] = text::MemberOut{rs.members[i], &builders[i]};
        }
        store::ChunkBuilder timesBuilder;
        Result<void> r = text::decodeChunk(*layout_, *conv_, set, chunkNo, outs,
                                           rs.timestamped ? &timesBuilder : nullptr, log_.get(), cancel);

        std::vector<std::pair<Target, store::ChunkBuilder*>> toSeal;
        store::ChunkCache* cache = nullptr;
        {
            std::lock_guard lock(m_);
            auto it = flights_.find(key);
            if (r) {
                const auto place = [&](Target mt, store::ChunkBuilder& b) {
                    if (mt == t) {
                        out = std::move(b);
                    } else if (it->second.waiters.erase(mt) > 0) {
                        it->second.handoff.emplace(mt, std::move(b));
                    } else {
                        toSeal.emplace_back(mt, &b);
                    }
                };
                for (std::size_t i = 0; i < rs.members.size(); ++i) {
                    place(Target{false, rs.members[i]}, builders[i]);
                }
                if (rs.timestamped) {
                    place(Target{true, set}, timesBuilder);
                }
            }
            it->second.decoding = false;
            cache = cache_;
            eraseIfDone(it);
        }
        cv_.notify_all();
        if (!r) {
            return r;
        }
        if (cache != nullptr && !toSeal.empty()) {
            sealSiblings(*cache, chunkNo, toSeal);
        }
        return ok();
    }

private:
    struct Flight {
        bool decoding = false;
        std::set<Target> waiters;
        std::map<Target, store::ChunkBuilder> handoff;
    };
    using Flights = std::map<std::pair<std::uint32_t, std::uint64_t>, Flight>;

    void eraseIfDone(Flights::iterator it) {
        if (!it->second.decoding && it->second.waiters.empty() && it->second.handoff.empty()) {
            flights_.erase(it);
        }
    }

    void sealSiblings(store::ChunkCache& cache, std::uint64_t chunkNo,
                      std::vector<std::pair<Target, store::ChunkBuilder*>>& members) {
        // The columns still alive, held for the duration of insertGroup (it reads them through pointers).
        std::vector<std::pair<store::Column, store::ChunkBuilder*>> alive;
        alive.reserve(members.size());
        {
            std::lock_guard lock(m_);
            for (auto& [mt, b] : members) {
                store::Column c = (mt.times ? times_[mt.index] : values_[mt.index]).lock();
                if (c.valid() && c.dtype() == b->dtype()) {
                    alive.emplace_back(std::move(c), b);
                }
            }
        }
        std::vector<store::ChunkCache::SealedChunk> group;
        group.reserve(alive.size());
        for (auto& [c, b] : alive) {
            auto sealed = b->seal(c.version());
            if (!sealed) {
                continue;
            }
            store::ChunkCache::SealedChunk s;
            s.column = &c;
            s.chunkNo = static_cast<SampleIdx>(chunkNo);
            s.version = c.version();
            s.chunk = std::move(sealed).value();
            group.push_back(std::move(s));
        }
        if (!group.empty()) {
            cache.insertGroup(group);
        }
    }

    std::shared_ptr<const Layout> layout_;
    std::shared_ptr<const std::vector<Conversion>> conv_;
    std::shared_ptr<DecodeLog> log_;
    std::mutex m_;
    std::condition_variable cv_;
    Flights flights_;
    std::vector<store::WeakColumn> values_; // per column
    std::vector<store::WeakColumn> times_;  // per row set
    store::ChunkCache* cache_ = nullptr;
};

class TextChunkProvider final : public store::IChunkProvider {
public:
    TextChunkProvider(std::shared_ptr<SlabDecoder> decoder, Target target, SampleIdx size, store::Dtype dtype,
                      std::string name, std::shared_ptr<DecodeLog> log)
        : decoder_(std::move(decoder)), target_(target), size_(size), dtype_(dtype), name_(std::move(name)),
          log_(std::move(log)) {}

    SampleIdx size() const override { return size_; }
    store::Dtype dtype() const override { return dtype_; }
    std::uint64_t version() const override { return 1; }
    bool chunkIndependent() const override { return true; }
    std::string debugName() const override { return name_; }

    Result<void> materialise(SampleIdx chunkNo, store::ChunkBuilder& out, CancelToken cancel) override {
        const std::uint32_t count = store::chunkSampleCount(size_, chunkNo);
        if (count == 0) {
            return fail(ErrorCode::Invalid,
                        "text: chunk " + std::to_string(chunkNo) + " is outside " + name_);
        }
        Result<void> r = decoder_->materialise(target_, static_cast<std::uint64_t>(chunkNo), out, cancel);
        if (r) {
            return r;
        }
        if (r.error().code == ErrorCode::Cancelled) {
            return r;
        }
        if (log_ && !errorLogged_.exchange(true)) {
            ImportLogEntry::Context ctx;
            ctx.channel = name_;
            ctx.detail = r.error().toString();
            log_->add(ImportLogEntry::Level::Warning, "open.read_error",
                      "decoding chunk " + std::to_string(chunkNo) + " failed; the chunk is marked invalid",
                      std::move(ctx));
        }
        return out.allocateAllInvalid(count, dtype_);
    }

private:
    std::shared_ptr<SlabDecoder> decoder_;
    Target target_;
    SampleIdx size_ = 0;
    store::Dtype dtype_ = store::Dtype::F32;
    std::string name_;
    std::shared_ptr<DecodeLog> log_;
    std::atomic<bool> errorLogged_{false};
};

class TextReader final : public IReader {
public:
    TextReader(std::shared_ptr<const Layout> layout, ProbeResult probe, std::vector<ImportLogEntry> log,
               std::shared_ptr<const Dictionary> dictionary)
        : layout_(std::move(layout)), probe_(std::move(probe)), log_(std::move(log)),
          dictionary_(std::move(dictionary)), decodeLog_(std::make_shared<DecodeLog>()),
          conv_(std::make_shared<std::vector<Conversion>>()) {
        for (const text::ColumnPlan& c : layout_->columns) {
            conv_->push_back(Conversion{c.dtype, 1.0, 0.0});
        }
        decoder_ = std::make_shared<SlabDecoder>(layout_, conv_, decodeLog_);
        timesProviders_.resize(layout_->rowSets.size());
    }

    const ProbeResult& probe() const override { return probe_; }

    Result<void> readBlock(LayerIx layer, ChannelIx channel, std::uint64_t first, std::uint32_t count,
                           Block& out, CancelToken cancel) override {
        if (layer != 0 || channel >= layout_->columns.size()) {
            return fail(ErrorCode::NotFound, "text: no channel " + std::to_string(channel));
        }
        const text::ColumnPlan& col = layout_->columns[channel];
        const text::RowSet& rs = layout_->rowSets[col.rowSet];
        const Conversion conv = (*conv_)[channel];
        if (first + count > rs.samples || out.dtype != conv.dtype ||
            out.values.size() < std::size_t{count} * store::dtypeSize(conv.dtype)) {
            return fail(ErrorCode::Invalid, "text: bad block for '" + col.loggedName + "'");
        }
        const std::size_t width = store::dtypeSize(conv.dtype);
        std::uint64_t done = 0;
        while (done < count) {
            const std::uint64_t at = first + done;
            const auto chunk = static_cast<std::uint64_t>(store::chunkOf(static_cast<SampleIdx>(at)));
            const auto chunkFirst =
                static_cast<std::uint64_t>(store::chunkFirstSample(static_cast<SampleIdx>(chunk)));
            store::ChunkBuilder b;
            text::MemberOut m{channel, &b};
            if (auto r =
                    text::decodeChunk(*layout_, *conv_, col.rowSet, chunk, std::span<text::MemberOut>(&m, 1),
                                      nullptr, decodeLog_.get(), cancel);
                !r) {
                return r;
            }
            const std::uint64_t local = at - chunkFirst;
            const std::uint64_t n = std::min<std::uint64_t>(count - done, b.count() - local);
            std::memcpy(out.values.data() + done * width, b.sampleBytes().data() + local * width, n * width);
            const std::span<std::uint8_t> bits = b.validBits();
            for (std::uint64_t i = 0; i < n; ++i) {
                const std::uint64_t src = local + i;
                const bool valid = bits.empty() || ((bits[src >> 3] >> (src & 7)) & 1u) != 0;
                const std::uint64_t dst = done + i;
                if (!out.validBits.empty()) {
                    auto& byte = out.validBits[dst >> 3];
                    const auto mask = static_cast<std::uint8_t>(1u << (dst & 7));
                    byte = valid ? static_cast<std::uint8_t>(byte | mask)
                                 : static_cast<std::uint8_t>(byte & ~mask);
                }
            }
            done += n;
        }
        return ok();
    }

    Result<void> readMany(LayerIx layer, std::span<const ChannelIx> channels, std::uint64_t first,
                          std::uint32_t count, BlockSink& sink, const Progress& progress,
                          CancelToken cancel) override {
        std::size_t k = 0;
        for (const ChannelIx c : channels) {
            auto block = sink.begin(layer, c, first, count);
            if (!block) {
                return Failure{block.error()};
            }
            if (auto r = readBlock(layer, c, first, count, **block, cancel); !r) {
                return r;
            }
            sink.commit(*block);
            if (progress) {
                progress(static_cast<double>(++k) / static_cast<double>(channels.size()));
            }
        }
        return ok();
    }

    Result<void> readTimestamps(LayerIx layer, ChannelIx channel, std::uint64_t first, std::uint32_t count,
                                std::span<Micros> out, CancelToken cancel) override {
        if (layer != 0 || channel >= layout_->columns.size()) {
            return fail(ErrorCode::NotFound, "text: no channel " + std::to_string(channel));
        }
        const text::ColumnPlan& col = layout_->columns[channel];
        const text::RowSet& rs = layout_->rowSets[col.rowSet];
        if (first + count > rs.samples || out.size() < count) {
            return fail(ErrorCode::Invalid, "text: bad timestamp range for '" + col.loggedName + "'");
        }
        if (!rs.timestamped) {
            for (std::uint32_t i = 0; i < count; ++i) {
                out[i] = layout_->rate.timeAt(static_cast<SampleIdx>(first + i), layout_->t0Us);
            }
            return ok();
        }
        std::uint64_t done = 0;
        while (done < count) {
            const std::uint64_t at = first + done;
            const auto chunk = static_cast<std::uint64_t>(store::chunkOf(static_cast<SampleIdx>(at)));
            const auto chunkFirst =
                static_cast<std::uint64_t>(store::chunkFirstSample(static_cast<SampleIdx>(chunk)));
            store::ChunkBuilder b;
            if (auto r =
                    text::decodeChunk(*layout_, *conv_, col.rowSet, chunk, {}, &b, decodeLog_.get(), cancel);
                !r) {
                return r;
            }
            const std::uint64_t local = at - chunkFirst;
            const std::uint64_t n = std::min<std::uint64_t>(count - done, b.count() - local);
            const auto samples = b.samples<std::int64_t>();
            for (std::uint64_t i = 0; i < n; ++i) {
                out[done + i] = samples[local + i];
            }
            done += n;
        }
        return ok();
    }

    std::span<const MarkerInfo> markers(LayerIx layer) const override {
        if (layer >= probe_.layers.size()) {
            return {};
        }
        return probe_.layers[layer].markers;
    }
    std::optional<EnumTableInfo> enumTable(std::uint32_t id) const override {
        for (const EnumTableInfo& t : probe_.enumTables) {
            if (t.id == id) {
                return t;
            }
        }
        return std::nullopt;
    }
    std::vector<ImportLogEntry> warnings() const override { return log_; }
    std::shared_ptr<DecodeLog> decodeLog() const override { return decodeLog_; }
    bool chunkIndependent() const override { return true; }

    store::Interp interp(LayerIx layer, ChannelIx channel) const override {
        if (layer == 0 && channel < layout_->columns.size() && layout_->columns[channel].interp) {
            return *layout_->columns[channel].interp;
        }
        return IReader::interp(layer, channel);
    }

    std::vector<std::byte> formatHeader() const override {
        const auto n =
            static_cast<std::size_t>(std::min<std::uint64_t>(layout_->formatHeaderEnd, layout_->text.size()));
        std::vector<std::byte> out(n);
        const std::shared_ptr<const MappedFile> mapping =
            layout_->owned.empty() && layout_->source != nullptr ? layout_->source->mappedFile() : nullptr;
        if (!text::guardedCopy(mapping.get(), layout_->text, 0,
                               std::span<char>(reinterpret_cast<char*>(out.data()), out.size()))) {
            return {}; // the file faulted while it was read (D-190)
        }
        return out;
    }

    Result<void> setConversion(LayerIx layer, ChannelIx channel,
                               const ChannelConversion& conversion) override {
        if (layer != 0 || channel >= layout_->columns.size()) {
            return fail(ErrorCode::NotFound, "setConversion: no channel " + std::to_string(channel));
        }
        const text::ColumnPlan& col = layout_->columns[channel];
        if (col.kind == store::SampleKind::Enum && !conversion.toBase.identity()) {
            return fail(ErrorCode::Unsupported, "a text column keeps its enumeration ids");
        }
        if (conversion.dtype == store::Dtype::I64) {
            return fail(ErrorCode::Invalid, "setConversion: I64 is not a channel dtype");
        }
        (*conv_)[channel] = Conversion{conversion.dtype, conversion.toBase.a, conversion.toBase.b};
        return ok();
    }

    std::shared_ptr<store::IChunkProvider> makeChunkProvider(LayerIx layer,
                                                             ChannelIx channel) const override {
        if (layer != 0 || channel >= layout_->columns.size()) {
            return nullptr;
        }
        const text::ColumnPlan& col = layout_->columns[channel];
        return std::make_shared<TextChunkProvider>(
            decoder_, Target{false, channel}, static_cast<SampleIdx>(layout_->rowSets[col.rowSet].samples),
            (*conv_)[channel].dtype, probe_.importerId + ":" + col.loggedName, decodeLog_);
    }

    std::shared_ptr<store::IChunkProvider> makeTimesProvider(LayerIx layer,
                                                             ChannelIx channel) const override {
        if (layer != 0 || channel >= layout_->columns.size()) {
            return nullptr;
        }
        const std::uint32_t set = layout_->columns[channel].rowSet;
        const text::RowSet& rs = layout_->rowSets[set];
        if (!rs.timestamped) {
            return nullptr;
        }
        std::lock_guard lock(timesMutex_);
        if (!timesProviders_[set]) {
            timesProviders_[set] = std::make_shared<TextChunkProvider>(
                decoder_, Target{true, set}, static_cast<SampleIdx>(rs.samples), store::Dtype::I64,
                probe_.importerId + ":times/" + std::to_string(set), decodeLog_);
        }
        return timesProviders_[set];
    }

    std::optional<std::pair<Micros, Micros>> timeRange(LayerIx layer, ChannelIx channel) const override {
        if (layer != 0 || channel >= layout_->columns.size()) {
            return std::nullopt;
        }
        const text::RowSet& rs = layout_->rowSets[layout_->columns[channel].rowSet];
        if (!rs.timestamped) {
            return std::nullopt;
        }
        return std::make_pair(rs.samples > 0 ? rs.tFirst : 0, rs.samples > 0 ? rs.tLast : -1);
    }

    const Dictionary::Entry* columnHint(LayerIx layer, ChannelIx channel) const override {
        if (layer != 0 || channel >= layout_->columns.size() || !layout_->columns[channel].hint) {
            return nullptr;
        }
        return &*layout_->columns[channel].hint;
    }
    const Dictionary* dictionary() const override { return dictionary_.get(); }

    void attachColumns(LayerIx layer, ChannelIx channel, const store::Column& values,
                       const store::Column* timesUs, store::ChunkCache& cache) override {
        if (layer == 0) {
            decoder_->attach(channel, values, timesUs, cache);
        }
    }

private:
    std::shared_ptr<const Layout> layout_;
    ProbeResult probe_;
    std::vector<ImportLogEntry> log_;
    std::shared_ptr<const Dictionary> dictionary_;
    std::shared_ptr<DecodeLog> decodeLog_;
    std::shared_ptr<std::vector<Conversion>> conv_;
    std::shared_ptr<SlabDecoder> decoder_;
    mutable std::mutex timesMutex_;
    mutable std::vector<std::shared_ptr<store::IChunkProvider>> timesProviders_;
};

std::string lowerExtension(const std::filesystem::path& p) {
    std::string e = pathToUtf8(p.extension());
    for (char& c : e) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return e;
}

// The head as UTF-8 text for the sniff regex: BOMs skipped, UTF-16 transcoded, at most kMaxRegexSubject
// bytes and the first kSniffLines lines (D-190: a pattern is tried at every line start, so the lines bound
// the work as the bytes do).
constexpr std::size_t kSniffLines = 16;
std::string sniffSubject(std::span<const std::byte> head) {
    const text::Enc enc = text::detectEncoding(head);
    const std::span<const std::byte> cut =
        head.first(std::min<std::size_t>(head.size(), 2 * text::kMaxRegexSubject));
    std::string s;
    if (enc == text::Enc::Utf16Le || enc == text::Enc::Utf16Be) {
        s = text::utf16ToUtf8(cut.subspan(2), enc == text::Enc::Utf16Be);
    } else {
        const std::size_t skip = enc == text::Enc::Utf8Bom ? 3 : 0;
        s.assign(reinterpret_cast<const char*>(cut.data()) + skip, cut.size() - skip);
    }
    if (s.size() > text::kMaxRegexSubject) {
        s.resize(text::kMaxRegexSubject);
    }
    // Cut after the kSniffLines-th line end ("\r\n", "\n" or "\r").
    std::size_t lines = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n' || (s[i] == '\r' && (i + 1 >= s.size() || s[i + 1] != '\n'))) {
            if (++lines == kSniffLines) {
                s.resize(i + 1);
                break;
            }
        }
    }
    return s;
}

// Text that is not UTF-16: no NUL byte in the first 4 KiB.
bool looksBinary(std::span<const std::byte> head) {
    const text::Enc enc = text::detectEncoding(head);
    if (enc == text::Enc::Utf16Le || enc == text::Enc::Utf16Be) {
        return false;
    }
    const std::size_t n = std::min<std::size_t>(head.size(), 4096);
    for (std::size_t i = 0; i < n; ++i) {
        if (std::to_integer<std::uint8_t>(head[i]) == 0) {
            return true;
        }
    }
    return false;
}

// A delimited table: at least three lines with the same, non-zero count of one delimiter.
bool looksDelimited(std::span<const std::byte> head) {
    const std::string s = sniffSubject(head);
    std::vector<std::string_view> lines;
    text::LineCursor cur(s, 0, text::isCrOnly(s));
    std::string_view line;
    while (lines.size() < 16 && cur.next(line)) {
        if (!text::isBlank(line)) {
            lines.push_back(line);
        }
    }
    if (lines.size() < 3) {
        return false;
    }
    for (const char d : {',', ';', '\t', '|'}) {
        const std::size_t last = text::countDelimiters(lines.back(), d, true, '"');
        if (last == 0) {
            continue;
        }
        std::size_t same = 0;
        for (const std::string_view l : lines) {
            same += text::countDelimiters(l, d, true, '"') == last ? 1 : 0;
        }
        if (same >= 3) {
            return true;
        }
    }
    return false;
}

class TextImporter final : public IImporter {
public:
    TextImporter(TextProfile profile, std::shared_ptr<const Dictionary> dictionary)
        : profile_(std::move(profile)), id_(profile_.effectiveImporterId()),
          dictionary_(std::move(dictionary)) {
        extensions_ = profile_.sniff.extensions;
        if (!profile_.sniff.preambleRegex.empty()) {
            sniffRegex_ = text::compileRegex(profile_.sniff.preambleRegex);
            sniffAnchored_ = profile_.sniff.preambleRegex.front() == '^';
        }
    }

    std::string_view id() const override { return id_; }
    std::string_view version() const override { return profile_.version; }
    std::span<const std::string> extensions() const override { return extensions_; }

    SniffResult sniff(const SniffInput& in) const override {
        SniffResult r;
        const std::string ext = lowerExtension(in.path);
        const bool extOk = std::find(extensions_.begin(), extensions_.end(), ext) != extensions_.end();
        if (in.head.empty() || looksBinary(in.head)) {
            r.reason = "not text";
            return r;
        }
        const int declared = profile_.sniff.confidence;
        if (sniffRegex_) {
            if (text::regexSearchLines(*sniffRegex_, sniffSubject(in.head), sniffAnchored_)) {
                r.confidence = declared;
                r.reason = "the first lines match the " + profile_.id + " profile";
            } else if (extOk) {
                r.confidence = std::min(declared, 20);
                r.reason = "extension " + ext + " only";
            }
            return r;
        }
        if (extOk) {
            r.confidence = std::min(declared, 49);
            r.reason = "extension " + ext;
        } else if (looksDelimited(in.head)) {
            r.confidence = std::min(declared, 10);
            r.reason = "delimited text";
        }
        return r;
    }

    Result<ProbeResult> probe(const ByteSource& source) const override {
        const std::shared_ptr<const MappedFile> mapping = source.mappedFile();
        auto a =
            text::analyse(profile_, source.span(0, source.size()), id_, profile_.version, {}, mapping.get());
        if (!a) {
            return Failure{a.error()};
        }
        return std::move(a->probe);
    }

    Result<std::unique_ptr<IReader>> open(std::shared_ptr<const ByteSource> source,
                                          const ImportOptions& options) const override {
        if (!source) {
            return fail(ErrorCode::Invalid, "text: no source");
        }
        const std::shared_ptr<const MappedFile> mapping = source->mappedFile();
        auto a = text::analyse(profile_, source->span(0, source->size()), id_, profile_.version,
                               options.cancel, mapping.get());
        if (!a) {
            return Failure{a.error()};
        }
        a->layout->source = std::move(source);
        std::shared_ptr<const Dictionary> dict = options.applyDictionary ? dictionary_ : nullptr;
        std::unique_ptr<IReader> reader = std::make_unique<TextReader>(
            std::move(a->layout), std::move(a->probe), std::move(a->log), std::move(dict));
        return reader;
    }

private:
    TextProfile profile_;
    std::string id_;
    std::vector<std::string> extensions_;
    std::shared_ptr<const Dictionary> dictionary_;
    std::optional<std::regex> sniffRegex_;
    bool sniffAnchored_ = false;
};

} // namespace

std::unique_ptr<IImporter> TextEngine::makeImporter(TextProfile profile,
                                                    std::shared_ptr<const Dictionary> dictionary) {
    return std::make_unique<TextImporter>(std::move(profile), std::move(dictionary));
}

std::vector<TextProfile> TextEngine::loadProfiles(const std::filesystem::path& dir,
                                                  std::vector<std::string>& log) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && lowerExtension(it->path()) == ".json") {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return pathToUtf8(a.filename()) < pathToUtf8(b.filename());
    });
    std::vector<TextProfile> out;
    for (const std::filesystem::path& f : files) {
        auto p = TextProfile::load(f, log);
        if (!p) {
            log.push_back(pathToUtf8(f) + ": skipped: " + p.error().message);
            continue;
        }
        out.push_back(std::move(p).value());
    }
    return out;
}

} // namespace esses::formats
