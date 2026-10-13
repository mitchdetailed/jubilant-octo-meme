// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// DataSelectionModel (06 sections 5.1-5.6, D-017, D-109, D-110, D-135). Every slot below implements one row
// of 06 section 5.2 and emits changed() once with the bits it changed (nothing when nothing changed).

#include "range_lookup.hpp"

#include <esses/analysis/fastest_lap.hpp>
#include <esses/base/assert.hpp>
#include <esses/base/utf8.hpp>
#include <esses/qtmodel/data_selection_model.hpp>
#include <esses/qtmodel/project_session.hpp>

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>
#include <cctype>
#include <span>
#include <utility>

namespace esses::qt {

namespace {

constexpr std::string_view kHashPrefix = "xxh3:";

bool isNilRef(const model::RangeRef& r) noexcept {
    return r.range.v.isNil();
}

std::string genericUtf8(const std::filesystem::path& p) {
    std::string s = pathToUtf8(p);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// A path under `folder` is written relative to it (project.json recentSources/lastSelection, 02 section 6.2).
std::string relativeOrAbsolute(const std::filesystem::path& path, const std::filesystem::path& folder) {
    if (!folder.empty()) {
        std::error_code ec;
        const std::filesystem::path rel = std::filesystem::relative(path, folder, ec);
        if (!ec && !rel.empty() && rel.begin()->string() != "..") {
            return genericUtf8(rel);
        }
    }
    return genericUtf8(path);
}

project::RangeKey keyOf(const model::DataSource& src, const model::RangeRef& r, std::string sourcePath) {
    project::RangeKey k;
    k.source.path = std::move(sourcePath);
    k.source.hash = std::string(kHashPrefix) + src.id.hex();
    k.layer = std::string(model::canonicalLayerName(r.layer));
    k.groupPath = r.groupPath;
    if (const model::Range* range = detail::findRange(src, r)) {
        k.startMarker = range->startMarker.isNil() ? std::string() : range->startMarker.str();
        k.name = range->name;
    } else {
        k.name = r.name;
    }
    return k;
}

bool sameSlots(const std::vector<model::RangeRef>& a, const std::vector<model::RangeRef>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

} // namespace

DataSelectionModel::DataSelectionModel(ProjectSession* session, QObject* parent)
    : QObject(parent), session_(session) {
    if (session_ != nullptr) {
        connect(session_, &ProjectSession::projectOpened, this, [this] { readProjectOptions(); });
        connect(session_, &ProjectSession::documentsChanged, this, [this] { readProjectOptions(); });
        readProjectOptions();
    }
}

DataSelectionModel::~DataSelectionModel() = default;

bool DataSelectionModel::hasMain() const noexcept {
    return !isNilRef(snapshot_.main);
}

std::optional<model::RangeRef> DataSelectionModel::referenceOverlay() const {
    return snapshot_.referenceOverlay();
}

std::vector<model::RangeRef> DataSelectionModel::visibleOverlays() const {
    return snapshot_.overlaysHidden ? std::vector<model::RangeRef>{} : snapshot_.overlays;
}

int DataSelectionModel::overlayNumber(const model::RangeRef& r) const {
    if (snapshot_.overlay0 && *snapshot_.overlay0 == r) {
        return 0;
    }
    for (std::size_t i = 0; i < snapshot_.overlays.size(); ++i) {
        if (snapshot_.overlays[i] == r) {
            return static_cast<int>(i) + 1;
        }
    }
    return -1;
}

FilterSpec DataSelectionModel::filter(const model::SourceId& source) const {
    const auto it = filters_.find(source);
    return it == filters_.end() ? FilterSpec{} : it->second;
}

std::shared_ptr<const model::DataSource> DataSelectionModel::source(const model::SourceId& id) const {
    for (const auto& s : sources_) {
        if (s->id == id) {
            return s;
        }
    }
    return nullptr;
}

const model::Range* DataSelectionModel::resolve(const model::RangeRef& r) const {
    const auto src = source(r.src);
    return src == nullptr ? nullptr : detail::findRange(*src, r);
}

void DataSelectionModel::setDisplayedGroupPath(std::string groupPath) {
    displayedGroupPath_ = std::move(groupPath);
}

// ---- filters and displayed order -----------------------------------------------------------------------

std::optional<std::vector<std::pair<int, int>>> DataSelectionModel::parseLapNumbers(std::string_view text) {
    std::vector<std::pair<int, int>> terms;
    std::size_t i = 0;
    const auto skipSpace = [&] {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
        }
    };
    const auto readInt = [&]() -> std::optional<int> {
        skipSpace();
        const std::size_t begin = i;
        long long v = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
            v = v * 10 + (text[i] - '0');
            if (v > 1'000'000) {
                return std::nullopt;
            }
            ++i;
        }
        if (i == begin) {
            return std::nullopt;
        }
        return static_cast<int>(v);
    };
    while (true) {
        const std::optional<int> a = readInt();
        if (!a) {
            return std::nullopt;
        }
        int b = *a;
        skipSpace();
        if (i < text.size() && text[i] == '-') {
            ++i;
            const std::optional<int> hi = readInt();
            if (!hi) {
                return std::nullopt;
            }
            b = *hi;
            skipSpace();
        }
        terms.emplace_back(std::min(*a, b), std::max(*a, b));
        if (i == text.size()) {
            break;
        }
        if (text[i] != ',') {
            return std::nullopt;
        }
        ++i;
    }
    return terms;
}

bool DataSelectionModel::filterAccepts(const model::RangeRef& r) const {
    if ((hasMain() && r == snapshot_.main) || overlayNumber(r) >= 0) {
        return true; // filters never hide a slot (06 section 5.4)
    }
    const auto src = source(r.src);
    const model::Range* range = src == nullptr ? nullptr : detail::findRange(*src, r);
    if (range == nullptr) {
        return false;
    }
    const FilterSpec spec = filter(r.src);
    switch (spec.kind) {
    case FilterSpec::Kind::All:
        return true;
    case FilterSpec::Kind::Selected:
        return false; // the slots were accepted above
    case FilterSpec::Kind::LapNumbers: {
        const auto terms = parseLapNumbers(spec.laps);
        if (!terms) {
            return true; // a parse error keeps the previous filter in the editor; here: everything shows
        }
        return std::any_of(terms->begin(), terms->end(), [&](const std::pair<int, int>& t) {
            return range->number >= t.first && range->number <= t.second;
        });
    }
    case FilterSpec::Kind::Fastest:
    case FilterSpec::Kind::Fastest3:
    case FilterSpec::Kind::WithinOnePercent: {
        const model::RangeGroup* g = detail::groupOf(*src, r.layer, r.groupPath);
        if (g == nullptr) {
            return false;
        }
        const std::span<const model::Range> laps(g->ranges);
        if (spec.kind == FilterSpec::Kind::WithinOnePercent) {
            const auto fast = analysis::fastLapsWithin(laps, 1.0);
            return std::find(fast.begin(), fast.end(), range) != fast.end();
        }
        auto candidates = analysis::fastestLapCandidates(laps);
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const model::Range* a, const model::Range* b) {
                             if (a->duration() != b->duration()) {
                                 return a->duration() < b->duration();
                             }
                             return a->number < b->number;
                         });
        const std::size_t keep = spec.kind == FilterSpec::Kind::Fastest ? 1 : 3;
        for (std::size_t i = 0; i < candidates.size() && i < keep; ++i) {
            if (candidates[i] == range) {
                return true;
            }
        }
        return false;
    }
    }
    return true;
}

std::vector<model::RangeRef> DataSelectionModel::displayedRanges(const model::SourceId& source,
                                                                 std::string_view layer,
                                                                 std::string_view groupPath) const {
    std::vector<model::RangeRef> out;
    const auto src = this->source(source);
    const model::RangeGroup* g = src == nullptr ? nullptr : detail::groupOf(*src, layer, groupPath);
    if (g == nullptr) {
        return out;
    }
    std::vector<const model::Range*> ordered;
    ordered.reserve(g->ranges.size());
    for (const model::Range& r : g->ranges) {
        ordered.push_back(&r);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const model::Range* a, const model::Range* b) { return a->start < b->start; });
    for (const model::Range* r : ordered) {
        model::RangeRef ref = detail::makeRef(*src, layer, groupPath, *r);
        if (filterAccepts(ref)) {
            out.push_back(std::move(ref));
        }
    }
    return out;
}

std::vector<model::RangeRef> DataSelectionModel::displayedRanges(const model::SourceId& source) const {
    const auto src = this->source(source);
    if (src == nullptr) {
        return {};
    }
    if (detail::groupOf(*src, model::kNormalLayer, displayedGroupPath_) != nullptr) {
        return displayedRanges(source, model::kNormalLayer, displayedGroupPath_);
    }
    return displayedRanges(source, model::kNormalLayer, detail::kOutingsGroup);
}

std::optional<model::RangeRef> DataSelectionModel::firstRangeOf(const model::DataSource& src) const {
    for (const std::string_view path : {std::string_view(displayedGroupPath_), detail::kOutingsGroup}) {
        const model::RangeGroup* g = detail::groupOf(src, model::kNormalLayer, path);
        if (g == nullptr || g->ranges.empty()) {
            continue;
        }
        const auto first =
            std::min_element(g->ranges.begin(), g->ranges.end(),
                             [](const model::Range& a, const model::Range& b) { return a.start < b.start; });
        return detail::makeRef(src, model::kNormalLayer, path, *first);
    }
    // Any range of any group of any layer (a profile without outings).
    for (const model::DataLayer& layer : src.layers) {
        std::optional<model::RangeRef> found;
        layer.ranges.forEach([&](const model::RangeGroup& g) {
            if (!found && !g.ranges.empty()) {
                found = detail::makeRef(src, layer.name, g.path, g.ranges.front());
            }
        });
        if (found) {
            return found;
        }
    }
    return std::nullopt;
}

std::optional<model::RangeRef> DataSelectionModel::fastestOf(const model::DataSource& src) const {
    const std::string layer(model::canonicalLayerName(hasMain() && snapshot_.main.src == src.id
                                                          ? std::string_view(snapshot_.main.layer)
                                                          : model::kNormalLayer));
    const model::RangeGroup* g = detail::groupOf(src, layer, detail::kLapsGroup);
    if (g == nullptr) {
        return std::nullopt;
    }
    const model::Range* fastest = analysis::fastestLap(std::span<const model::Range>(g->ranges));
    if (fastest == nullptr) {
        return std::nullopt;
    }
    return detail::makeRef(src, layer, detail::kLapsGroup, *fastest);
}

std::optional<model::RangeRef> DataSelectionModel::initialMainOf(const model::DataSource& src) const {
    if (selectFastestOnLoad_) {
        if (auto f = fastestOf(src)) {
            return f;
        }
    }
    const auto shown = displayedRanges(src.id);
    if (!shown.empty()) {
        return shown.front();
    }
    return firstRangeOf(src);
}

// ---- slot bookkeeping -----------------------------------------------------------------------------------

bool DataSelectionModel::removeFromSlots(const model::RangeRef& r, Changes& what) {
    bool removed = false;
    if (snapshot_.overlay0 && *snapshot_.overlay0 == r) {
        snapshot_.overlay0.reset();
        what |= Change::Overlay0;
        removed = true;
    }
    const auto it = std::find(snapshot_.overlays.begin(), snapshot_.overlays.end(), r);
    if (it != snapshot_.overlays.end()) {
        snapshot_.overlays.erase(it); // compact renumbering in selection order (D-017)
        what |= Change::Overlays;
        removed = true;
    }
    return removed;
}

void DataSelectionModel::checkInvariants() const {
#if ESSES_ASSERTS_ENABLED
    const plot::SelectionSnapshot& s = snapshot_;
    if (hasMain()) {
        ESSES_ASSERT_MSG(!(s.overlay0 && *s.overlay0 == s.main), "Main is Overlay 0");
        ESSES_ASSERT_MSG(std::find(s.overlays.begin(), s.overlays.end(), s.main) == s.overlays.end(),
                         "Main is a numbered overlay");
    }
    if (s.overlay0) {
        ESSES_ASSERT_MSG(std::find(s.overlays.begin(), s.overlays.end(), *s.overlay0) == s.overlays.end(),
                         "Overlay 0 is a numbered overlay");
    }
    for (std::size_t i = 0; i < s.overlays.size(); ++i) {
        for (std::size_t j = i + 1; j < s.overlays.size(); ++j) {
            ESSES_ASSERT_MSG(!(s.overlays[i] == s.overlays[j]), "a range holds two overlay numbers");
        }
    }
    bool anyRange = false;
    for (const auto& src : sources_) {
        anyRange = anyRange || firstRangeOf(*src).has_value();
    }
    ESSES_ASSERT_MSG(hasMain() == anyRange, "exactly one Main exists while any range exists");
#endif
}

void DataSelectionModel::commit(Changes what) {
    if (what == Changes()) {
        return;
    }
    ++snapshot_.version;
    checkInvariants();
    if (session_ != nullptr) {
        session_->setLastSelection(selectionKeys(session_->folder().value_or(std::filesystem::path{})));
    }
    Q_EMIT changed(snapshot_, what);
}

void DataSelectionModel::readProjectOptions() {
    if (session_ == nullptr) {
        return;
    }
    const JsonDoc& options = session_->projectFile().options;
    if (!options.isObject()) {
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(options.compact())).object();
    if (const QJsonValue v = o.value(QStringLiteral("fastLapTolerancePct")); v.isDouble()) {
        setFastLapTolerancePct(v.toDouble());
    }
    const QJsonObject general = o.value(QStringLiteral("general")).toObject();
    if (const QJsonValue v = general.value(QStringLiteral("selectFastestOnLoad")); v.isBool()) {
        selectFastestOnLoad_ = v.toBool();
    }
}

// ---- 06 section 5.2 operations --------------------------------------------------------------------------

void DataSelectionModel::setMain(const model::RangeRef& r) {
    const model::Range* range = resolve(r);
    if (range == nullptr) {
        Q_EMIT hint(tr("The range is not part of an open source."));
        return;
    }
    if (hasMain() && r == snapshot_.main) {
        return;
    }
    Changes what = Change::Main;
    removeFromSlots(r, what); // D-109: one slot per range
    snapshot_.main = r;
    snapshot_.main.layer = std::string(model::canonicalLayerName(r.layer));
    snapshot_.main.name = range->name;
    commit(what);
}

void DataSelectionModel::toggleOverlay0(const model::RangeRef& r) {
    const model::Range* range = resolve(r);
    if (range == nullptr) {
        Q_EMIT hint(tr("The range is not part of an open source."));
        return;
    }
    if (hasMain() && r == snapshot_.main) {
        Q_EMIT hint(tr("The Main range cannot also be Overlay 0."));
        return;
    }
    Changes what = Change::Overlay0;
    if (snapshot_.overlay0 && *snapshot_.overlay0 == r) {
        snapshot_.overlay0.reset();
    } else {
        removeFromSlots(r, what);
        model::RangeRef ref = r;
        ref.layer = std::string(model::canonicalLayerName(r.layer));
        ref.name = range->name;
        snapshot_.overlay0 = std::move(ref);
    }
    commit(what);
}

void DataSelectionModel::toggleOverlay(const model::RangeRef& r) {
    const model::Range* range = resolve(r);
    if (range == nullptr) {
        Q_EMIT hint(tr("The range is not part of an open source."));
        return;
    }
    if (hasMain() && r == snapshot_.main) {
        Q_EMIT hint(tr("The Main range cannot also be an overlay."));
        return;
    }
    if (snapshot_.overlay0 && *snapshot_.overlay0 == r) {
        Q_EMIT hint(tr("The range is already Overlay 0."));
        return;
    }
    const auto it = std::find(snapshot_.overlays.begin(), snapshot_.overlays.end(), r);
    if (it != snapshot_.overlays.end()) {
        snapshot_.overlays.erase(it); // compact renumbering (D-017)
    } else {
        model::RangeRef ref = r;
        ref.layer = std::string(model::canonicalLayerName(r.layer));
        ref.name = range->name;
        snapshot_.overlays.push_back(std::move(ref));
    }
    commit(Change::Overlays);
}

void DataSelectionModel::toggleOverlaysHidden() {
    snapshot_.overlaysHidden = !snapshot_.overlaysHidden;
    commit(Change::Hidden);
}

void DataSelectionModel::clearOverlays() {
    Changes what;
    if (snapshot_.overlay0) {
        snapshot_.overlay0.reset();
        what |= Change::Overlay0;
    }
    if (!snapshot_.overlays.empty()) {
        snapshot_.overlays.clear();
        what |= Change::Overlays;
    }
    if (snapshot_.overlaysHidden) {
        snapshot_.overlaysHidden = false;
        what |= Change::Hidden;
    }
    commit(what);
}

void DataSelectionModel::swapMainOverlay0() {
    if (!snapshot_.overlay0 || !hasMain()) {
        Q_EMIT hint(tr("Swap needs an Overlay 0."));
        return;
    }
    std::swap(snapshot_.main, *snapshot_.overlay0);
    commit(Change::Main | Change::Overlay0);
}

void DataSelectionModel::stepMain(int direction) {
    if (!hasMain()) {
        return;
    }
    const std::vector<model::RangeRef> order =
        displayedRanges(snapshot_.main.src, snapshot_.main.layer, snapshot_.main.groupPath);
    const auto it = std::find(order.begin(), order.end(), snapshot_.main);
    if (it == order.end()) {
        return;
    }
    const auto pos = static_cast<std::ptrdiff_t>(it - order.begin()) + direction;
    if (pos < 0 || pos >= static_cast<std::ptrdiff_t>(order.size())) {
        return; // no wrap (D-110)
    }
    setMain(order[static_cast<std::size_t>(pos)]);
}

void DataSelectionModel::nextRange() {
    stepMain(+1);
}

void DataSelectionModel::prevRange() {
    stepMain(-1);
}

void DataSelectionModel::selectFastest() {
    const auto src = hasMain() ? source(snapshot_.main.src) : (sources_.empty() ? nullptr : sources_.front());
    const std::optional<model::RangeRef> fastest = src == nullptr ? std::nullopt : fastestOf(*src);
    if (!fastest) {
        Q_EMIT hint(tr("No lap of the Main source can be the fastest lap."));
        return;
    }
    setMain(*fastest);
}

void DataSelectionModel::selectAllFast() {
    const auto src = hasMain() ? source(snapshot_.main.src) : (sources_.empty() ? nullptr : sources_.front());
    const std::optional<model::RangeRef> fastest = src == nullptr ? std::nullopt : fastestOf(*src);
    if (!fastest) {
        Q_EMIT hint(tr("No lap of the Main source can be the fastest lap."));
        return;
    }
    const model::RangeGroup* g = detail::groupOf(*src, fastest->layer, fastest->groupPath);
    if (g == nullptr) {
        return;
    }
    Changes what;
    if (!hasMain() || !(snapshot_.main == *fastest)) {
        what |= Change::Main;
        removeFromSlots(*fastest, what);
        snapshot_.main = *fastest;
    }
    std::vector<model::RangeRef> overlays;
    for (const model::Range* lap :
         analysis::fastLapsWithin(std::span<const model::Range>(g->ranges), fastLapTolerancePct_)) {
        model::RangeRef ref = detail::makeRef(*src, fastest->layer, fastest->groupPath, *lap);
        if (ref == snapshot_.main || (snapshot_.overlay0 && *snapshot_.overlay0 == ref)) {
            continue; // Overlay 0 is untouched; one slot per range (D-109)
        }
        overlays.push_back(std::move(ref));
    }
    if (!sameSlots(overlays, snapshot_.overlays)) {
        snapshot_.overlays = std::move(overlays);
        what |= Change::Overlays;
    }
    commit(what);
}

void DataSelectionModel::setSingleColourMain(bool on) {
    if (snapshot_.singleColourMain == on) {
        return;
    }
    snapshot_.singleColourMain = on;
    commit(Change::Colour);
}

void DataSelectionModel::setTrusted(const model::RangeRef& r, bool trusted) {
    const auto src = source(r.src);
    const model::Range* range = src == nullptr ? nullptr : detail::findRange(*src, r);
    if (range == nullptr) {
        Q_EMIT hint(tr("The range is not part of an open source."));
        return;
    }
    if (range->trusted == trusted) {
        return;
    }
    if (!sidecarCommit_) {
        Q_EMIT hint(tr("Trust cannot be changed: this source cannot be edited."));
        return;
    }
    model::SidecarDocument doc = src->sidecar;
    const std::string layer(model::canonicalLayerName(r.layer));
    model::RangeOverride ov;
    ov.groupPath = r.groupPath;
    if (!range->startMarker.isNil()) {
        ov.startMarker = range->startMarker;
    }
    ov.name = range->name;
    if (doc.hasLayer(layer)) {
        for (const model::RangeOverride& existing : doc.layer(layer).rangeOverrides) {
            const bool same = existing.groupPath == ov.groupPath &&
                              (ov.startMarker ? existing.startMarker == ov.startMarker
                                              : (!existing.startMarker && existing.name == ov.name));
            if (same) {
                ov = existing; // keep its offsets
                break;
            }
        }
    }
    ov.trusted = trusted;
    doc.setRangeOverride(layer, std::move(ov));
    sidecarCommit_(src, std::move(doc));
}

void DataSelectionModel::setFilter(const model::SourceId& source, const FilterSpec& spec) {
    const bool flagWasSet = applyToAllSources_;
    applyToAllSources_ = false; // a later per-source change clears the flag (06 section 5.4)
    if (filter(source) == spec && !flagWasSet) {
        return;
    }
    filters_[source] = spec;
    Q_EMIT filterChanged(source);
}

void DataSelectionModel::applyFilterToAllSources() {
    const FilterSpec spec = hasMain() ? filter(snapshot_.main.src) : FilterSpec{};
    applyToAllSources_ = true;
    for (const auto& s : sources_) {
        if (!(filter(s->id) == spec)) {
            filters_[s->id] = spec;
            Q_EMIT filterChanged(s->id);
        }
    }
}

// ---- sources --------------------------------------------------------------------------------------------

void DataSelectionModel::sourcePublished(std::shared_ptr<const model::DataSource> src) {
    if (src == nullptr) {
        return;
    }
    if (source(src->id) != nullptr) {
        sourceReplaced(std::move(src));
        return;
    }
    if (applyToAllSources_ && hasMain()) {
        filters_[src->id] = filter(snapshot_.main.src);
    }
    sources_.push_back(src);
    Q_EMIT sourcesChanged();
    if (!hasMain()) {
        if (const auto main = initialMainOf(*src)) {
            snapshot_.main = *main;
            commit(Change::Main | Change::Sources);
        }
    }
}

std::optional<model::RangeRef> DataSelectionModel::reResolve(const model::DataSource& src,
                                                             const model::RangeRef& r) const {
    const model::RangeGroup* g = detail::groupOf(src, r.layer, r.groupPath);
    if (g == nullptr) {
        return std::nullopt;
    }
    for (const model::Range& range : g->ranges) {
        if (range.id == r.range) {
            return detail::makeRef(src, r.layer, r.groupPath, range);
        }
    }
    // Fallback: the same lap number (06 section 5.6), read from the snapshot being replaced.
    const auto old = source(r.src);
    const model::Range* before = old == nullptr ? nullptr : detail::findRange(*old, r);
    if (before != nullptr && before->kind == model::RangeKind::Lap) {
        for (const model::Range& range : g->ranges) {
            if (range.kind == model::RangeKind::Lap && range.number == before->number &&
                range.lapClass == before->lapClass) {
                return detail::makeRef(src, r.layer, r.groupPath, range);
            }
        }
    }
    return std::nullopt;
}

void DataSelectionModel::sourceReplaced(std::shared_ptr<const model::DataSource> src) {
    if (src == nullptr) {
        return;
    }
    const auto pos =
        std::find_if(sources_.begin(), sources_.end(), [&](const auto& s) { return s->id == src->id; });
    if (pos == sources_.end()) {
        sourcePublished(std::move(src));
        return;
    }
    Changes what = Change::Sources;
    QStringList dropped;
    // Every slot of this source is re-resolved against the new snapshot (the old one is still in sources_).
    const auto reslot = [&](const model::RangeRef& r) -> std::optional<model::RangeRef> {
        if (r.src != src->id) {
            return r;
        }
        auto resolved = reResolve(*src, r);
        if (!resolved) {
            dropped << QString::fromStdString(r.name);
        }
        return resolved;
    };
    const bool hadMain = hasMain();
    std::optional<model::RangeRef> main = hadMain ? reslot(snapshot_.main) : std::nullopt;
    std::optional<model::RangeRef> overlay0;
    if (snapshot_.overlay0) {
        overlay0 = reslot(*snapshot_.overlay0);
        if (!overlay0 || !(*overlay0 == *snapshot_.overlay0) || overlay0->name != snapshot_.overlay0->name) {
            what |= Change::Overlay0;
        }
    }
    std::vector<model::RangeRef> overlays;
    for (const model::RangeRef& o : snapshot_.overlays) {
        if (auto r = reslot(o)) {
            if (!(*r == o) || r->name != o.name) {
                what |= Change::Overlays;
            }
            overlays.push_back(std::move(*r));
        } else {
            what |= Change::Overlays;
        }
    }
    *pos = src;
    if (hadMain && (!main || !(*main == snapshot_.main) || main->name != snapshot_.main.name)) {
        what |= Change::Main;
    }
    if (!main) {
        main = initialMainOf(*src); // exactly one Main exists while any range exists
        if (main && !hadMain) {
            what |= Change::Main;
        }
    }
    snapshot_.main = main.value_or(model::RangeRef{});
    snapshot_.overlay0 = std::move(overlay0);
    // A fallback may land two slots on one range: the higher slot keeps it (D-109).
    if (snapshot_.overlay0 && hasMain() && *snapshot_.overlay0 == snapshot_.main) {
        snapshot_.overlay0.reset();
        what |= Change::Overlay0;
    }
    std::vector<model::RangeRef> unique;
    for (model::RangeRef& o : overlays) {
        if ((hasMain() && o == snapshot_.main) || (snapshot_.overlay0 && o == *snapshot_.overlay0) ||
            std::find(unique.begin(), unique.end(), o) != unique.end()) {
            what |= Change::Overlays;
            continue;
        }
        unique.push_back(std::move(o));
    }
    snapshot_.overlays = std::move(unique);
    for (const QString& name : dropped) {
        Q_EMIT hint(tr("Selection: %1 no longer exists and left its slot.").arg(name));
    }
    Q_EMIT sourcesChanged();
    commit(what);
}

void DataSelectionModel::closeSource(const model::SourceId& id) {
    const auto pos =
        std::find_if(sources_.begin(), sources_.end(), [&](const auto& s) { return s->id == id; });
    if (pos == sources_.end()) {
        return;
    }
    const auto index = static_cast<std::size_t>(pos - sources_.begin());
    Changes what = Change::Sources;
    if (snapshot_.overlay0 && snapshot_.overlay0->src == id) {
        snapshot_.overlay0.reset();
        what |= Change::Overlay0;
    }
    const auto removed = std::remove_if(snapshot_.overlays.begin(), snapshot_.overlays.end(),
                                        [&](const model::RangeRef& r) { return r.src == id; });
    if (removed != snapshot_.overlays.end()) {
        snapshot_.overlays.erase(removed, snapshot_.overlays.end());
        what |= Change::Overlays;
    }
    const bool mainLeaves = hasMain() && snapshot_.main.src == id;
    sources_.erase(pos);
    filters_.erase(id);
    if (mainLeaves) {
        what |= Change::Main;
        snapshot_.main = model::RangeRef{};
        // The next source in Data-window order, else the previous ones (06 section 5.2 closeSource).
        std::vector<std::size_t> order;
        for (std::size_t i = index; i < sources_.size(); ++i) {
            order.push_back(i);
        }
        for (std::size_t i = index; i-- > 0;) {
            order.push_back(i);
        }
        for (const std::size_t i : order) {
            if (const auto main = initialMainOf(*sources_[i])) {
                removeFromSlots(*main, what);
                snapshot_.main = *main;
                break;
            }
        }
    }
    Q_EMIT sourcesChanged();
    commit(what);
}

void DataSelectionModel::closeAllSources() {
    if (sources_.empty()) {
        return;
    }
    Changes what = Change::Sources;
    if (hasMain()) {
        what |= Change::Main;
    }
    if (snapshot_.overlay0) {
        what |= Change::Overlay0;
    }
    if (!snapshot_.overlays.empty()) {
        what |= Change::Overlays;
    }
    sources_.clear();
    filters_.clear();
    snapshot_.main = model::RangeRef{};
    snapshot_.overlay0.reset();
    snapshot_.overlays.clear();
    Q_EMIT sourcesChanged();
    commit(what);
}

// ---- persistence ----------------------------------------------------------------------------------------

project::SelectionKeys DataSelectionModel::selectionKeys(const std::filesystem::path& projectFolder) const {
    project::SelectionKeys keys;
    // std::filesystem::relative touches the file system (weakly_canonical): once per (source, folder), not
    // on every selection change (commit() runs this on the GUI thread for N/P, ENTER, Q ...).
    const std::string folderKey = genericUtf8(projectFolder);
    const auto pathOf = [&](const model::DataSource& src) {
        const std::string cacheKey = src.id.hex() + '\n' + genericUtf8(src.prov.path) + '\n' + folderKey;
        auto it = relativePaths_.find(cacheKey);
        if (it == relativePaths_.end()) {
            it = relativePaths_.emplace(cacheKey, relativeOrAbsolute(src.prov.path, projectFolder)).first;
        }
        return it->second;
    };
    const auto keyFor = [&](const model::RangeRef& r) -> std::optional<project::RangeKey> {
        const auto src = source(r.src);
        if (src == nullptr) {
            return std::nullopt;
        }
        return keyOf(*src, r, pathOf(*src));
    };
    if (hasMain()) {
        keys.main = keyFor(snapshot_.main);
    }
    if (snapshot_.overlay0) {
        keys.overlay0 = keyFor(*snapshot_.overlay0);
    }
    for (const model::RangeRef& o : snapshot_.overlays) {
        if (auto k = keyFor(o)) {
            keys.overlays.push_back(std::move(*k));
        }
    }
    keys.overlaysHidden = snapshot_.overlaysHidden;
    keys.singleColourMain = snapshot_.singleColourMain;
    return keys;
}

void DataSelectionModel::restore(const project::SelectionKeys& keys) {
    const std::filesystem::path folder =
        session_ != nullptr ? session_->folder().value_or(std::filesystem::path{}) : std::filesystem::path{};
    QStringList dropped;
    const auto resolveKey = [&](const project::RangeKey& k) -> std::optional<model::RangeRef> {
        const std::string layer(model::canonicalLayerName(k.layer));
        // 1. (hash, layer, groupPath, startMarker)
        for (const auto& src : sources_) {
            if (k.source.hash != std::string(kHashPrefix) + src->id.hex() || k.startMarker.empty()) {
                continue;
            }
            if (const model::RangeGroup* g = detail::groupOf(*src, layer, k.groupPath)) {
                for (const model::Range& r : g->ranges) {
                    if (!r.startMarker.isNil() && r.startMarker.str() == k.startMarker) {
                        return detail::makeRef(*src, layer, k.groupPath, r);
                    }
                }
            }
        }
        // 2. (path, layer, groupPath, name)
        std::filesystem::path keyPath = pathFromUtf8(k.source.path);
        if (keyPath.is_relative() && !folder.empty()) {
            keyPath = folder / keyPath;
        }
        std::error_code ec;
        const std::filesystem::path wanted = std::filesystem::weakly_canonical(keyPath, ec);
        for (const auto& src : sources_) {
            std::error_code ec2;
            const std::filesystem::path have = std::filesystem::weakly_canonical(src->prov.path, ec2);
            if (ec || ec2 || have != wanted) {
                continue;
            }
            if (const model::RangeGroup* g = detail::groupOf(*src, layer, k.groupPath)) {
                for (const model::Range& r : g->ranges) {
                    if (r.name == k.name) {
                        return detail::makeRef(*src, layer, k.groupPath, r);
                    }
                }
            }
        }
        dropped << QString::fromStdString(k.name);
        return std::nullopt;
    };

    plot::SelectionSnapshot next = snapshot_;
    if (keys.main) {
        if (auto m = resolveKey(*keys.main)) {
            next.main = *m;
        }
    }
    next.overlay0.reset();
    if (keys.overlay0) {
        if (auto o = resolveKey(*keys.overlay0); o && !(*o == next.main)) {
            next.overlay0 = *o;
        }
    }
    next.overlays.clear();
    for (const project::RangeKey& k : keys.overlays) {
        if (auto o = resolveKey(k)) {
            if (*o == next.main || (next.overlay0 && *o == *next.overlay0) ||
                std::find(next.overlays.begin(), next.overlays.end(), *o) != next.overlays.end()) {
                continue;
            }
            next.overlays.push_back(*o);
        }
    }
    next.overlaysHidden = keys.overlaysHidden;
    next.singleColourMain = keys.singleColourMain;
    Changes what;
    if (!(next.main == snapshot_.main)) {
        what |= Change::Main;
    }
    if (next.overlay0 != snapshot_.overlay0) {
        what |= Change::Overlay0;
    }
    if (!sameSlots(next.overlays, snapshot_.overlays)) {
        what |= Change::Overlays;
    }
    if (next.overlaysHidden != snapshot_.overlaysHidden) {
        what |= Change::Hidden;
    }
    if (next.singleColourMain != snapshot_.singleColourMain) {
        what |= Change::Colour;
    }
    snapshot_ = std::move(next);
    for (const QString& name : dropped) {
        Q_EMIT hint(tr("Selection restore: %1 was not found and was dropped.").arg(name));
    }
    commit(what);
}

} // namespace esses::qt
