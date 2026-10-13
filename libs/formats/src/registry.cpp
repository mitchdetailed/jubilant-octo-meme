// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ImporterRegistry and the open pipeline (05 section 2, M0 scope in registry.hpp).
#include "importer_chunk_provider.hpp"

#include <esses/base/assert.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/ld_importer.hpp>
#include <esses/formats/registry.hpp>
#include <esses/model/whole_outing.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/store/series.hpp>
#include <esses/units/unit_registry.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <system_error>
#include <utility>

namespace esses::formats {
namespace {

using json = nlohmann::json;
using Level = ImportLogEntry::Level;

Level levelOf(model::ModelNote::Level l) {
    switch (l) {
    case model::ModelNote::Level::Info:
        return Level::Info;
    case model::ModelNote::Level::Warning:
        return Level::Warning;
    case model::ModelNote::Level::Error:
        break;
    }
    return Level::Error;
}

void addNotes(ImportLog& log, const std::vector<model::ModelNote>& notes) {
    for (const model::ModelNote& n : notes) {
        ImportLogEntry e;
        e.level = levelOf(n.level);
        e.code = n.code;
        e.message = n.message;
        if (!n.layer.empty()) {
            e.ctx.layer = n.layer;
        }
        if (!n.ref.empty()) {
            e.ctx.detail = n.ref;
        }
        log.add(std::move(e));
    }
}

std::vector<std::string> splitDialect(std::string_view d) {
    std::vector<std::string> out;
    while (!d.empty()) {
        const std::size_t comma = d.find(',');
        const std::string_view part = d.substr(0, comma);
        if (!part.empty()) {
            out.emplace_back(part);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        d.remove_prefix(comma + 1);
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

// The dtype a converted channel is stored in: floats keep theirs; an integer channel becomes F64 when its
// stored width is 32 bits or more (exact), else F32.
store::Dtype convertedDtype(const ChannelInfo& c) {
    if (c.dtype == store::Dtype::F64) {
        return store::Dtype::F64;
    }
    if (c.dtype == store::Dtype::I32) {
        const bool wide =
            c.vendorDtype.find("32") != std::string::npos || c.vendorDtype.find("64") != std::string::npos;
        return wide ? store::Dtype::F64 : store::Dtype::F32;
    }
    return store::Dtype::F32;
}

bool hasIntrinsic(const std::vector<model::Marker>& markers) {
    return std::any_of(markers.begin(), markers.end(), [](const model::Marker& m) {
        return m.cls == model::MarkerClass::SOL || m.cls == model::MarkerClass::EOL;
    });
}

struct UnresolvedUnit {
    std::uint32_t count = 0;
    std::string firstChannel;
    std::string layer;
};

} // namespace

ImporterRegistry::~ImporterRegistry() = default;

std::unique_ptr<ImporterRegistry> ImporterRegistry::withBuiltins(const units::UnitRegistry* units) {
    auto r = std::make_unique<ImporterRegistry>();
    r->add(makeLdImporter(units));
    r->add(makeLdWriter(units));
    return r;
}

void ImporterRegistry::add(std::unique_ptr<IImporter> importer) {
    if (!importer) {
        return;
    }
    ESSES_ASSERT_MSG(find(importer->id()) == nullptr, "ImporterRegistry: duplicate importer id");
    importers_.push_back(std::move(importer));
}

void ImporterRegistry::add(std::unique_ptr<IWriter> writer) {
    if (!writer) {
        return;
    }
    ESSES_ASSERT_MSG(this->writer(writer->id()) == nullptr, "ImporterRegistry: duplicate writer id");
    writers_.push_back(std::move(writer));
}

std::vector<std::pair<const IImporter*, SniffResult>> ImporterRegistry::sniff(const SniffInput& input) const {
    std::vector<std::pair<const IImporter*, SniffResult>> out;
    for (const auto& imp : importers_) {
        SniffResult r = imp->sniff(input);
        if (r.confidence > 0) {
            out.emplace_back(imp.get(), std::move(r));
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const auto& a, const auto& b) { return a.second.confidence > b.second.confidence; });
    return out;
}

Result<std::vector<std::pair<const IImporter*, SniffResult>>>
ImporterRegistry::sniff(const std::filesystem::path& path) const {
    auto st = statFile(path);
    if (!st) {
        return Failure{st.error()};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ErrorContext ctx;
        ctx.path = path;
        return fail(ErrorCode::Io, "cannot read the file", std::move(ctx));
    }
    std::vector<std::byte> head(static_cast<std::size_t>(std::min<std::uint64_t>(st->size, kSniffHeadBytes)));
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    const std::vector<std::filesystem::path> siblings = listSiblings(path);
    SniffInput si;
    si.path = path;
    si.head = head;
    si.size = st->size;
    si.siblings = siblings;
    return sniff(si);
}

const IImporter* ImporterRegistry::find(std::string_view id) const {
    for (const auto& imp : importers_) {
        if (imp->id() == id) {
            return imp.get();
        }
    }
    return nullptr;
}

const IWriter* ImporterRegistry::writer(std::string_view id) const {
    for (const auto& w : writers_) {
        if (w->id() == id) {
            return w.get();
        }
    }
    return nullptr;
}

IWriter* ImporterRegistry::writer(std::string_view id) {
    for (const auto& w : writers_) {
        if (w->id() == id) {
            return w.get();
        }
    }
    return nullptr;
}

std::vector<const IImporter*> ImporterRegistry::importers() const {
    std::vector<const IImporter*> out;
    out.reserve(importers_.size());
    for (const auto& imp : importers_) {
        out.push_back(imp.get());
    }
    return out;
}

std::vector<const IWriter*> ImporterRegistry::writers() const {
    std::vector<const IWriter*> out;
    out.reserve(writers_.size());
    for (const auto& w : writers_) {
        out.push_back(w.get());
    }
    return out;
}

model::OpaqueJson ImporterRegistry::catalogue() const {
    json importers = json::array();
    for (const auto& imp : importers_) {
        json j = json::object();
        j["id"] = std::string(imp->id());
        j["version"] = std::string(imp->version());
        json ext = json::array();
        for (const std::string& e : imp->extensions()) {
            ext.push_back(e);
        }
        j["extensions"] = std::move(ext);
        if (const auto schema = imp->optionsSchema()) {
            j["optionsSchema"] = json::parse(schema->compact(), nullptr, false);
        }
        importers.push_back(std::move(j));
    }
    json writers = json::array();
    for (const auto& w : writers_) {
        json j = json::object();
        j["id"] = w->id();
        j["extensions"] = w->extensions();
        if (const auto schema = w->optionsSchema()) {
            j["optionsSchema"] = json::parse(schema->compact(), nullptr, false);
        }
        writers.push_back(std::move(j));
    }
    auto out = model::OpaqueJson::parse(json{{"importers", importers}, {"writers", writers}}.dump());
    return out ? std::move(out).value() : model::OpaqueJson();
}

Result<std::pair<const IImporter*, ProbeResult>> ImporterRegistry::probe(const std::filesystem::path& path,
                                                                         const ImportOptions& options) const {
    const IImporter* imp = nullptr;
    if (!options.importerId.empty()) {
        imp = find(options.importerId);
        if (imp == nullptr) {
            ErrorContext ctx;
            ctx.importer = options.importerId;
            return fail(ErrorCode::NotFound, "no importer '" + options.importerId + "'", std::move(ctx));
        }
    } else {
        auto ranked = sniff(path);
        if (!ranked) {
            return Failure{ranked.error()};
        }
        if (ranked->empty()) {
            ErrorContext ctx;
            ctx.path = path;
            return fail(ErrorCode::NotFound, "no importer recognises the file", std::move(ctx));
        }
        imp = ranked->front().first;
    }
    auto source = openByteSource(path);
    if (!source) {
        return Failure{source.error()};
    }
    auto p = imp->probe(**source);
    if (!p) {
        return Failure{p.error()};
    }
    return std::make_pair(imp, std::move(p).value());
}

Result<OpenedSource> ImporterRegistry::open(const std::filesystem::path& path, ImportOptions options,
                                            const OpenContext& context, const CancelToken& cancel) const {
    auto source = openByteSource(path);
    if (!source) {
        return Failure{source.error()};
    }
    return open(*source, std::move(options), context, cancel);
}

Result<OpenedSource> ImporterRegistry::open(const std::shared_ptr<const ByteSource>& source,
                                            ImportOptions options, const OpenContext& context,
                                            const CancelToken& cancel) const {
    if (!source) {
        return fail(ErrorCode::Invalid, "open: no source");
    }
    const std::filesystem::path& path = source->path();
    const auto withPath = [&](Error e) {
        if (!e.context.path && !path.empty()) {
            e.context.path = path;
        }
        return Failure{std::move(e)};
    };

    // ---- 1. sniff (or the caller's importer) -----------------------------------------------------------
    const IImporter* imp = nullptr;
    if (!options.importerId.empty()) {
        imp = find(options.importerId);
        if (imp == nullptr) {
            ErrorContext ctx;
            ctx.importer = options.importerId;
            ctx.path = path;
            return fail(ErrorCode::NotFound, "no importer '" + options.importerId + "'", std::move(ctx));
        }
    } else {
        const std::vector<std::filesystem::path> siblings = source->siblings();
        SniffInput si;
        si.path = path;
        si.head = source->span(0, kSniffHeadBytes);
        si.size = source->size();
        si.siblings = siblings;
        const auto ranked = sniff(si);
        if (ranked.empty()) {
            ErrorContext ctx;
            ctx.path = path;
            return fail(ErrorCode::NotFound, "no importer recognises the file", std::move(ctx));
        }
        imp = ranked.front().first;
    }
    options.importerId = std::string(imp->id());
    options.importerVersion = std::string(imp->version());
    if (cancel.cancelled()) {
        return fail(ErrorCode::Cancelled, "open cancelled");
    }

    // ---- 2-3. probe and open -----------------------------------------------------------------------------
    auto opened = imp->open(source, options);
    if (!opened) {
        return withPath(opened.error());
    }
    std::shared_ptr<IReader> reader(std::move(opened).value());
    const ProbeResult& probe = reader->probe();
    if (probe.layers.empty()) {
        return fail(ErrorCode::Internal, "open: the reader reported no layer");
    }
    ImportLog log;
    for (ImportLogEntry& e : reader->warnings()) {
        log.add(std::move(e));
    }
    store::ChunkCache* cache = context.cache != nullptr ? context.cache : store::ChunkCache::installed();
    if (cache == nullptr) {
        return fail(ErrorCode::Invalid, "open: no chunk cache (OpenContext::cache, or one installed)");
    }
    const std::shared_ptr<DecodeLog> decodeLog = reader->decodeLog();
    const model::SourceId id = source->quickHash(reader->formatHeader());

    auto ds = std::make_shared<model::DataSource>();
    ds->id = id;
    ds->prov.path = path;
    for (const std::filesystem::path& c : probe.companions) {
        ds->prov.companions.push_back(path.has_parent_path() ? path.parent_path() / c : c);
    }
    ds->prov.importerId = options.importerId;
    ds->prov.importerVersion = options.importerVersion;
    ds->prov.size = source->size();
    ds->prov.hash = id;
    ds->prov.mtimeUnixUs = source->mtimeUnixUs();
    ds->prov.dialectFlags = splitDialect(probe.dialect);
    auto probeJson = probe.toJson();
    if (!probeJson) {
        return withPath(probeJson.error());
    }
    ds->prov.probe = std::move(probeJson).value();
    ds->prov.rawHeader = reader->rawHeader();
    for (const model::Detail& d : probe.details) {
        if (d.prov == model::Provenance::Logged) {
            ds->details.push_back(d); // companion details arrive through companion() in step 5
        }
    }

    // ---- 3-4. layers, channels, units ---------------------------------------------------------------------
    const units::UnitRegistry* units = context.units;
    if (units == nullptr) {
        log.add(Level::Info, std::string(open_code::kUnitsSkipped),
                "no unit registry: channels open unitless, in the units the file stores");
    }
    std::map<std::string, UnresolvedUnit> unresolved;
    std::vector<std::string> strictMisses;
    const model::CompanionData companion =
        context.mergeCompanion ? reader->companion() : model::CompanionData{};
    // One readBlock at a time across all channels of a reader that is not chunk-independent.
    const std::shared_ptr<std::mutex> readerMutex =
        reader->chunkIndependent() ? nullptr : std::make_shared<std::mutex>();
    for (LayerIx li = 0; li < probe.layers.size(); ++li) {
        const LayerInfo& info = probe.layers[li];
        model::DataLayer layer;
        layer.name = info.name;
        layer.origin = info.utc;
        layer.duration = info.durationUs;
        for (ChannelIx ci = 0; ci < info.channels.size(); ++ci) {
            if (cancel.cancelled()) {
                return fail(ErrorCode::Cancelled, "open cancelled");
            }
            const ChannelInfo& c = info.channels[ci];
            const Rate* rate = std::get_if<Rate>(&c.timing);
            if (rate == nullptr) {
                return fail(ErrorCode::Unsupported,
                            "open: timestamped channels are not supported in M0 ('" + c.loggedName + "')");
            }
            model::Channel ch;
            model::ChannelDescriptor& d = ch.desc;
            d.loggedName = c.loggedName;
            d.name = c.loggedName;
            d.shortName = c.shortName;
            d.id = model::channelIdLogged(id, layer.name, c.loggedName, options.importerId);
            d.dtype = c.dtype;
            d.kind = c.kind;
            d.interp = reader->interp(li, ci);
            d.dps = c.dps;
            if (c.enumTable) {
                d.enums = model::EnumTableId{*c.enumTable};
            }
            d.source = model::ChannelSource::Logged;
            d.rate = *rate;
            d.sourceEncoding = reader->sourceEncoding(li, ci);

            // Step 4: the vendor unit against the registry; conversion to the base unit at decode (D-010).
            const std::string_view vendorUnit = trim(c.vendorUnit);
            if (units != nullptr && !vendorUnit.empty()) {
                if (const auto m = units->match(vendorUnit)) {
                    const units::QuantityId q = units->quantityOf(m->unit);
                    const units::Converter toBase = units->toBase(m->unit);
                    const bool numeric =
                        c.kind == store::SampleKind::Real || c.kind == store::SampleKind::Integer;
                    if (!numeric && !toBase.identity()) {
                        ImportLogEntry::Context ctx;
                        ctx.layer = layer.name;
                        ctx.channel = c.loggedName;
                        ctx.detail = std::string(vendorUnit);
                        log.add(Level::Info, std::string(open_code::kUnitNotApplied),
                                "a " + std::string(store::sampleKindName(c.kind)) +
                                    " channel keeps its stored values; its unit is not applied",
                                std::move(ctx));
                    } else {
                        d.unit = units->quantity(q).base;
                        d.quantity = q;
                        if (!toBase.identity()) {
                            const ChannelConversion conv{toBase, convertedDtype(c)};
                            if (auto r = reader->setConversion(li, ci, conv); !r) {
                                return withPath(r.error());
                            }
                            d.dtype = conv.dtype;
                            if (d.kind == store::SampleKind::Integer) {
                                d.kind = store::SampleKind::Real;
                            }
                        }
                    }
                } else {
                    UnresolvedUnit& u = unresolved[std::string(vendorUnit)];
                    if (u.count++ == 0) {
                        u.firstChannel = c.loggedName;
                        u.layer = layer.name;
                    }
                    if (options.strictUnits) {
                        strictMisses.push_back(c.loggedName + " [" + std::string(vendorUnit) + "]");
                    }
                    // Headless open (05 section 2 step 4): unitless, with the status text the Channel
                    // Status view shows; statusFromText() keeps the channel Ok.
                    d.statusText = "Unit unknown: \"" + std::string(vendorUnit) + "\"";
                    ch.status = model::statusFromText(d.statusText);
                }
            }

            std::shared_ptr<store::IChunkProvider> provider = reader->makeChunkProvider(li, ci);
            if (!provider) {
                provider =
                    std::make_shared<ImporterChunkProvider>(reader, li, ci, d.dtype, decodeLog, readerMutex);
            }
            store::ColumnOptions co;
            co.cache = cache;
            co.pool = context.pool;
            co.taskKeyPrefix = "chunk/" + id.hex() + "/" + layer.name + "/" + std::to_string(ci);
            ch.series = store::UniformSeries{*rate, c.t0Us, store::Column(std::move(provider), co)};
            layer.addChannel(std::move(ch));
        }
        for (const MarkerInfo& m : reader->markers(li)) {
            layer.markers.push_back(model::makeLoggedMarker(m.cls, m.tUs, m.group, m.name, m.comment, m.flags,
                                                            m.ordinal, model::Provenance::Logged));
        }
        // SOL/EOL synthesised (prov Derived) when neither the log nor its companion has them (02
        // section 4.3).
        bool companionIntrinsic = false;
        for (const auto& [name, markers] : companion.markers) {
            if (model::canonicalLayerName(name) == model::canonicalLayerName(layer.name) &&
                hasIntrinsic(markers)) {
                companionIntrinsic = true;
            }
        }
        if (layer.duration > 0 && !hasIntrinsic(layer.markers) && !companionIntrinsic) {
            layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::SOL, 0,
                                                            std::string(model::kGroupIntrinsic), {}, {}, 0, 0,
                                                            model::Provenance::Derived));
            layer.markers.push_back(model::makeLoggedMarker(model::MarkerClass::EOL, layer.duration,
                                                            std::string(model::kGroupIntrinsic), {}, {}, 0, 0,
                                                            model::Provenance::Derived));
        }
        model::sortMarkers(layer.markers);
        for (const EnumTableInfo& t : probe.enumTables) {
            layer.enumTables.push_back(model::EnumTable{model::EnumTableId{t.id}, {}, t.entries});
        }
        ds->layers.push_back(std::move(layer));
    }
    for (const auto& [spelling, u] : unresolved) {
        ImportLogEntry::Context ctx;
        ctx.layer = u.layer;
        ctx.channel = u.firstChannel;
        ctx.detail = spelling;
        ctx.repeats = u.count;
        log.add(Level::Warning, std::string(open_code::kUnitUnknown),
                "unit \"" + spelling + "\" is not in the unit registry; " + std::to_string(u.count) +
                    " channel(s) open unitless",
                std::move(ctx));
    }
    if (!strictMisses.empty()) {
        std::string list;
        for (const std::string& s : strictMisses) {
            list += (list.empty() ? "" : ", ") + s;
        }
        ErrorContext ctx;
        ctx.path = path;
        ctx.detail = list;
        return fail(ErrorCode::NotFound, "strict units: unresolved units on " + list, std::move(ctx));
    }

    // ---- 5. companion, file details, sidecar (precedence sidecar > companion > file, D-015) ---------------
    addNotes(log, model::mergeCompanion(*ds, companion));
    if (!path.empty()) {
        model::deriveFileDetails(*ds);
    }
    if (units != nullptr) {
        // The base (file + companion) resolved silently; the notes come from the final details below, after
        // the sidecar's own numeric details joined them.
        (void)model::resolveDetailUnits(ds->baseDetails, *units);
    }
    if (options.expectedHash && *options.expectedHash != id) {
        log.add(Level::Warning, std::string(open_code::kSourceChanged),
                "the file's source id " + id.hex() + " differs from the expected " +
                    options.expectedHash->hex());
    }
    model::SidecarDocument sidecar;
    if (context.loadSidecar && !path.empty() && source->mappedFile() != nullptr) { // files only
        auto loaded = model::SidecarDocument::load(model::SidecarDocument::pathFor(path));
        if (loaded) {
            sidecar = std::move(loaded).value();
        } else {
            ImportLogEntry::Context ctx;
            ctx.detail = loaded.error().toString();
            log.add(Level::Warning, std::string(open_code::kSidecarLoadFailed),
                    "the sidecar could not be read; the source opens without its edits", std::move(ctx));
        }
    }
    {
        model::SidecarApplyReport rep = model::applySidecar(*ds, std::move(sidecar));
        addNotes(log, rep.notes);
    }
    if (units != nullptr) {
        addNotes(log, model::resolveDetailUnits(ds->details, *units));
    }

    // ---- 6. phase-1 ranges (the caller's rules; without them the whole-outing rule of M0) -----------------
    addNotes(log,
             (context.rangeRebuild ? context.rangeRebuild(*ds) : model::rebuildWholeOutingRanges(*ds)).notes);

    {
        std::lock_guard lock(logsMutex_);
        decodeLogs_[id] = decodeLog;
    }
    OpenedSource out;
    out.source = std::move(ds);
    out.probe = probe;
    out.log = log.snapshot();
    out.decodeLog = decodeLog;
    return out;
}

Future<OpenedSource> ImporterRegistry::load(const std::filesystem::path& path, ImportOptions options,
                                            TaskPool& pool, const OpenContext& context,
                                            CancelToken cancel) const {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        canonical = path;
    }
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    if (auto st = statFile(path)) {
        size = st->size;
        mtime = st->mtimeUnixUs;
    }
    TaskKey key{"open/" + pathToUtf8(canonical) + "/" + std::to_string(size) + "/" + std::to_string(mtime) +
                "/a"};
    return pool.submit(Priority::Interactive, std::move(key), std::move(cancel),
                       [this, path, options = std::move(options), context](const CancelToken& token) {
                           return open(path, options, context, token);
                       });
}

std::shared_ptr<DecodeLog> ImporterRegistry::decodeLog(const model::SourceId& id) const {
    std::lock_guard lock(logsMutex_);
    const auto it = decodeLogs_.find(id);
    return it == decodeLogs_.end() ? nullptr : it->second.lock();
}

void ImporterRegistry::release(const model::SourceId& id) {
    std::lock_guard lock(logsMutex_);
    decodeLogs_.erase(id);
}

} // namespace esses::formats
