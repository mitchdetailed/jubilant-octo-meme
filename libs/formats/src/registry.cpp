// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ImporterRegistry and the open pipeline (05 section 2, M0 scope in registry.hpp).
#include "builtin_text_profiles.hpp"
#include "importer_chunk_provider.hpp"

#include <esses/base/assert.hpp>
#include <esses/base/taskpool.hpp>
#include <esses/base/utf8.hpp>
#include <esses/formats/csv.hpp>
#include <esses/formats/ld_importer.hpp>
#include <esses/formats/registry.hpp>
#include <esses/formats/text_engine.hpp>
#include <esses/model/whole_outing.hpp>
#include <esses/store/chunk_cache.hpp>
#include <esses/store/column.hpp>
#include <esses/store/series.hpp>
#include <esses/units/unit_registry.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
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
    // The shipped text profiles with their dictionaries (embedded copies of data/profiles-text and
    // data/dictionaries; M2, D-044), registered after motec.ld so that sniff ties keep the binary format
    // first.
    const std::pair<std::string_view, std::string_view> shipped[] = {
        {text::builtin::kLegacyCsvProfile, text::builtin::kLegacyCsvDictionary},
        {text::builtin::kGenericCsvProfile, text::builtin::kGenericCsvDictionary}};
    for (const auto& [profileJson, dictionaryJson] : shipped) {
        std::vector<std::string> notes;
        auto profile = TextProfile::parse(profileJson, notes);
        auto dictionary = Dictionary::parse(dictionaryJson, notes);
        ESSES_ASSERT_MSG(profile && dictionary, "a shipped text profile or dictionary does not parse");
        if (profile) {
            std::shared_ptr<const Dictionary> d =
                dictionary ? std::make_shared<const Dictionary>(std::move(dictionary).value()) : nullptr;
            const Result<void> added = r->addProfile(std::move(profile).value(), std::move(d));
            ESSES_ASSERT_MSG(added.ok(), "a shipped text profile collides with another importer");
            (void)added;
        }
    }
    r->add(makeLegacyCsvWriter(units));
    r->add(makeGenericCsvWriter(units));
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

// ---- the open pipeline (05 section 2) -------------------------------------------------------------------

// The state task A hands to task B (mapping.hpp): the reader, the skeleton DataSource (provenance, details,
// no layers yet), one plan per channel with its descriptor after step 4, and the log so far.
struct OpenState {
    struct Plan {
        LayerIx layer = 0;
        ChannelIx index = 0;
        model::ChannelDescriptor desc;
        model::ChannelStatus status = model::ChannelStatus::Ok;
        std::string vendorUnit;                  // the spelling looked up (after step 4a)
        std::optional<std::string> quantityHint; // a quantity named by a hint or the dictionary
        bool unresolved = false;
    };
    std::shared_ptr<const ByteSource> source;
    std::shared_ptr<IReader> reader;
    ImportOptions options;
    OpenContext context;
    model::SourceId id;
    std::shared_ptr<model::DataSource> ds;
    ImportLog log;
    std::vector<Plan> plans;
    std::vector<model::MappingRow> mappings; // analysis names of unit-map rules and of the user's decision
    std::optional<UnitMappingProfile> unitMap;
};

namespace {

std::optional<store::SampleKind> kindFrom(std::string_view s) {
    return parseSampleKind(s);
}

std::optional<store::Interp> interpFrom(std::string_view s) {
    if (s == "Linear") {
        return store::Interp::Linear;
    }
    if (s == "Closest") {
        return store::Interp::Closest;
    }
    if (s == "Previous") {
        return store::Interp::Previous;
    }
    if (s == "Next") {
        return store::Interp::Next;
    }
    return std::nullopt;
}

// Step 4a: a hint or dictionary entry overwrites what it states (05 section 8.3); `dtype` collects a storage
// override for the reader.
void applyEntry(const Dictionary::Entry& e, OpenState::Plan& p, std::optional<store::Dtype>& dtype) {
    model::ChannelDescriptor& d = p.desc;
    if (e.name && !e.name->empty()) {
        d.name = *e.name;
    }
    if (e.shortName) {
        d.shortName = *e.shortName;
    }
    if (e.unit) {
        p.vendorUnit = std::string(trim(*e.unit));
    }
    if (e.quantity && !e.quantity->empty()) {
        p.quantityHint = *e.quantity;
    }
    if (e.kind) {
        if (const auto k = kindFrom(*e.kind)) {
            d.kind = *k;
        }
    }
    if (e.interp) {
        if (const auto i = interpFrom(*e.interp)) {
            d.interp = *i;
        }
    }
    if (e.dtype) {
        dtype = parseDtype(*e.dtype);
    }
}

// Step 4b for one channel: the unit map's rule (05 section 8.2, most specific first), else the registry's
// spelling match; the samples are converted to the quantity's base unit at decode (D-010). false when the
// channel stays unresolved (a vendor unit nothing knows, or a quantity without a unit).
Result<bool> resolveChannel(OpenState& st, OpenState::Plan& p, const ChannelInfo& c,
                            const UnitMappingProfile* map) {
    const units::UnitRegistry* units = st.context.units;
    if (units == nullptr) {
        return true;
    }
    model::ChannelDescriptor& d = p.desc;
    std::optional<units::UnitId> unit;
    std::optional<std::string> analysisName;
    if (map != nullptr) {
        if (const auto r = map->resolve(d.loggedName, p.vendorUnit, *units)) {
            unit = r->unit;
            analysisName = r->analysisName;
        }
    }
    if (!unit && !p.vendorUnit.empty()) {
        if (const auto m = units->match(p.vendorUnit)) {
            unit = m->unit;
        }
    }
    if (!unit) {
        return p.vendorUnit.empty() && !p.quantityHint;
    }
    const units::QuantityId q = units->quantityOf(*unit);
    const units::Converter toBase = units->toBase(*unit);
    const bool numeric = d.kind == store::SampleKind::Real || d.kind == store::SampleKind::Integer;
    if (!numeric && !toBase.identity()) {
        ImportLogEntry::Context ctx;
        ctx.layer = st.reader->probe().layers[p.layer].name;
        ctx.channel = c.loggedName;
        ctx.detail = p.vendorUnit;
        st.log.add(Level::Info, std::string(open_code::kUnitNotApplied),
                   "a " + std::string(store::sampleKindName(d.kind)) +
                       " channel keeps its stored values; its unit is not applied",
                   std::move(ctx));
        return true;
    }
    d.unit = units->quantity(q).base;
    d.quantity = q;
    if (!toBase.identity()) {
        ChannelInfo widened = c;
        widened.dtype = d.dtype;
        const ChannelConversion conv{toBase, convertedDtype(widened)};
        if (auto r = st.reader->setConversion(p.layer, p.index, conv); !r) {
            return Failure{r.error()};
        }
        d.dtype = conv.dtype;
        if (d.kind == store::SampleKind::Integer) {
            d.kind = store::SampleKind::Real;
        }
    }
    if (analysisName) {
        st.mappings.push_back(model::MappingRow{d.loggedName, *analysisName, std::nullopt, false});
    }
    return true;
}

// A few decoded values of a channel for the Mapping panel's "Sample values" column (task A, worker).
std::vector<double> sampleValues(IReader& reader, const OpenState::Plan& p, const ChannelInfo& c) {
    std::vector<double> out;
    const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(c.sampleCount, 5));
    if (n == 0) {
        return out;
    }
    const store::Dtype dt = p.desc.dtype;
    std::vector<std::byte> values(std::size_t{n} * store::dtypeSize(dt));
    std::vector<std::uint8_t> bits((n + 7) / 8, 0xFF);
    Block b;
    b.dtype = dt;
    b.first = 0;
    b.count = n;
    b.values = values;
    b.validBits = bits;
    if (!reader.readBlock(p.layer, p.index, 0, n, b, {})) {
        return out;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        if (((bits[i >> 3] >> (i & 7)) & 1u) == 0) {
            continue;
        }
        double v = 0.0;
        if (dt == store::Dtype::F32) {
            float f = 0.0F;
            std::memcpy(&f, values.data() + std::size_t{i} * 4, 4);
            v = static_cast<double>(f);
        } else if (dt == store::Dtype::F64) {
            std::memcpy(&v, values.data() + std::size_t{i} * 8, 8);
        } else if (dt == store::Dtype::I32) {
            std::int32_t x = 0;
            std::memcpy(&x, values.data() + std::size_t{i} * 4, 4);
            v = static_cast<double>(x);
        }
        if (std::isfinite(v)) {
            out.push_back(v);
        }
    }
    return out;
}

std::string openKey(const std::filesystem::path& path, std::uint64_t size, std::int64_t mtime, char task) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        canonical = path;
    }
    return "open/" + pathToUtf8(canonical) + "/" + std::to_string(size) + "/" + std::to_string(mtime) + "/" +
           std::string(1, task);
}

} // namespace

Result<std::shared_ptr<OpenState>>
ImporterRegistry::beginOpen(const std::shared_ptr<const ByteSource>& source, ImportOptions options,
                            const OpenContext& context, const CancelToken& cancel) const {
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
    options.cancel = cancel; // the importer's own long passes stop with the open (D-190)
    auto opened = imp->open(source, options);
    if (!opened) {
        return withPath(opened.error());
    }
    auto st = std::make_shared<OpenState>();
    st->source = source;
    st->reader = std::shared_ptr<IReader>(std::move(opened).value());
    st->context = context;
    IReader& reader = *st->reader;
    const ProbeResult& probe = reader.probe();
    if (probe.layers.empty()) {
        return fail(ErrorCode::Internal, "open: the reader reported no layer");
    }
    for (ImportLogEntry& e : reader.warnings()) {
        st->log.add(std::move(e));
    }
    store::ChunkCache* cache = context.cache != nullptr ? context.cache : store::ChunkCache::installed();
    if (cache == nullptr) {
        return fail(ErrorCode::Invalid, "open: no chunk cache (OpenContext::cache, or one installed)");
    }
    st->context.cache = cache;
    const model::SourceId id = source->quickHash(reader.formatHeader());
    st->id = id;

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
    ds->prov.rawHeader = reader.rawHeader();
    for (const model::Detail& d : probe.details) {
        if (d.prov == model::Provenance::Logged) {
            ds->details.push_back(d); // companion details arrive through companion() in step 5
        }
    }
    st->ds = std::move(ds);

    // ---- 4. dictionary (4a), unit map and units (4b) ------------------------------------------------------
    const units::UnitRegistry* units = context.units;
    if (units == nullptr) {
        st->log.add(Level::Info, std::string(open_code::kUnitsSkipped),
                    "no unit registry: channels open unitless, in the units the file stores");
    }
    if (options.unitMap) {
        std::vector<std::string> notes;
        auto m = UnitMappingProfile::load(*options.unitMap, notes);
        if (m) {
            st->unitMap = std::move(m).value();
        } else {
            ImportLogEntry::Context ctx;
            ctx.detail = m.error().toString();
            st->log.add(Level::Warning, std::string(open_code::kUnitMapLoadFailed),
                        "the unit map could not be read; the source opens without it", std::move(ctx));
        }
    } else if (const auto it = context.unitMaps.find(UnitMappingProfile::fileStem(options.importerId));
               it != context.unitMaps.end()) {
        st->unitMap = it->second;
    }
    const Dictionary* dictionary = options.applyDictionary ? reader.dictionary() : nullptr;
    const UnitMappingProfile* unitMap = nullptr; // the unit map every channel resolves against, if any
    if (const std::optional<UnitMappingProfile>& m = st->unitMap) {
        unitMap = &*m;
    }
    for (LayerIx li = 0; li < probe.layers.size(); ++li) {
        const LayerInfo& info = probe.layers[li];
        for (ChannelIx ci = 0; ci < info.channels.size(); ++ci) {
            if (cancel.cancelled()) {
                return fail(ErrorCode::Cancelled, "open cancelled");
            }
            const ChannelInfo& c = info.channels[ci];
            OpenState::Plan p;
            p.layer = li;
            p.index = ci;
            model::ChannelDescriptor& d = p.desc;
            d.loggedName = c.loggedName;
            d.name = c.loggedName;
            d.shortName = c.shortName;
            d.id = model::channelIdLogged(id, info.name, c.loggedName, options.importerId);
            d.dtype = c.dtype;
            d.kind = c.kind;
            d.interp = reader.interp(li, ci);
            d.dps = c.dps;
            if (c.enumTable) {
                d.enums = model::EnumTableId{*c.enumTable};
            }
            d.source = model::ChannelSource::Logged;
            if (const Rate* rate = std::get_if<Rate>(&c.timing)) {
                d.rate = *rate;
            }
            d.sourceEncoding = reader.sourceEncoding(li, ci);
            p.vendorUnit = std::string(trim(c.vendorUnit));
            std::optional<store::Dtype> dtype;
            if (const Dictionary::Entry* hint = reader.columnHint(li, ci)) {
                applyEntry(*hint, p, dtype);
            }
            if (dictionary != nullptr) {
                if (const Dictionary::Entry* e = dictionary->match(c.vendorId.value_or(""), c.loggedName)) {
                    applyEntry(*e, p, dtype);
                }
            }
            if (dtype && *dtype != d.dtype &&
                reader.setConversion(li, ci, ChannelConversion{units::Converter{}, *dtype})) {
                d.dtype =
                    *dtype; // the dictionary's storage dtype (05 section 8.3), when the reader can store it
            }
            auto resolved = resolveChannel(*st, p, c, unitMap);
            if (!resolved) {
                return withPath(resolved.error());
            }
            p.unresolved = !*resolved;
            st->plans.push_back(std::move(p));
        }
    }
    st->options = std::move(options);
    return st;
}

Result<OpenedSource> ImporterRegistry::finishOpen(OpenState& st, const CancelToken& cancel) const {
    IReader& reader = *st.reader;
    const ProbeResult& probe = reader.probe();
    const std::filesystem::path& path = st.source->path();
    const units::UnitRegistry* units = st.context.units;
    store::ChunkCache* cache = st.context.cache;
    const model::SourceId id = st.id;
    model::DataSource& ds = *st.ds;
    ImportLog& log = st.log;

    // ---- 4, leftovers: unresolved channels open unitless with the status text (headless, 05 section 2) --
    std::map<std::string, UnresolvedUnit> unresolved;
    std::vector<std::string> strictMisses;
    for (OpenState::Plan& p : st.plans) {
        if (!p.unresolved) {
            continue;
        }
        const std::string spelling = !p.vendorUnit.empty()
                                         ? p.vendorUnit
                                         : "(" + p.quantityHint.value_or(std::string()) + ", no unit)";
        UnresolvedUnit& u = unresolved[spelling];
        if (u.count++ == 0) {
            u.firstChannel = p.desc.loggedName;
            u.layer = probe.layers[p.layer].name;
        }
        if (st.options.strictUnits) {
            strictMisses.push_back(p.desc.loggedName + " [" + spelling + "]");
        }
        // statusFromText() keeps the channel Ok: the Channel Status view shows the text.
        p.desc.statusText = !p.vendorUnit.empty() ? "Unit unknown: \"" + p.vendorUnit + "\""
                                                  : "Unit unknown: " + p.quantityHint.value_or(std::string());
        p.status = model::statusFromText(p.desc.statusText);
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

    // ---- 3 (columns), markers, enumeration tables; 4c mappings --------------------------------------------
    const std::shared_ptr<DecodeLog> decodeLog = reader.decodeLog();
    const model::CompanionData companion =
        st.context.mergeCompanion ? reader.companion() : model::CompanionData{};
    // One readBlock at a time across all channels of a reader that is not chunk-independent.
    const std::shared_ptr<std::mutex> readerMutex =
        reader.chunkIndependent() ? nullptr : std::make_shared<std::mutex>();
    model::MappingTable mappings = st.context.mappings;
    mappings.rows.insert(mappings.rows.end(), st.mappings.begin(), st.mappings.end());
    std::size_t next = 0;
    for (LayerIx li = 0; li < probe.layers.size(); ++li) {
        const LayerInfo& info = probe.layers[li];
        model::DataLayer layer;
        layer.name = info.name;
        layer.origin = info.utc;
        layer.duration = info.durationUs;
        std::map<const store::IChunkProvider*, store::Column> timeColumns; // shared I64 time columns
        for (; next < st.plans.size() && st.plans[next].layer == li; ++next) {
            if (cancel.cancelled()) {
                return fail(ErrorCode::Cancelled, "open cancelled");
            }
            OpenState::Plan& p = st.plans[next];
            const ChannelIx ci = p.index;
            const ChannelInfo& c = info.channels[ci];
            model::Channel ch;
            ch.desc = p.desc;
            ch.status = p.status;
            std::shared_ptr<store::IChunkProvider> provider = reader.makeChunkProvider(li, ci);
            if (!provider) {
                provider = std::make_shared<ImporterChunkProvider>(st.reader, li, ci, ch.desc.dtype,
                                                                   decodeLog, readerMutex);
            }
            store::ColumnOptions co;
            co.cache = cache;
            co.pool = st.context.pool;
            co.taskKeyPrefix = "chunk/" + id.hex() + "/" + layer.name + "/" + std::to_string(ci);
            store::Column values(std::move(provider), co);
            if (const Rate* rate = std::get_if<Rate>(&c.timing)) {
                ch.series = store::UniformSeries{*rate, c.t0Us, values};
                reader.attachColumns(li, ci, values, nullptr, *cache);
            } else {
                std::shared_ptr<store::IChunkProvider> tp = reader.makeTimesProvider(li, ci);
                if (!tp) {
                    tp = std::make_shared<TimestampChunkProvider>(st.reader, li, ci, decodeLog, readerMutex);
                }
                auto found = timeColumns.find(tp.get());
                if (found == timeColumns.end()) {
                    store::ColumnOptions to = co;
                    to.taskKeyPrefix = "chunk/" + id.hex() + "/" + layer.name + "/t" + std::to_string(ci);
                    const store::IChunkProvider* key = tp.get();
                    found = timeColumns.emplace(key, store::Column(std::move(tp), to)).first;
                }
                const store::Column& times = found->second;
                reader.attachColumns(li, ci, values, &times, *cache);
                if (const auto range = reader.timeRange(li, ci)) {
                    ch.series = store::TimestampedSeries{times, values, range->first, range->second};
                } else {
                    auto series = store::makeTimestampedSeries(times, values, cancel);
                    if (!series) {
                        return Failure{series.error()};
                    }
                    ch.series = std::move(series).value();
                }
            }
            layer.addChannel(std::move(ch));
        }
        for (const MarkerInfo& m : reader.markers(li)) {
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
        if (!mappings.rows.empty()) {
            (void)model::applyMappings(layer, mappings, id, units);
        }
        ds.layers.push_back(std::move(layer));
    }

    // ---- 5. companion, file details, sidecar (precedence sidecar > companion > file, D-015) ---------------
    addNotes(log, model::mergeCompanion(ds, companion));
    if (!path.empty()) {
        model::deriveFileDetails(ds);
    }
    if (units != nullptr) {
        // The base (file + companion) resolved silently; the notes come from the final details below, after
        // the sidecar's own numeric details joined them.
        (void)model::resolveDetailUnits(ds.baseDetails, *units);
    }
    if (st.options.expectedHash && *st.options.expectedHash != id) {
        log.add(Level::Warning, std::string(open_code::kSourceChanged),
                "the file's source id " + id.hex() + " differs from the expected " +
                    st.options.expectedHash->hex());
    }
    model::SidecarDocument sidecar;
    if (st.context.loadSidecar && !path.empty() && st.source->mappedFile() != nullptr) { // files only
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
        model::SidecarApplyReport rep = model::applySidecar(ds, std::move(sidecar));
        addNotes(log, rep.notes);
    }
    if (units != nullptr) {
        addNotes(log, model::resolveDetailUnits(ds.details, *units));
    }

    // ---- 6. phase-1 ranges (the caller's rules; without them the whole-outing rule of M0) -----------------
    addNotes(
        log,
        (st.context.rangeRebuild ? st.context.rangeRebuild(ds) : model::rebuildWholeOutingRanges(ds)).notes);

    {
        std::lock_guard lock(logsMutex_);
        decodeLogs_[id] = decodeLog;
    }
    OpenedSource out;
    out.source = st.ds;
    out.probe = probe;
    out.log = log.snapshot();
    out.decodeLog = decodeLog;
    return out;
}

Result<OpenedSource> ImporterRegistry::open(const std::shared_ptr<const ByteSource>& source,
                                            ImportOptions options, const OpenContext& context,
                                            const CancelToken& cancel) const {
    auto st = beginOpen(source, std::move(options), context, cancel);
    if (!st) {
        return Failure{st.error()};
    }
    return finishOpen(**st, cancel);
}

Future<OpenedSource> ImporterRegistry::load(const std::filesystem::path& path, ImportOptions options,
                                            TaskPool& pool, const OpenContext& context,
                                            CancelToken cancel) const {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    if (auto st = statFile(path)) {
        size = st->size;
        mtime = st->mtimeUnixUs;
    }
    TaskKey key{openKey(path, size, mtime, 'a')};
    return pool.submit(Priority::Interactive, std::move(key), std::move(cancel),
                       [this, path, options = std::move(options), context](const CancelToken& token) {
                           return open(path, options, context, token);
                       });
}

// ---- M2: text profiles and the Mapping-panel hand-off (05 sections 2, 7 and 8) ---------------------------

Result<void> ImporterRegistry::addProfile(TextProfile profile, std::shared_ptr<const Dictionary> dictionary) {
    if (!profile.importerId.empty() && profile.importerId != kLegacyCsvImporterId) {
        ErrorContext ctx;
        ctx.importer = profile.importerId;
        return fail(ErrorCode::Invalid,
                    "profile '" + profile.id +
                        "': only the shipped profiles may set importerId, and only to a "
                        "built-in id",
                    std::move(ctx));
    }
    const std::string id = profile.effectiveImporterId();
    if (find(id) != nullptr) {
        ErrorContext ctx;
        ctx.importer = id;
        return fail(ErrorCode::Invalid, "an importer '" + id + "' is already registered", std::move(ctx));
    }
    importers_.push_back(TextEngine::makeImporter(std::move(profile), std::move(dictionary)));
    return ok();
}

Future<LoadResult> ImporterRegistry::load(const std::filesystem::path& path, ImportOptions options,
                                          TaskPool& pool, const OpenContext& context, CancelToken cancel,
                                          MappingHook hook) const {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    if (auto st = statFile(path)) {
        size = st->size;
        mtime = st->mtimeUnixUs;
    }
    TaskKey key{openKey(path, size, mtime, 'a')};
    return pool.submit(Priority::Interactive, std::move(key), std::move(cancel),
                       [this, path, options = std::move(options), context,
                        hook = std::move(hook)](const CancelToken& token) -> Result<LoadResult> {
                           auto source = openByteSource(path);
                           if (!source) {
                               return Failure{source.error()};
                           }
                           auto st = beginOpen(*source, options, context, token);
                           if (!st) {
                               return Failure{st.error()};
                           }
                           OpenState& state = **st;
                           std::vector<UnresolvedRow> rows;
                           const ProbeResult& probe = state.reader->probe();
                           for (const OpenState::Plan& p : state.plans) {
                               if (!p.unresolved) {
                                   continue;
                               }
                               const ChannelInfo& c = probe.layers[p.layer].channels[p.index];
                               UnresolvedRow r;
                               r.loggedName = p.desc.loggedName;
                               r.vendorUnit = p.vendorUnit;
                               r.inferredQuantity = p.quantityHint;
                               r.sampleValues = sampleValues(*state.reader, p, c);
                               rows.push_back(std::move(r));
                           }
                           if (!rows.empty() && hook) {
                               // Task A ends here; the GUI shows the Mapping panel and submits task B through
                               // resume().
                               PendingResolution pending =
                                   hook(std::move(rows), std::shared_ptr<const OpenState>(*st));
                               if (pending.path.empty()) {
                                   pending.path = pathToUtf8(path);
                               }
                               if (pending.importerId.empty()) {
                                   pending.importerId = state.options.importerId;
                               }
                               if (!pending.skeleton) {
                                   pending.skeleton = *st;
                               }
                               return LoadResult{std::move(pending)};
                           }
                           auto opened = finishOpen(state, token);
                           if (!opened) {
                               return Failure{opened.error()};
                           }
                           return LoadResult{std::move(opened).value()};
                       });
}

Future<OpenedSource> ImporterRegistry::resume(PendingResolution pending, MappingDecision decision,
                                              TaskPool& pool, const OpenContext& context,
                                              CancelToken cancel) const {
    if (!pending.skeleton) {
        return makeReadyFuture<OpenedSource>(
            fail(ErrorCode::Invalid, "resume: the pending open has no state"));
    }
    const OpenState& skeleton = *pending.skeleton;
    TaskKey key{
        openKey(skeleton.source->path(), skeleton.source->size(), skeleton.source->mtimeUnixUs(), 'b')};
    return pool.submit(Priority::Interactive, std::move(key), std::move(cancel),
                       [this, skeletonPtr = std::move(pending.skeleton), decision = std::move(decision),
                        context](const CancelToken& token) -> Result<OpenedSource> {
                           // Task B works on its own copy (the reader is shared: a PendingResolution is
                           // resumed once).
                           OpenState st = *skeletonPtr;
                           if (context.units != nullptr || context.cache != nullptr) {
                               store::ChunkCache* cache = st.context.cache;
                               st.context = context;
                               if (st.context.cache == nullptr) {
                                   st.context.cache = cache;
                               }
                           }
                           for (const model::MappingRow& r : decision.mappings) {
                               st.mappings.push_back(r);
                           }
                           const ProbeResult& probe = st.reader->probe();
                           for (OpenState::Plan& p : st.plans) {
                               if (!p.unresolved) {
                                   continue;
                               }
                               const ChannelInfo& c = probe.layers[p.layer].channels[p.index];
                               if (decision.unitMap) {
                                   auto r = resolveChannel(st, p, c, &*decision.unitMap);
                                   if (!r) {
                                       return Failure{r.error()};
                                   }
                                   p.unresolved = !*r;
                               }
                           }
                           return finishOpen(st, token);
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
