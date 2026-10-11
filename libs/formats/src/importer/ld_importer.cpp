// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// The motec.ld IImporter and its IReader (05 sections 1.3, 3 and 4) over the .ld/.ldx core of src/ld/.
#include "importer/ld_support.hpp"
#include "ld/ld_chunk_provider.hpp"
#include "ld/ld_codes.hpp"

#include <esses/base/byte_reader.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/ld_importer.hpp>
#include <esses/units/unit_registry.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>
#include <span>
#include <string>
#include <utility>

namespace esses::formats {

LdSourceEncoding::LdSourceEncoding(LdEncoding encoding, std::shared_ptr<const Origin> origin)
    : encoding_(std::move(encoding)), origin_(std::move(origin)) {}

namespace {

using namespace ldimpl;
namespace L = ld::layout;
using json = nlohmann::json;

constexpr std::string_view kOptionsSchema = R"({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "motec.ld importer options",
  "type": "object",
  "properties": {
    "ld": {
      "type": "object",
      "properties": {
        "shiftOrder": {"enum": ["afterMul", "beforeMul"]},
        "enumScanFallback": {"type": "boolean"},
        "gapConvention": {"enum": ["standard", "shifted"]}
      },
      "additionalProperties": false
    }
  },
  "additionalProperties": false
})";

struct LdOpenOptions {
    ld::ShiftOrder shiftOrder = ld::ShiftOrder::AfterMul;
    bool enumScanFallback = true;
    ld::GapConvention gapConvention = ld::GapConvention::Standard;
};

Result<LdOpenOptions> parseOptions(const model::OpaqueJson& options) {
    LdOpenOptions out;
    if (options.isNull()) {
        return out;
    }
    const auto invalid = [](const std::string& pointer, const std::string& message) {
        ErrorContext ctx;
        ctx.detail = pointer;
        ctx.importer = std::string(kLdImporterId);
        return fail(ErrorCode::Invalid, "importer options: " + message + " at '" + pointer + "'",
                    std::move(ctx));
    };
    const json j = json::parse(options.compact(), nullptr, false);
    if (!j.is_object()) {
        return invalid("", "expected an object");
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() != "ld") {
            return invalid("/" + it.key(), "unknown option");
        }
        if (!it->is_object()) {
            return invalid("/ld", "expected an object");
        }
        for (auto o = it->begin(); o != it->end(); ++o) {
            const std::string pointer = "/ld/" + o.key();
            if (o.key() == "shiftOrder") {
                if (*o == "afterMul") {
                    out.shiftOrder = ld::ShiftOrder::AfterMul;
                } else if (*o == "beforeMul") {
                    out.shiftOrder = ld::ShiftOrder::BeforeMul;
                } else {
                    return invalid(pointer, R"(expected "afterMul" or "beforeMul")");
                }
            } else if (o.key() == "enumScanFallback") {
                if (!o->is_boolean()) {
                    return invalid(pointer, "expected a boolean");
                }
                out.enumScanFallback = o->get<bool>();
            } else if (o.key() == "gapConvention") {
                if (*o == "standard") {
                    out.gapConvention = ld::GapConvention::Standard;
                } else if (*o == "shifted") {
                    out.gapConvention = ld::GapConvention::Shifted;
                } else {
                    return invalid(pointer, R"(expected "standard" or "shifted")");
                }
            } else {
                return invalid(pointer, "unknown option");
            }
        }
    }
    return out;
}

// Clears the valid bit (LSB first) of every NaN among the first `count` values of an F32/F64 block.
void clearNaNBits(store::Dtype dtype, std::span<const std::byte> values, std::uint32_t count,
                  std::span<std::uint8_t> bits) {
    for (std::uint32_t k = 0; k < count; ++k) {
        const bool nan = dtype == store::Dtype::F32
                             ? std::isnan(loadAs<float>(values.data() + std::size_t{4} * k, Endian::Little))
                             : std::isnan(loadAs<double>(values.data() + std::size_t{8} * k, Endian::Little));
        if (nan) {
            bits[k >> 3] &= static_cast<std::uint8_t>(~(1u << (k & 7u)));
        }
    }
}

std::string hex(std::uint64_t v) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string s;
    do {
        s.insert(s.begin(), kDigits[v & 0xFu]);
        v >>= 4;
    } while (v != 0);
    return "0x" + s;
}

std::string lowerExtension(const std::filesystem::path& p) {
    std::string e = pathToUtf8(p.extension());
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

ld::LdOptions coreOptions(const LdOpenOptions& o, const units::UnitRegistry* units) {
    ld::LdOptions lo;
    lo.shiftOrder = o.shiftOrder;
    lo.enumScanFallback = o.enumScanFallback;
    lo.gapConvention = o.gapConvention;
    if (units != nullptr) {
        lo.unitResolves = [units](std::string_view s) { return units->find(s).has_value(); };
    }
    return lo;
}

Result<std::shared_ptr<const ld::LdFile>> parseFile(const ByteSource& source, ld::LdOptions options) {
    if (auto mapped = source.mappedFile()) {
        return ld::LdFile::fromMapped(std::move(mapped), std::move(options));
    }
    if (auto buffer = source.buffer()) {
        return ld::LdFile::fromBuffer(std::move(buffer), std::move(options));
    }
    const auto all = source.span(0, source.size()); // another ByteSource implementation: one copy
    return ld::LdFile::fromBuffer(std::make_shared<const std::vector<std::byte>>(all.begin(), all.end()),
                                  std::move(options));
}

ChannelInfo channelInfoOf(const ld::LdChannel& c) {
    ChannelInfo ci;
    ci.index = c.index;
    ci.loggedName = c.loggedName;
    ci.shortName = c.shortName;
    ci.vendorUnit = c.unit;
    if (!c.vendorId.empty()) {
        ci.vendorId = c.vendorId;
    }
    ci.vendorDtype = std::string(c.vendorDtype());
    ci.dtype = c.dtype;
    ci.kind = c.kind;
    ci.timing = c.rate();
    ci.sampleCount = c.sampleCount;
    ci.t0Us = 0;
    ci.dps = std::max(0, c.dps);
    if (c.enumResolved && c.enumRef) {
        ci.enumTable = *c.enumRef;
    }
    ci.flags = c.flags;
    return ci;
}

MarkerInfo deviceEventInfo(const ld::LdDeviceEvent& e) {
    return MarkerInfo{model::MarkerClass::DeviceEvent,
                      e.tUs,
                      model::deviceEventName(e.code),
                      std::string(model::kGroupDeviceEvents),
                      {},
                      e.code,
                      e.ordinal};
}

// The parsed .ld with what its companion says.
struct Parsed {
    std::shared_ptr<const ld::LdFile> file;
    std::vector<std::filesystem::path> companions;
    std::optional<ld::LdxDocument> ldx;
    ImportLog log;
};

Result<Parsed> parse(const ByteSource& source, const LdOpenOptions& options,
                     const units::UnitRegistry* units) {
    auto file = parseFile(source, coreOptions(options, units));
    if (!file) {
        Error e = file.error();
        if (!e.context.path && !source.path().empty()) {
            e.context.path = source.path();
        }
        e.context.importer = std::string(kLdImporterId);
        return Failure{std::move(e)};
    }
    Parsed p;
    p.file = std::move(file).value();
    p.log = p.file->log();
    const std::vector<std::filesystem::path> siblings = source.siblings();
    if (const auto ldx = findCompanion(siblings, source.path(), ".ldx")) {
        p.companions.push_back(*ldx);
        auto doc = ld::readLdxFile(*ldx, p.log, p.file->durationUs());
        if (doc) {
            p.ldx = std::move(doc).value(); // a malformed .ldx logged ldx.parse_error; the .ld opens anyway
        }
    }
    return p;
}

ProbeResult buildProbe(const Parsed& p) {
    const ld::LdFile& f = *p.file;
    ProbeResult r;
    r.importerId = std::string(kLdImporterId);
    r.importerVersion = std::string(kLdImporterVersion);
    r.dialect = f.dialectString();
    r.details = detailsOf(f);
    LayerInfo layer;
    layer.name = std::string(model::kNormalLayer);
    layer.durationUs = f.durationUs();
    layer.utc = utcOf(f.header());
    layer.utcAssumedLocal = layer.utc.has_value();
    for (const ld::LdChannel& c : f.channels()) {
        layer.channels.push_back(channelInfoOf(c));
    }
    for (const ld::LdDeviceEvent& e : f.deviceEvents()) {
        layer.markers.push_back(deviceEventInfo(e));
    }
    if (p.ldx) {
        for (const ld::LdxDetail& d : p.ldx->details) {
            // Companion provenance in memory (not serialised): the pipeline takes the file's details from
            // the probe and the companion's from companion() (05 section 2 step 5).
            r.details.push_back(model::Detail{d.id, d.value, model::Provenance::Companion, std::nullopt});
        }
        for (const ld::LdxMarker& m : p.ldx->markers) {
            const auto cls = model::parseMarkerClass(m.className);
            if (!cls) {
                continue;
            }
            layer.markers.push_back(
                MarkerInfo{*cls,
                           m.tUs,
                           m.name,
                           m.group < p.ldx->groups.size() ? p.ldx->groups[m.group].name : std::string(),
                           {},
                           m.flagsValue,
                           m.ordinal});
        }
        // Device events first, then the companion's markers, each in stored order: one time order.
        std::stable_sort(layer.markers.begin(), layer.markers.end(),
                         [](const MarkerInfo& a, const MarkerInfo& b) { return a.tUs < b.tUs; });
    }
    r.layers.push_back(std::move(layer));
    for (const ld::LdEnumTable& t : f.enumTables()) {
        EnumTableInfo info;
        info.id = t.index;
        for (const ld::LdEnumEntry& e : t.entries) {
            info.entries.push_back(model::EnumEntry{
                e.value, e.label, static_cast<std::uint8_t>(std::min<std::uint16_t>(e.severity, 3))});
        }
        r.enumTables.push_back(std::move(info));
    }
    for (const auto& c : p.companions) {
        r.companions.push_back(c.filename());
    }
    r.warnings = p.log.snapshot();
    return r;
}

class LdReader final : public IReader {
public:
    explicit LdReader(Parsed parsed)
        : file_(std::move(parsed.file)), log_(parsed.log), // ImportLog copies (it has no move constructor)
          decodeLog_(std::make_shared<DecodeLog>()) {
        Parsed view;
        view.file = file_;
        view.companions = parsed.companions;
        view.ldx = parsed.ldx;
        view.log = log_;
        probe_ = buildProbe(view);
        for (const ld::LdDeviceEvent& e : file_->deviceEvents()) {
            deviceMarkers_.push_back(deviceEventInfo(e));
        }
        if (parsed.ldx) {
            companion_ = companionFromLdx(*parsed.ldx);
        }
        conversions_.resize(file_->channels().size());
    }

    const ProbeResult& probe() const override { return probe_; }

    Result<void> readBlock(LayerIx layer, ChannelIx channel, std::uint64_t first, std::uint32_t count,
                           Block& out, CancelToken cancel) override {
        if (auto r = check(layer, channel); !r) {
            return r;
        }
        if (cancel.cancelled()) {
            return fail(ErrorCode::Cancelled, "readBlock cancelled");
        }
        const ld::LdChannel& c = file_->channels()[channel];
        if (first > c.sampleCount || count > c.sampleCount - first) {
            return fail(ErrorCode::Invalid, "readBlock: range beyond the channel's " +
                                                std::to_string(c.sampleCount) + " samples");
        }
        const std::optional<ld::LdConversion>& conv = conversions_[channel];
        const store::Dtype dtype = conv ? conv->dtype : c.dtype;
        if (out.dtype != dtype) {
            return fail(ErrorCode::Invalid, "readBlock: the block's dtype is " +
                                                std::string(store::dtypeName(out.dtype)) +
                                                ", the channel's " + std::string(store::dtypeName(dtype)));
        }
        if (out.values.size() < std::size_t{count} * store::dtypeSize(dtype) ||
            (!out.validBits.empty() && out.validBits.size() < (std::size_t{count} + 7) / 8)) {
            return fail(ErrorCode::Invalid, "readBlock: the block is too small");
        }
        const auto validity = file_->validity(channel, first, count);
        if (validity != ld::LdFile::RangeValidity::AllValid && out.validBits.empty() &&
            !store::isFloatDtype(dtype)) {
            return fail(ErrorCode::Invalid, "readBlock: invalid samples in an integer range need validBits");
        }
        if (auto r = ld::decodeConverted(*file_, channel, first, count, dtype, conv, out.values); !r) {
            return r;
        }
        if (!out.validBits.empty()) {
            const std::span<std::uint8_t> bits(out.validBits.data(), (std::size_t{count} + 7) / 8);
            std::fill(bits.begin(), bits.end(), std::uint8_t{0xFF});
            // Only the intervals that touch the range (binary search), not every interval of the channel.
            file_->clearInvalidBytes(channel, first, count, bits);
            // A NaN stored in a float channel is invalid too (S-1 D13), as the chunk path's seal() decides.
            if (store::isFloatDtype(dtype)) {
                clearNaNBits(dtype, out.values, count, bits);
            }
        }
        if (validity != ld::LdFile::RangeValidity::AllValid) {
            file_->writeInvalidNaN(channel, first, count, dtype, out.values);
        }
        return ok();
    }

    Result<void> readMany(LayerIx layer, std::span<const ChannelIx> channels, std::uint64_t first,
                          std::uint32_t count, BlockSink& sink, const Progress& progress,
                          CancelToken cancel) override {
        for (std::size_t k = 0; k < channels.size(); ++k) {
            auto block = sink.begin(layer, channels[k], first, count);
            if (!block) {
                return Failure{block.error()};
            }
            auto r = readBlock(layer, channels[k], first, count, **block, cancel);
            sink.commit(*block);
            if (!r) {
                return r;
            }
            if (progress) {
                progress(static_cast<double>(k + 1) / static_cast<double>(channels.size()));
            }
        }
        return ok();
    }

    Result<void> readTimestamps(LayerIx, ChannelIx, std::uint64_t, std::uint32_t, std::span<Micros>,
                                CancelToken) override {
        return fail(ErrorCode::Unsupported,
                    "every .ld channel is uniformly sampled; there are no timestamps");
    }

    std::span<const MarkerInfo> markers(LayerIx layer) const override {
        return layer == 0 ? std::span<const MarkerInfo>(deviceMarkers_) : std::span<const MarkerInfo>();
    }

    std::optional<EnumTableInfo> enumTable(std::uint32_t id) const override {
        for (const EnumTableInfo& t : probe_.enumTables) {
            if (t.id == id) {
                return t;
            }
        }
        return std::nullopt;
    }

    std::vector<ImportLogEntry> warnings() const override { return log_.snapshot(); }
    std::shared_ptr<DecodeLog> decodeLog() const override { return decodeLog_; }
    bool chunkIndependent() const override { return true; }
    std::vector<std::byte> rawHeader() const override { return packRawHeader(*file_); }

    std::shared_ptr<const model::SourceEncoding> sourceEncoding(LayerIx layer,
                                                                ChannelIx channel) const override {
        if (!check(layer, channel)) {
            return nullptr;
        }
        auto origin = std::make_shared<LdSourceEncoding::Origin>();
        origin->file = file_;
        origin->channel = channel;
        return std::make_shared<const LdSourceEncoding>(encodingOf(file_->channels()[channel]),
                                                        std::move(origin));
    }

    store::Interp interp(LayerIx layer, ChannelIx channel) const override {
        return check(layer, channel) ? file_->channels()[channel].interp : store::Interp::Linear;
    }

    model::CompanionData companion() const override { return companion_; }

    std::vector<std::byte> formatHeader() const override {
        std::vector<std::byte> out(file_->rawHeader().begin(), file_->rawHeader().end());
        for (const ld::LdChannel& c : file_->channels()) {
            out.insert(out.end(), c.enc.raw.begin(), c.enc.raw.end());
        }
        return out;
    }

    Result<void> setConversion(LayerIx layer, ChannelIx channel,
                               const ChannelConversion& conversion) override {
        if (auto r = check(layer, channel); !r) {
            return r;
        }
        const ld::LdChannel& c = file_->channels()[channel];
        if (conversion.toBase.identity() && conversion.dtype == c.dtype) {
            conversions_[channel].reset();
            return ok();
        }
        if (!store::isFloatDtype(conversion.dtype)) {
            return fail(ErrorCode::Invalid, "setConversion: a converted channel is stored as F32 or F64");
        }
        conversions_[channel] = ld::LdConversion{conversion.toBase.a, conversion.toBase.b, conversion.dtype};
        return ok();
    }

    std::shared_ptr<store::IChunkProvider> makeChunkProvider(LayerIx layer,
                                                             ChannelIx channel) const override {
        if (!check(layer, channel)) {
            return nullptr;
        }
        return std::make_shared<ld::LdChunkProvider>(file_, channel, decodeLog_, conversions_[channel]);
    }

private:
    Result<void> check(LayerIx layer, ChannelIx channel) const {
        if (layer != 0 || channel >= file_->channels().size()) {
            return fail(ErrorCode::NotFound, "no channel " + std::to_string(channel) + " in layer " +
                                                 std::to_string(layer) + " of the .ld");
        }
        return ok();
    }

    std::shared_ptr<const ld::LdFile> file_;
    ImportLog log_;
    std::shared_ptr<DecodeLog> decodeLog_;
    ProbeResult probe_;
    std::vector<MarkerInfo> deviceMarkers_;
    model::CompanionData companion_;
    std::vector<std::optional<ld::LdConversion>> conversions_;
};

class LdImporter final : public IImporter {
public:
    explicit LdImporter(const units::UnitRegistry* units) : units_(units) {}

    std::string_view id() const override { return kLdImporterId; }
    std::string_view version() const override { return kLdImporterVersion; }
    std::span<const std::string> extensions() const override { return extensions_; }

    SniffResult sniff(const SniffInput& in) const override {
        SniffResult r;
        const bool extLd = lowerExtension(in.path) == ".ld";
        const auto u32 = [&](std::uint64_t off) -> std::uint32_t {
            if (off + 4 > in.head.size()) {
                return 0;
            }
            std::uint32_t v = 0;
            for (int i = 0; i < 4; ++i) {
                v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in.head[off + i])) << (8 * i);
            }
            return v;
        };
        const bool magic = in.head.size() >= 4 && u32(L::kHdrMagic) == L::kMagic;
        if (!magic) {
            if (extLd) {
                r.confidence = 25;
                r.reason = in.head.size() < 4 ? "extension .ld, but the file is shorter than 4 bytes"
                                              : "extension .ld, but the u32 at 0x00 is not the magic 0x40";
            } else {
                r.reason = "u32 at 0x00 is not the magic 0x40";
            }
        } else {
            std::string why;
            const std::uint64_t size = in.size;
            const std::uint64_t meta = u32(L::kHdrMetaPtr);
            const std::uint64_t data = u32(L::kHdrDataPtr);
            const std::uint64_t event = u32(L::kHdrEventPtr);
            const bool headerOnly = size >= L::kHeaderSize && meta == size && data == size;
            const auto inFile = [&](std::uint64_t p) { return p >= L::kHeaderSize && p < size; };
            if (size < L::kHeaderSize || in.head.size() < L::kHeaderSize) {
                why = "the file is shorter than the 1,762-byte header";
            } else if (!headerOnly && !(inFile(meta) && inFile(data))) {
                why = "meta_ptr " + hex(meta) + " or data_ptr " + hex(data) + " is outside the file";
            } else if (event != 0 &&
                       !(event >= L::kHeaderSize && size >= L::kEvUsed && event <= size - L::kEvUsed)) {
                why = "event_ptr " + hex(event) + " leaves no room for the event record";
            } else if (!headerOnly && meta + L::kRecordSize <= in.head.size()) {
                const std::uint64_t prev = u32(meta + L::kRecPrev);
                const std::uint64_t next = u32(meta + L::kRecNext);
                if (prev != 0) {
                    why = "the first channel record has a previous pointer " + hex(prev);
                } else if (next != 0 && !(next >= L::kHeaderSize && next <= size - L::kRecordSize)) {
                    why = "the first channel record's next pointer " + hex(next) + " is outside the file";
                }
            }
            if (why.empty()) {
                r.confidence = 95;
                r.reason = headerOnly ? "magic 0x40, header only (no channel list)"
                                      : "magic 0x40, pointers plausible";
            } else {
                r.confidence = extLd ? 60 : 50;
                r.reason = "magic 0x40, but " + why;
            }
        }
        if (r.confidence > 0) {
            if (const auto ldx = findCompanion(in.siblings, in.path, ".ldx")) {
                r.companions.push_back(*ldx);
            }
        }
        return r;
    }

    Result<ProbeResult> probe(const ByteSource& source) const override {
        auto parsed = parse(source, LdOpenOptions{}, units_);
        if (!parsed) {
            return Failure{parsed.error()};
        }
        return buildProbe(*parsed);
    }

    Result<std::unique_ptr<IReader>> open(std::shared_ptr<const ByteSource> source,
                                          const ImportOptions& options) const override {
        if (!source) {
            return fail(ErrorCode::Invalid, "motec.ld open: no source");
        }
        auto opts = parseOptions(options.options);
        if (!opts) {
            return Failure{opts.error()};
        }
        auto parsed = parse(*source, *opts, units_);
        if (!parsed) {
            return Failure{parsed.error()};
        }
        return std::unique_ptr<IReader>(std::make_unique<LdReader>(std::move(parsed).value()));
    }

    std::optional<model::OpaqueJson> optionsSchema() const override {
        auto j = model::OpaqueJson::parse(kOptionsSchema);
        return j ? std::optional<model::OpaqueJson>(std::move(j).value()) : std::nullopt;
    }

private:
    const units::UnitRegistry* units_;
    std::array<std::string, 1> extensions_{".ld"};
};

} // namespace

std::unique_ptr<IImporter> makeLdImporter(const units::UnitRegistry* units) {
    return std::make_unique<LdImporter>(units);
}

} // namespace esses::formats
