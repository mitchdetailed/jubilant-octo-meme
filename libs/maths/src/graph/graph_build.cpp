// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// MathGraph::Impl::rebuild() (03 sections 3.3, 4.2-4.4, 7.1-7.2, 8; D-013, D-022, D-140):
//   1. nodes from the documents in chain-building order: built-in registrations (a `builtin` item of the
//      local or a global module overrides the registration's settings for this source), then the local
//      module, the global modules in project order and the transient module. A module's condition is
//      evaluated over the source's details (and the module's constants); the first enabled item per name in a
//      module wins (W-DUP-IGNORED on later ones).
//   2. chains: per case-folded name, every element that is not ignored, in that order.
//   3. build (depth first, so an item compiles after what it reads): references resolve in the Scope order
//      of 03 section 3.3 (mappings, aliases, the chain head (self-maths: the enabled element before the
//      requester), the logged channel, a detail, a constant); a reference to a node still on the build
//      stack closes a cycle (E-CYCLE on every node of it, with the path). Each item is compiled against a
//      FrozenScope of what its references resolved to, unless its compile signature (definition, compat
//      switches, resolved inputs) equals the previous build's.
//   4. versions (03 section 7.2) and the runtime state carried from the previous build where the identity
//      and version hold.

#include "../lang/checker_internal.hpp"
#include "graph_internal.hpp"

#include <esses/base/casefold.hpp>
#include <esses/base/hash.hpp>
#include <esses/maths/parser.hpp>
#include <esses/maths/type_checker.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/layer.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace esses::maths {

namespace {

using graph::InputRef;
using graph::MathNode;
using graph::Off;
using graph::updateHash;
using model::ResolutionStep;

constexpr std::string_view kArrow = "\xE2\x86\x92"; // U+2192
constexpr std::string_view kSkipped = "skipped: ";

std::string numberText(double v, bool decimal) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os.precision(17);
    os << v;
    std::string s = os.str();
    if (decimal && s.find_first_of(".eEn") == std::string::npos) {
        s += ".0";
    }
    return s;
}

void hashDouble(Hash128Stream& h, double v) {
    h.updateU64(std::bit_cast<std::uint64_t>(v));
}

void hashAxis(Hash128Stream& h, const TableSpec::Axis& a) {
    h.updatePart(a.expr);
    h.updateU64(a.unit);
    h.updateU64(a.breakpoints.size());
    for (const double b : a.breakpoints) {
        hashDouble(h, b);
    }
}

// Every member of an item that decides what it computes (not comment, enable flag, display or x-i2).
void hashItem(Hash128Stream& h, const MathItem& item) {
    h.updatePart(graph::itemKindName(item.kind));
    h.updatePart(caseFold(item.name));
    h.updatePart(item.expr);
    h.updateU64(item.rate.automatic ? 1u : 0u);
    h.updateU64(item.rate.rate.num);
    h.updateU64(item.rate.rate.den);
    h.updateU64(item.quantity);
    h.updateU64(item.resultUnit);
    h.updateU64(static_cast<std::uint64_t>(item.interp));
    h.updatePart(item.enumeration.value_or(std::string("\x01")));
    h.updateU64(item.window.points ? static_cast<std::uint64_t>(*item.window.points) : ~std::uint64_t{0});
    hashDouble(h, item.window.seconds.value_or(-1.0));
    hashDouble(h, item.scale);
    hashDouble(h, item.offset.v);
    h.updateU64(item.offset.unit);
    hashAxis(h, item.table.x);
    h.updateU64(item.table.y ? 1u : 0u);
    if (item.table.y) {
        hashAxis(h, *item.table.y);
    }
    h.updateU64(item.table.valuesUnit);
    h.updateU64(item.table.values.size());
    for (const double v : item.table.values) {
        hashDouble(h, v);
    }
    h.updateU64(item.table.rowMajor ? 1u : 0u);
    h.updateU64(static_cast<std::uint64_t>(item.table.interpolation));
    h.updateU64(static_cast<std::uint64_t>(item.table.outOfRange));
    h.updatePart(item.precision.value_or(std::string()));
}

// The function aliases of the compat switches (part of every compile signature and built-in definition).
std::string aliasesText(const MathsCompat& c) {
    std::string s;
    for (const auto& [from, to] : c.functionAliases) {
        s += '|';
        s += from;
        s += '=';
        s += to;
    }
    return s;
}

// Every switch: the key of the parse cache. unaryMinusAbovePower changes how a text parses, and the canonical
// text of the parsed AST (Ast::sourceHash, in the compile signature through the definition hash) differs
// between the two settings exactly for the expressions with a sign and '^' (03 section 2.3), so it is not
// hashed into the signatures themselves.
std::string compatText(const MathsCompat& c) {
    return (c.unaryMinusAbovePower ? "pow1" : "pow0") + aliasesText(c);
}

model::ChannelId mathsChannelId(const model::SourceId& src, std::string_view layer, const MathNode& n) {
    if (n.origin == MathModule::Origin::Local) {
        return model::channelIdMathsLocal(src, layer, n.name);
    }
    return model::channelIdMathsModule(src, layer, n.name, n.moduleId);
}

bool integerChannel(const model::Channel& ch) {
    return ch.desc.dtype == store::Dtype::I32 || ch.desc.kind == model::SampleKind::Integer ||
           ch.desc.kind == model::SampleKind::Enum || ch.desc.kind == model::SampleKind::Bool ||
           ch.desc.kind == model::SampleKind::Bitfield;
}

// What one name resolved to.
struct Found {
    enum class Kind : std::uint8_t { Missing, Logged, Maths, Detail, Constant };
    Kind kind = Kind::Missing;
    const model::Channel* channel = nullptr;
    std::size_t node = 0;
    const model::Detail* detail = nullptr;
    const MathConstant* constant = nullptr;
    std::string error; // "Error: alias cycle ..." stops the resolution
    bool self = false; // read through the requester's own chain (self-maths)
};

class Builder {
public:
    Builder(MathGraph::Impl& g, const std::unordered_map<std::string, std::unique_ptr<MathNode>>& olds)
        : g_(g), layer_(g.dataLayer()), units_(g.units.get()), compat_(compatText(g.compat)),
          aliases_(aliasesText(g.compat)), olds_(olds) {}

    // Step 1: the nodes from the documents.
    void collect() {
        for (std::size_t r = 0; r < g_.builtins.size(); ++r) {
            collectBuiltin(r);
        }
        if (g_.local) {
            collectModule(*g_.local, MathModule::Origin::Local, 0);
        }
        for (std::size_t m = 0; m < g_.globals.size(); ++m) {
            collectModule(g_.globals[m], MathModule::Origin::Global, m);
        }
        if (g_.transient) {
            collectModule(*g_.transient, MathModule::Origin::Transient, 0);
        }
    }

    // Step 2: chains and the output order.
    void chain() {
        g_.chains.clear();
        g_.outputOrder.clear();
        for (std::size_t i = 0; i < g_.nodes.size(); ++i) {
            MathNode& n = *g_.nodes[i];
            if (n.ignored) {
                n.chainIndex = NodeId::kIgnored;
                continue;
            }
            auto [it, inserted] = g_.chains.try_emplace(n.key.nameCf);
            if (inserted) {
                g_.outputOrder.push_back(n.key.nameCf);
            }
            n.chainIndex = static_cast<std::uint32_t>(it->second.size());
            it->second.push_back(i);
        }
    }

    // Step 3. Nodes build in an approximate dependency order first (an item after the chains of the names
    // its text references), so the depth-first build rarely recurses: aliases and mappings are the only
    // edges the order does not foresee.
    void buildAll() {
        order_ = approximateOrder();
        for (const std::size_t i : order_) {
            build(i);
        }
    }

    const std::vector<std::size_t>& order() const noexcept { return order_; }

private:
    // The referenced names of an item's text (its own name for filter and scale_offset items).
    std::vector<std::string> namesRead(const MathNode& n) const {
        std::vector<std::string> out;
        if (n.builtin) {
            for (const std::string& up : n.def.upstream) {
                out.push_back(caseFold(up));
            }
            return out;
        }
        std::string text;
        switch (n.kind) {
        case ItemKind::Expression:
            text = n.item.expr;
            break;
        case ItemKind::Table2D:
            text = n.item.table.x.expr;
            break;
        case ItemKind::Filter:
        case ItemKind::ScaleOffset:
            out.push_back(n.key.nameCf);
            return out;
        default:
            return out;
        }
        const graph::ParsedText& parsed = parse(text);
        if (parsed.ast != nullptr) {
            for (const auto& node : parsed.ast->nodes) {
                if (node.kind == NodeKind::ChannelRef) {
                    out.push_back(caseFold(node.text));
                } else if (node.kind == NodeKind::Call && node.text == "integrate_dist") {
                    out.emplace_back("corr dist");
                }
            }
        }
        return out;
    }

    // Post-order of an iterative depth-first walk over "reads an element of the chain of".
    std::vector<std::size_t> approximateOrder() const {
        const std::size_t count = g_.nodes.size();
        std::vector<std::vector<std::size_t>> edges(count);
        for (std::size_t i = 0; i < count; ++i) {
            const MathNode& n = *g_.nodes[i];
            if (!n.active()) {
                continue;
            }
            for (const std::string& nameCf : namesRead(n)) {
                const auto it = g_.chains.find(nameCf);
                if (it == g_.chains.end()) {
                    continue;
                }
                for (const std::size_t j : it->second) {
                    if (j != i && (nameCf != n.key.nameCf || g_.nodes[j]->chainIndex < n.chainIndex)) {
                        edges[i].push_back(j);
                    }
                }
            }
        }
        std::vector<std::size_t> out;
        out.reserve(count);
        std::vector<std::uint8_t> state(count, 0);
        std::vector<std::pair<std::size_t, std::size_t>> stack;
        for (std::size_t root = 0; root < count; ++root) {
            if (state[root] != 0) {
                continue;
            }
            stack.emplace_back(root, 0);
            state[root] = 1;
            while (!stack.empty()) {
                auto& [i, next] = stack.back();
                if (next < edges[i].size()) {
                    const std::size_t j = edges[i][next++];
                    if (state[j] == 0) {
                        state[j] = 1;
                        stack.emplace_back(j, 0);
                    }
                    continue;
                }
                state[i] = 2;
                out.push_back(i);
                stack.pop_back();
            }
        }
        return out;
    }

    // ---- step 1 ---------------------------------------------------------------------------------------

    const MathItem* builtinOverride(std::string_view moduleId) const {
        const auto findIn = [&](const MathModule& m) -> const MathItem* {
            for (const MathItem& item : m.items) {
                if (item.kind == ItemKind::Builtin && item.moduleId == moduleId) {
                    return &item;
                }
            }
            return nullptr;
        };
        if (g_.local) {
            if (const MathItem* item = findIn(*g_.local)) {
                return item;
            }
        }
        for (const MathModule& m : g_.globals) {
            if (m.enabled && g_.moduleEnabled(m.id) && conditionHolds(m)) {
                if (const MathItem* item = findIn(m)) {
                    return item;
                }
            }
        }
        return nullptr;
    }

    void collectBuiltin(std::size_t r) {
        graph::BuiltinReg& reg = g_.builtins[r];
        const MathItem* override = builtinOverride(reg.id);
        const std::vector<BuiltinOutput>* outputs = &reg.outputs;
        std::optional<Error> expandError;
        Diagnostics expandDiags;
        std::string settingsText = reg.settings.compact();
        if (override != nullptr && !override->settings.isNull() &&
            override->settings.compact() != settingsText && reg.module != nullptr && units_ != nullptr) {
            const std::string text = override->settings.compact();
            if (reg.overrideText != text || !reg.overrideOutputs) {
                reg.overrideText = text;
                reg.overrideDiags.clear();
                reg.overrideOutputs = reg.module->expand(override->settings, *units_, reg.overrideDiags);
            }
            expandDiags = reg.overrideDiags;
            if (*reg.overrideOutputs) {
                outputs = &**reg.overrideOutputs;
                settingsText = text;
            } else {
                expandError = reg.overrideOutputs->error();
            }
        }
        Off off = Off::None;
        if (!g_.moduleEnabled(reg.id)) {
            off = Off::Module;
        } else if (override != nullptr && !override->enabled &&
                   (reg.module == nullptr || reg.module->disabledItemDisablesOutputs())) {
            off = Off::Item;
        }
        for (std::size_t o = 0; o < outputs->size(); ++o) {
            const BuiltinOutput& out = (*outputs)[o];
            auto n = std::make_unique<MathNode>();
            n->builtin = true;
            n->builtinReg = r;
            n->itemIndex = o;
            n->name = out.name;
            n->key = ChannelKey::make(g_.sourceId(), g_.layer, out.name);
            n->moduleId = reg.id;
            n->origin = MathModule::Origin::Builtin;
            n->kind = ItemKind::Builtin;
            n->identity = "b\x1F" + reg.id + "\x1F" + n->key.nameCf;
            n->def = out;
            n->settingsText = settingsText;
            n->off = off;
            if (off == Off::None) {
                for (const std::string& req : out.required) {
                    if (layer_ == nullptr ||
                        (layer_->find(req) == nullptr && layer_->findLogged(req) == nullptr)) {
                        n->off = Off::Missing;
                        n->offSummary = out.missingSummary.empty() ? std::string("Disabled (missing inputs)")
                                                                   : out.missingSummary;
                        break;
                    }
                }
            }
            if (expandError) {
                n->diags = expandDiags;
                n->defError = *expandError;
            }
            g_.nodes.push_back(std::move(n));
        }
    }

    void collectModule(const MathModule& m, MathModule::Origin origin, std::size_t slot) {
        Off moduleOff = Off::None;
        std::string offSummary;
        Diagnostics conditionDiags;
        if (!m.enabled || !g_.moduleEnabled(m.id)) {
            moduleOff = Off::Module;
        } else if (!conditionHolds(m, &conditionDiags)) {
            moduleOff = Off::Condition;
            offSummary = "Disabled (condition)";
        }
        const char originChar = origin == MathModule::Origin::Local       ? 'l'
                                : origin == MathModule::Origin::Transient ? 't'
                                                                          : 'g';
        std::unordered_map<std::string, std::size_t> occurrences;
        std::unordered_map<std::string, std::size_t> enabledSeen;
        for (std::size_t i = 0; i < m.items.size(); ++i) {
            const MathItem& item = m.items[i];
            if (item.kind == ItemKind::Builtin || item.name.empty()) {
                continue; // built-in items override a registration (collectBuiltin); a nameless item is no
                          // output
            }
            auto n = std::make_unique<MathNode>();
            n->name = item.name;
            n->key = ChannelKey::make(g_.sourceId(), g_.layer, item.name);
            n->moduleId = m.id;
            n->origin = origin;
            n->kind = item.kind;
            n->moduleSlot = slot;
            n->itemIndex = i;
            n->item = item;
            if (!n->item.precision && m.precision == "double") {
                n->item.precision = "double"; // the module default (03 section 6.5)
            }
            const std::size_t occurrence = occurrences[n->key.nameCf]++;
            n->identity = std::string(1, originChar) + "\x1F" + m.id + "\x1F" + n->key.nameCf;
            if (occurrence > 0) {
                n->identity += "\x1F" + std::to_string(occurrence);
            }
            n->off = moduleOff;
            n->offSummary = offSummary;
            if (n->off == Off::None && !item.enabled) {
                n->off = Off::Item;
            }
            if (item.enabled && enabledSeen[n->key.nameCf]++ > 0) {
                n->ignored = true;
                Diagnostic d;
                d.code = "W-DUP-IGNORED";
                d.level = Diagnostic::Warning;
                d.message = "'" + item.name + "' is defined earlier in module " + m.id +
                            "; this definition is ignored";
                n->diags.push_back(std::move(d));
            }
            if (moduleOff == Off::Condition) {
                n->diags.insert(n->diags.end(), conditionDiags.begin(), conditionDiags.end());
            }
            g_.nodes.push_back(std::move(n));
        }
    }

    // ---- conditions (03 section 4.3 rule 8) -----------------------------------------------------------

    bool conditionHolds(const MathModule& m, Diagnostics* out = nullptr) const {
        if (!m.condition || m.condition->find_first_not_of(" \t\r\n") == std::string::npos) {
            return true;
        }
        const auto cached = conditions_.find(&m);
        if (cached != conditions_.end()) {
            if (out != nullptr) {
                *out = cached->second.second;
            }
            return cached->second.first;
        }
        Diagnostics diags;
        const bool holds = evaluateCondition(m, diags);
        conditions_.emplace(&m, std::make_pair(holds, diags));
        if (out != nullptr) {
            *out = diags;
        }
        return holds;
    }

    bool evaluateCondition(const MathModule& m, Diagnostics& diags) const {
        const auto conditionError = [&](const std::string& message) {
            Diagnostic d;
            d.code = "E-CONDITION";
            d.level = Diagnostic::Error;
            d.message = "module " + m.id + " condition: " + message;
            diags.push_back(std::move(d));
            return false;
        };
        if (units_ == nullptr) {
            return conditionError("no unit registry");
        }
        const graph::ParsedText& parsed = parse(*m.condition);
        if (parsed.error || parsed.ast == nullptr) {
            return conditionError(parsed.error ? parsed.error->message : std::string("does not parse"));
        }
        graph::FrozenScope scope(g_.units, g_.source != nullptr);
        for (const auto& ref : parsed.ast->nodes) {
            if (ref.kind != NodeKind::ChannelRef) {
                continue;
            }
            const std::string nameCf = caseFold(ref.text);
            std::optional<ScopeEntry> e;
            if (g_.source != nullptr) {
                if (const model::Detail* d = model::findDetail(g_.source->details, ref.text)) {
                    e = detailEntry(*d);
                }
            }
            if (!e) {
                for (const MathConstant& c : m.constants) {
                    if (c.enabled && caseFold(c.name) == nameCf) {
                        e = constantEntry(c);
                        break;
                    }
                }
            }
            if (e) {
                scope.entries.emplace(nameCf, std::move(*e));
            }
        }
        Ast copy = *parsed.ast;
        Diagnostics checkDiags;
        const lang::CheckResult result = lang::checkAst(copy, scope, TypeChecker::Options{}, checkDiags);
        if (hasError(checkDiags)) {
            for (const Diagnostic& d : checkDiags) {
                if (d.level == Diagnostic::Error) {
                    return conditionError(d.message);
                }
            }
        }
        if (!result.reachedRoot || copy.root >= result.info.size() || !result.info[copy.root].known) {
            if (g_.source == nullptr && !hasError(checkDiags)) {
                return true; // without data (esses-cli maths check) a condition over the details is unknown
            }
            return conditionError("not a constant expression over the details of the source");
        }
        return result.info[copy.root].value != 0.0;
    }

    // ---- parsing ----------------------------------------------------------------------------------------

    const graph::ParsedText& parse(const std::string& text) const {
        const std::string key = compat_ + "\x1F" + text;
        auto it = g_.astCache.find(key);
        if (it == g_.astCache.end()) {
            graph::ParsedText p;
            Result<Ast> ast = Parser::parse(text, g_.compat, p.diags);
            if (ast) {
                p.ast = std::make_shared<const Ast>(std::move(ast).value());
            } else {
                p.error = ast.error();
            }
            it = g_.astCache.emplace(key, std::move(p)).first;
        }
        it->second.used = true;
        return it->second;
    }

    // ---- scope entries ----------------------------------------------------------------------------------

    ScopeEntry detailEntry(const model::Detail& d) const {
        ScopeEntry e;
        e.resolvedName = d.id;
        if (const auto* text = std::get_if<std::string>(&d.value)) {
            e.what = ScopeEntry::DetailText;
            e.type.kind = Kind::Text;
            e.text = *text;
        } else if (const auto* dt = std::get_if<model::DateTime>(&d.value)) {
            e.what = ScopeEntry::DetailText;
            e.type.kind = Kind::Text;
            e.text = dt->iso();
        } else if (const auto* q = std::get_if<model::Quantityd>(&d.value)) {
            e.what = ScopeEntry::DetailNumber;
            e.type.kind = Kind::Scalar;
            e.type.unit = units_ != nullptr ? graph::baseUnitOf(q->unit, *units_) : units::kNoUnit;
            e.number = units_ != nullptr ? model::toBase(*q, *units_).value_or(q->v) : q->v;
        }
        return e;
    }

    ScopeEntry constantEntry(const MathConstant& c) const {
        ScopeEntry e;
        e.what = ScopeEntry::Constant;
        e.type.kind = Kind::Scalar;
        e.resolvedName = c.name;
        if (units_ != nullptr && units_->contains(c.value.unit)) {
            e.type.unit = graph::baseUnitOf(c.value.unit, *units_);
            e.number = units_->toBase(c.value.unit)(c.value.v);
        } else {
            e.number = c.value.v;
        }
        return e;
    }

    // ---- resolution (03 section 3.3)
    // -----------------------------------------------------------------------

    const MathModule* moduleOf(const MathNode& n) const {
        switch (n.origin) {
        case MathModule::Origin::Local:
            return g_.local ? &*g_.local : nullptr;
        case MathModule::Origin::Transient:
            return g_.transient ? &*g_.transient : nullptr;
        case MathModule::Origin::Global:
            return n.moduleSlot < g_.globals.size() ? &g_.globals[n.moduleSlot] : nullptr;
        case MathModule::Origin::Builtin:
            return nullptr;
        }
        return nullptr;
    }

    static const MathConstant* constantIn(const MathModule* m, const std::string& nameCf) {
        if (m == nullptr) {
            return nullptr;
        }
        for (const MathConstant& c : m->constants) {
            if (c.enabled && caseFold(c.name) == nameCf) {
                return &c;
            }
        }
        return nullptr;
    }

    // The constant `nameCf` as seen from `requester`: own module, local maths, then the other enabled global
    // modules whose condition holds, in project order (W-CONST-AMBIGUOUS when several define it).
    const MathConstant* constantFor(const std::string& nameCf, const MathNode& requester,
                                    Diagnostics* diags) const {
        const MathModule* own = moduleOf(requester);
        if (const MathConstant* c = constantIn(own, nameCf)) {
            return c;
        }
        if (requester.origin != MathModule::Origin::Local && g_.local) {
            if (const MathConstant* c = constantIn(&*g_.local, nameCf)) {
                return c;
            }
        }
        const MathConstant* first = nullptr;
        std::vector<std::string> definers;
        for (const MathModule& m : g_.globals) {
            if (&m == own || !m.enabled || !g_.moduleEnabled(m.id) || !conditionHolds(m)) {
                continue;
            }
            if (const MathConstant* c = constantIn(&m, nameCf)) {
                if (first == nullptr) {
                    first = c;
                }
                definers.push_back(m.id);
            }
        }
        if (definers.size() > 1 && diags != nullptr) {
            Diagnostic d;
            d.code = "W-CONST-AMBIGUOUS";
            d.level = Diagnostic::Warning;
            d.message = "constant '" + first->name + "' is defined in modules " + definers[0];
            for (std::size_t k = 1; k < definers.size(); ++k) {
                d.message += ", " + definers[k];
            }
            d.message += "; the one in " + definers[0] + " is used";
            diags->push_back(std::move(d));
        }
        return first;
    }

    const model::MappingRow* mappingFor(std::string_view name) const {
        const std::string cf = caseFold(name);
        for (const model::MappingRow& row : g_.mappings.rows) {
            if (caseFold(row.from) == cf) {
                return &row;
            }
        }
        return nullptr;
    }

    const model::AliasRow* aliasFor(std::string_view name) const {
        const std::string cf = caseFold(name);
        for (const model::AliasRow& row : g_.aliases.rows) {
            if (caseFold(row.name) == cf) {
                return &row;
            }
        }
        return nullptr;
    }

    // Makes sure node `j` is built; a node still on the build stack closes a cycle.
    void ensure(std::size_t j) {
        MathNode& t = *g_.nodes[j];
        if (t.buildState == 2) {
            return;
        }
        if (t.buildState == 1) {
            markCycle(j);
            return;
        }
        build(j);
    }

    void markCycle(std::size_t j) {
        const auto pos = std::find(stack_.begin(), stack_.end(), j);
        if (pos == stack_.end()) {
            return;
        }
        const std::vector<std::size_t> cycle(pos, stack_.end());
        for (std::size_t q = 0; q < cycle.size(); ++q) {
            MathNode& n = *g_.nodes[cycle[q]];
            if (n.onCycle) {
                continue;
            }
            n.onCycle = true;
            std::string path;
            for (std::size_t k = 0; k < cycle.size(); ++k) {
                path +=
                    "'" + g_.nodes[cycle[(q + k) % cycle.size()]]->name + "' " + std::string(kArrow) + " ";
            }
            path += "'" + n.name + "'";
            n.cyclePath = std::move(path);
        }
    }

    // The enabled element of chain `nameCf` that `requester` reads: the last one, or for its own name the
    // last one before it.
    std::optional<std::size_t> chainTarget(const std::string& nameCf, std::size_t requester,
                                           bool& self) const {
        const MathNode& req = *g_.nodes[requester];
        const auto it = g_.chains.find(nameCf);
        std::size_t limit = it != g_.chains.end() ? it->second.size() : 0;
        if (!req.ignored && req.key.nameCf == nameCf) {
            self = true;
            limit = std::min<std::size_t>(limit, req.chainIndex);
        }
        if (it == g_.chains.end()) {
            return std::nullopt;
        }
        for (std::size_t k = limit; k-- > 0;) {
            if (g_.nodes[it->second[k]]->active()) {
                return it->second[k];
            }
        }
        return std::nullopt;
    }

    void lookupLogged(std::string_view name, Found& out, std::vector<ResolutionStep>& steps) const {
        if (layer_ == nullptr) {
            return;
        }
        if (const model::Channel* c = layer_->find(name)) {
            steps.push_back(ResolutionStep{ResolutionStep::Kind::CaseFold, std::string(name), c->desc.name,
                                           c->desc.name == name ? std::string("exact") : std::string()});
            if (c->desc.loggedName != c->desc.name && !c->desc.loggedName.empty()) {
                const model::MappingRow* m = mappingFor(c->desc.loggedName);
                if (m != nullptr && ciEquals(m->to, c->desc.name)) {
                    steps.push_back(ResolutionStep{ResolutionStep::Kind::Mapping, c->desc.loggedName,
                                                   c->desc.name, "renamed at load"});
                } else {
                    steps.push_back(ResolutionStep{
                        ResolutionStep::Kind::Dictionary, c->desc.loggedName, c->desc.name, {}});
                }
            }
            out.kind = Found::Kind::Logged;
            out.channel = c;
            return;
        }
        if (const model::Channel* c = layer_->findLogged(name)) {
            steps.push_back(
                ResolutionStep{ResolutionStep::Kind::LoggedName, std::string(name), c->desc.name, {}});
            out.kind = Found::Kind::Logged;
            out.channel = c;
        }
    }

    Found resolve(std::string_view name, std::size_t requester, const model::AliasRow* excluded,
                  std::vector<const model::AliasRow*>& aliasChain, std::vector<ResolutionStep>& steps,
                  Diagnostics& diags) {
        Found out;
        std::string current(name);

        // 1. Mappings: one hop.
        if (const model::MappingRow* m = mappingFor(current); m != nullptr && !ciEquals(m->to, current)) {
            steps.push_back(ResolutionStep{ResolutionStep::Kind::Mapping, current, m->to, {}});
            current = m->to;
        }

        // 2. Aliases; the alias being expanded is invisible to its own candidates (02 section 4.6 rule 17).
        const model::AliasRow* alias = aliasFor(current);
        if (alias != nullptr && alias != excluded) {
            if (std::find(aliasChain.begin(), aliasChain.end(), alias) != aliasChain.end()) {
                std::string cycle;
                for (auto it = std::find(aliasChain.begin(), aliasChain.end(), alias); it != aliasChain.end();
                     ++it) {
                    cycle += "'" + (*it)->name + "' " + std::string(kArrow) + " ";
                }
                cycle += "'" + alias->name + "'";
                out.error = "Error: alias cycle " + cycle;
                steps.push_back(ResolutionStep{ResolutionStep::Kind::Missing, current, {}, out.error});
                return out;
            }
            aliasChain.push_back(alias);
            for (const model::AliasCandidate& cand : alias->candidates) {
                if (!cand.enabled) {
                    steps.push_back(ResolutionStep{ResolutionStep::Kind::Alias, alias->name, cand.name,
                                                   std::string(kSkipped) + "disabled"});
                    continue;
                }
                std::vector<ResolutionStep> sub;
                Diagnostics subDiags;
                Found found = resolve(cand.name, requester, alias, aliasChain, sub, subDiags);
                if (!found.error.empty()) {
                    aliasChain.pop_back();
                    steps.insert(steps.end(), sub.begin(), sub.end());
                    return found;
                }
                const bool channel = found.kind == Found::Kind::Logged || found.kind == Found::Kind::Maths;
                if (!channel) {
                    steps.push_back(ResolutionStep{ResolutionStep::Kind::Alias, alias->name, cand.name,
                                                   std::string(kSkipped) + "missing"});
                    continue;
                }
                const bool inError =
                    (found.kind == Found::Kind::Logged &&
                     found.channel->status == model::ChannelStatus::Error) ||
                    (found.kind == Found::Kind::Maths && g_.nodes[found.node]->defError.has_value());
                if (inError) {
                    steps.push_back(
                        ResolutionStep{ResolutionStep::Kind::Alias, alias->name, cand.name,
                                       std::string(kSkipped) + "status Error (W-ALIAS-FALLBACK)"});
                    Diagnostic d;
                    d.code = "W-ALIAS-FALLBACK";
                    d.level = Diagnostic::Warning;
                    d.message = "alias '" + alias->name + "': '" + cand.name +
                                "' has an error; trying the next candidate";
                    diags.push_back(std::move(d));
                    continue;
                }
                steps.push_back(ResolutionStep{ResolutionStep::Kind::Alias, alias->name, cand.name, {}});
                steps.insert(steps.end(), sub.begin(), sub.end());
                diags.insert(diags.end(), subDiags.begin(), subDiags.end());
                aliasChain.pop_back();
                return found;
            }
            aliasChain.pop_back();
            steps.push_back(
                ResolutionStep{ResolutionStep::Kind::Missing, alias->name, {}, "no candidate resolves"});
            return out;
        }

        const std::string nameCf = caseFold(current);
        const MathNode& req = *g_.nodes[requester];
        // 3. The shadowing chain (self-maths: the enabled element before the requester).
        bool self = false;
        if (const std::optional<std::size_t> j = chainTarget(nameCf, requester, self)) {
            ensure(*j);
            const MathNode& t = *g_.nodes[*j];
            steps.push_back(ResolutionStep{ResolutionStep::Kind::CaseFold, current, t.name,
                                           t.name == current ? std::string("exact") : std::string()});
            out.kind = Found::Kind::Maths;
            out.node = *j;
            out.self = self;
        } else {
            // 4. The logged channel: canonical name, then logged name.
            lookupLogged(current, out, steps);
            out.self = self && out.kind == Found::Kind::Logged;
        }
        if (out.kind != Found::Kind::Missing) {
            if (constantFor(nameCf, req, nullptr) != nullptr) {
                Diagnostic d;
                d.code = "W-CONST-SHADOWED";
                d.level = Diagnostic::Warning;
                d.message = "'" + current + "' is a channel and a constant: the channel is read";
                diags.push_back(std::move(d));
            }
            return out;
        }
        // 5. A numeric or text detail of the source.
        if (g_.source != nullptr) {
            if (const model::Detail* d = model::findDetail(g_.source->details, current)) {
                out.kind = Found::Kind::Detail;
                out.detail = d;
                return out;
            }
        }
        // 6. A constant.
        if (const MathConstant* c = constantFor(nameCf, req, &diags)) {
            out.kind = Found::Kind::Constant;
            out.constant = c;
            return out;
        }
        steps.push_back(ResolutionStep{ResolutionStep::Kind::Missing, current, {}, {}});
        return out;
    }

    // ---- building -----------------------------------------------------------------------------------------

    void build(std::size_t i) {
        MathNode& n = *g_.nodes[i];
        if (n.buildState != 0) {
            return;
        }
        n.buildState = 1;
        stack_.push_back(i);
        if (n.ignored || n.off != Off::None) {
            basicDescriptor(n);
        } else if (stack_.size() > graph::kMaxDepth) {
            n.defError = graph::definitionError("E-EXPR-TOO-DEEP", "maths items read each other more than " +
                                                                       std::to_string(graph::kMaxDepth) +
                                                                       " levels deep");
            basicDescriptor(n);
        } else if (n.builtin) {
            buildBuiltin(i);
        } else {
            buildItem(i);
        }
        if (n.onCycle) {
            n.defError = graph::definitionError("E-CYCLE", "cycle: " + n.cyclePath);
            Diagnostic d;
            d.code = "E-CYCLE";
            d.level = Diagnostic::Error;
            d.message = n.defError->message;
            n.diags.push_back(std::move(d));
        }
        stack_.pop_back();
        n.buildState = 2;
    }

    void basicDescriptor(MathNode& n) const {
        model::ChannelDescriptor d;
        d.id = mathsChannelId(g_.sourceId(), g_.layer, n);
        d.name = n.name;
        d.loggedName = n.name;
        d.source = model::ChannelSource::Maths;
        if (n.builtin) {
            d.unit = n.def.resultUnit;
            d.quantity = n.def.quantity;
            d.dtype = n.def.dtype;
            d.interp = n.def.interp;
            d.dps = n.def.display.dps;
            d.colourIndex = n.def.display.colourIndex;
        } else {
            d.unit = units_ != nullptr ? graph::baseUnitOf(n.item.resultUnit, *units_) : units::kNoUnit;
            d.quantity = n.item.quantity;
            d.interp = n.item.interp;
            d.dps = n.item.display.dps;
            d.colourIndex = n.item.display.colourIndex;
        }
        n.descriptor = std::move(d);
    }

    // The descriptor of the element a self-maths node shadows (03 section 8), or nullptr.
    const model::ChannelDescriptor* shadowedDescriptor(const Found& f) const {
        if (f.kind == Found::Kind::Logged && f.channel != nullptr) {
            return &f.channel->desc;
        }
        if (f.kind == Found::Kind::Maths && g_.nodes[f.node]->buildState == 2) {
            return &g_.nodes[f.node]->descriptor;
        }
        return nullptr;
    }

    void buildBuiltin(std::size_t i) {
        MathNode& n = *g_.nodes[i];
        const BuiltinOutput& def = n.def;
        // Upstream: outputs of the same registration.
        for (const std::string& up : def.upstream) {
            const std::string upCf = caseFold(up);
            for (std::size_t j = 0; j < g_.nodes.size(); ++j) {
                const MathNode& t = *g_.nodes[j];
                if (t.builtin && t.builtinReg == n.builtinReg && t.key.nameCf == upCf) {
                    ensure(j);
                    n.upstream.push_back(j);
                    break;
                }
            }
        }
        Rate best{};
        for (const std::string& input : def.inputs) {
            const model::Channel* ch = nullptr;
            if (layer_ != nullptr) {
                ch = layer_->find(input);
                if (ch == nullptr) {
                    ch = layer_->findLogged(input);
                }
            }
            if (ch == nullptr) {
                continue;
            }
            n.loggedRead.push_back(ch);
            if (const std::optional<Rate> r = graph::nominalRate(*ch);
                r && r->valid() && (!best.valid() || best < *r)) {
                best = *r;
            }
            if (caseFold(input) == n.key.nameCf) {
                n.selfMaths = true;
            }
        }
        for (const std::size_t u : n.upstream) {
            const Rate r = g_.nodes[u]->rate;
            if (r.valid() && (!best.valid() || best < r)) {
                best = r;
            }
        }
        if (!best.valid() && layer_ != nullptr) {
            best = graph::highestRate(*layer_);
        }
        n.rate = best;
        n.storedUnit = def.resultUnit;
        n.dtype = def.dtype;
        n.integer = def.dtype == store::Dtype::I32;

        const graph::BuiltinReg& reg = g_.builtins[n.builtinReg];
        Hash128Stream h;
        h.updatePart("esses.maths.defHash-1");
        h.updatePart(graph::kEngineVersion);
        h.updatePart("builtin");
        h.updatePart(reg.id);
        h.updateU64(reg.module != nullptr ? static_cast<std::uint64_t>(reg.module->version()) : 0u);
        h.updatePart(n.settingsText);
        h.updatePart(def.name);
        h.updateU64(def.upstream.size());
        for (const std::string& up : def.upstream) {
            h.updatePart(caseFold(up));
        }
        h.updateU64(def.inputs.size());
        for (const std::string& in : def.inputs) {
            h.updatePart(caseFold(in));
        }
        h.updateU64(def.quantity);
        h.updateU64(def.resultUnit);
        h.updateU64(static_cast<std::uint64_t>(def.interp));
        h.updateU64(static_cast<std::uint64_t>(def.dtype));
        h.updatePart(aliases_);
        n.defHash = h.digest();

        basicDescriptor(n);
        if (n.selfMaths) {
            // The element before this one (the logged channel for the first maths element).
            std::vector<const model::AliasRow*> aliasChain;
            std::vector<ResolutionStep> steps;
            Diagnostics ignored;
            bool self = false;
            Found f;
            if (const std::optional<std::size_t> j = chainTarget(n.key.nameCf, i, self)) {
                ensure(*j);
                f.kind = Found::Kind::Maths;
                f.node = *j;
                steps.push_back(
                    ResolutionStep{ResolutionStep::Kind::CaseFold, n.name, g_.nodes[*j]->name, "exact"});
            } else {
                lookupLogged(n.name, f, steps);
            }
            if (const model::ChannelDescriptor* shadowed = shadowedDescriptor(f)) {
                model::ChannelDescriptor d = *shadowed;
                d.id = n.descriptor.id;
                d.name = n.name;
                d.source = model::ChannelSource::Maths;
                d.statusText.clear();
                d.dtype = def.dtype;
                d.rate.reset();
                d.sourceEncoding = nullptr;
                n.descriptor = std::move(d);
                n.storedUnit = n.descriptor.unit;
            }
            MathInput in;
            in.name = n.name;
            in.kind = f.kind == Found::Kind::Maths    ? MathInput::Kind::Maths
                      : f.kind == Found::Kind::Logged ? MathInput::Kind::Logged
                                                      : MathInput::Kind::Missing;
            if (f.kind == Found::Kind::Maths) {
                in.node = g_.idOf(*g_.nodes[f.node]);
            }
            in.trace = traceOf(n.name, steps, f);
            n.inputsInfo.push_back(std::move(in));
            n.resolvedInput = n.key;
        }
    }

    model::ResolutionTrace traceOf(const std::string& name, std::vector<ResolutionStep> steps,
                                   const Found& f) const {
        model::ResolutionTrace t;
        t.requested = name;
        t.steps = std::move(steps);
        units::UnitId unit = units::kNoUnit;
        if (f.kind == Found::Kind::Logged && f.channel != nullptr) {
            t.result = f.channel->desc.id;
            unit = f.channel->desc.unit;
        } else if (f.kind == Found::Kind::Maths) {
            const MathNode& target = *g_.nodes[f.node];
            t.result = mathsChannelId(g_.sourceId(), g_.layer, target);
            unit = target.storedUnit;
        }
        if (t.result && units_ != nullptr && unit != units::kNoUnit && units_->contains(unit)) {
            t.unitLabel = units_->unit(unit).symbol;
        }
        return t;
    }

    // The scope entry of a channel target, registered under a key unique to the target within the node.
    ScopeEntry channelEntry(const Found& f, MathNode& n,
                            std::unordered_map<std::string, std::string>& keyOwner) {
        ScopeEntry e;
        e.what = ScopeEntry::Channel;
        e.type.kind = Kind::Series;
        std::string targetName;
        std::string owner;
        InputRef ref;
        if (f.kind == Found::Kind::Logged) {
            const model::Channel& ch = *f.channel;
            e.type.unit = ch.desc.unit;
            e.type.rate = graph::nominalRate(ch);
            e.type.integer = integerChannel(ch);
            e.interp = ch.desc.interp;
            e.dtype = ch.desc.dtype;
            targetName = ch.desc.name;
            owner = "L" + ch.desc.id.str();
            ref.channel = &ch;
            ref.storedUnit = ch.desc.unit;
        } else {
            const MathNode& t = *g_.nodes[f.node];
            const bool built = t.buildState == 2 && !t.defError;
            units::UnitId unit = t.storedUnit;
            if (!built && !t.builtin && units_ != nullptr) {
                unit = graph::baseUnitOf(t.item.resultUnit, *units_);
            }
            e.type.unit = unit;
            e.type.rate = built && t.rate.valid() ? t.rate : Rate{};
            e.type.integer = t.integer;
            e.interp = t.builtin ? t.def.interp : t.item.interp;
            if (t.buildState == 2) {
                e.interp = t.descriptor.interp;
            }
            e.dtype = t.dtype;
            targetName = t.name;
            owner = "N" + t.identity;
            ref.fromNode = true;
            ref.node = f.node;
            ref.storedUnit = unit;
        }
        ChannelKey key = ChannelKey::make(g_.sourceId(), g_.layer, targetName);
        for (std::size_t suffix = 1;; ++suffix) {
            const auto [it, inserted] = keyOwner.try_emplace(key.nameCf, owner);
            if (inserted || it->second == owner) {
                break;
            }
            key.nameCf = caseFold(targetName) + "\x1F" + std::to_string(suffix);
        }
        e.key = key;
        e.resolvedName = targetName;
        ref.key = key;
        ref.name = targetName;
        if (std::none_of(n.inputs.begin(), n.inputs.end(), [&](const InputRef& r) { return r.key == key; })) {
            n.inputs.push_back(std::move(ref));
        }
        return e;
    }

    // The self reference of a synthesised expression: 'N'[u] in the unit `u` it is read in (as stored: an
    // identity conversion), so it raises no W-UNIT-MISSING in text the user never wrote; bare for a unitless
    // channel.
    std::string selfReference(const std::string& name, units::UnitId u) const {
        std::string ref = "'" + name + "'";
        if (u == units::kNoUnit || units_ == nullptr || !units_->contains(u) ||
            units_->unit(u).qty == units::kUnitless || units_->unit(u).symbol.empty()) {
            return ref;
        }
        return ref + "[" + units_->unit(u).symbol + "]";
    }

    // The expression a filter or scale_offset item stands for (03 section 4.4), or its definition error.
    std::optional<std::string> synthesise(MathNode& n, const Found& self) {
        const MathItem& item = n.item;
        units::UnitId stored = units::kNoUnit;
        if (self.kind == Found::Kind::Logged) {
            stored = self.channel->desc.unit;
        } else if (self.kind == Found::Kind::Maths) {
            stored = g_.nodes[self.node]->storedUnit;
        }
        if (item.kind == ItemKind::Filter) {
            // smooth()'s window rules (03 section 5.2): 0 and 1 points, and seconds below two samples, are a
            // window of 2; a negative or non-finite window is an error.
            const std::string ref = selfReference(item.name, stored);
            if (item.window.points) {
                if (*item.window.points < 0) {
                    n.defError =
                        graph::definitionError("E-ARG-VALUE", "the filter window must not be negative");
                    return std::nullopt;
                }
                return "smooth(" + ref + ", " + std::to_string(*item.window.points) + ")";
            }
            if (item.window.seconds && *item.window.seconds >= 0.0 && std::isfinite(*item.window.seconds)) {
                return "smooth(" + ref + ", " + numberText(*item.window.seconds, true) + ")";
            }
            n.defError = graph::definitionError(
                "E-ARG-VALUE", item.window.seconds
                                   ? "the filter window must be a number of seconds of at least 0"
                                   : "the filter item needs a window in points or seconds");
            return std::nullopt;
        }
        // scale_offset (D-186): y = 'N' * scale + offset in the unit of the offset when it is a unit of the
        // input's display family (a temperature scaled in deg C, as the user reads it), else in the unit N is
        // stored in with the offset converted as a delta; the result is stored in the base unit.
        units::UnitId apply = stored;
        double offset = item.offset.v;
        if (item.offset.unit != units::kNoUnit && stored != units::kNoUnit && units_ != nullptr &&
            item.offset.unit != stored && units_->contains(item.offset.unit) && units_->contains(stored)) {
            Result<units::Converter> conv = units_->converter(item.offset.unit, stored);
            if (conv) {
                apply = item.offset.unit;
            } else {
                Diagnostic d;
                d.code = "W-UNIT-FAMILY";
                d.level = Diagnostic::Warning;
                d.message = "the offset's unit [" + units_->unit(item.offset.unit).symbol +
                            "] cannot convert into [" + units_->unit(stored).symbol +
                            "]; the number is added as it is";
                n.diags.push_back(std::move(d));
            }
        }
        if (!std::isfinite(item.scale) || !std::isfinite(offset)) {
            n.defError = graph::definitionError("E-ARG-VALUE", "the scale and offset must be finite numbers");
            return std::nullopt;
        }
        // compile() converts the values from the unit they are computed in to its base unit.
        n.item.resultUnit = apply;
        std::string text = selfReference(item.name, apply) + " * " + numberText(item.scale, true);
        text += offset < 0.0 ? " - " + numberText(-offset, true) : " + " + numberText(offset, true);
        return text;
    }

    // Whether a reference to `nameCf` sits inside a previous_sample() call of `ast` (03 section 5.4).
    static bool readInPreviousSample(const Ast& ast, const std::string& nameCf) {
        std::vector<std::uint32_t> stack;
        for (const Node& call : ast.nodes) {
            if (call.kind != NodeKind::Call || caseFold(call.text) != "previous_sample") {
                continue;
            }
            stack.assign(call.kids.begin(), call.kids.end());
            while (!stack.empty()) {
                const std::uint32_t k = stack.back();
                stack.pop_back();
                if (k >= ast.nodes.size()) {
                    continue;
                }
                const Node& node = ast.nodes[k];
                if (node.kind == NodeKind::ChannelRef && caseFold(node.text) == nameCf) {
                    return true;
                }
                stack.insert(stack.end(), node.kids.begin(), node.kids.end());
            }
        }
        return false;
    }

    void buildItem(std::size_t i) {
        MathNode& n = *g_.nodes[i];
        basicDescriptor(n);
        switch (n.kind) {
        case ItemKind::Table3D:
        case ItemKind::Regression:
            n.defError = graph::definitionError("E-KIND-UNSUPPORTED",
                                                "items of kind " + std::string(graph::itemKindName(n.kind)) +
                                                    " are evaluated from version 1.0");
            pushError(n);
            return;
        case ItemKind::Plugin:
            n.defError = graph::definitionError("E-PLUGIN-UNAVAILABLE",
                                                "plugin items cannot be evaluated in this version");
            pushError(n);
            return;
        case ItemKind::Builtin:
            return; // not a node (collect skips them)
        case ItemKind::Expression:
        case ItemKind::Filter:
        case ItemKind::ScaleOffset:
        case ItemKind::Table2D:
            break;
        }
        if (units_ == nullptr) {
            n.defError = graph::definitionError("E-EVAL", "the maths graph has no unit registry");
            pushError(n);
            return;
        }

        // The self reference of filter and scale_offset items first (the offset needs its unit).
        std::vector<const model::AliasRow*> aliasChain;
        Found selfFound;
        std::vector<ResolutionStep> selfSteps;
        Diagnostics selfDiags;
        if (n.kind == ItemKind::Filter || n.kind == ItemKind::ScaleOffset) {
            selfFound = resolve(n.name, i, nullptr, aliasChain, selfSteps, selfDiags);
        }
        if (n.kind == ItemKind::Expression) {
            n.sourceText = n.item.expr;
        } else if (n.kind == ItemKind::Table2D) {
            n.sourceText = n.item.table.x.expr; // compile() appends the table stage (03 section 4.4)
        } else if (std::optional<std::string> text = synthesise(n, selfFound)) {
            n.sourceText = std::move(*text);
        } else {
            pushError(n);
            return;
        }
        const graph::ParsedText& parsed = parse(n.sourceText);
        n.diags.insert(n.diags.end(), parsed.diags.begin(), parsed.diags.end());
        if (parsed.error || parsed.ast == nullptr) {
            n.defError = parsed.error ? *parsed.error : graph::definitionError("E-SYNTAX", "does not parse");
            return;
        }
        n.ast = parsed.ast;

        // Every referenced name, in source order.
        std::vector<std::pair<std::uint32_t, std::string>> refs;
        for (const auto& node : n.ast->nodes) {
            if (node.kind == NodeKind::ChannelRef) {
                refs.emplace_back(node.span.begin, node.text);
            }
        }
        std::sort(refs.begin(), refs.end());
        // integrate_dist reads the source's Corr Dist without naming it (03 section 5.4, D-170): compile()
        // looks it up in the scope, so it is resolved (and versioned) like a reference.
        const bool readsDistance =
            std::any_of(n.ast->nodes.begin(), n.ast->nodes.end(), [](const auto& node) {
                return node.kind == NodeKind::Call && node.text == "integrate_dist";
            });
        if (readsDistance && std::none_of(refs.begin(), refs.end(),
                                          [](const auto& r) { return caseFold(r.second) == "corr dist"; })) {
            refs.emplace_back(std::numeric_limits<std::uint32_t>::max(), "Corr Dist");
        }
        auto scope = std::make_shared<graph::FrozenScope>(g_.units, g_.source != nullptr);
        std::unordered_map<std::string, std::string> keyOwner;
        Found selfTarget;
        bool haveSelf = false;
        Hash128Stream scopeHash;
        scopeHash.updatePart("esses.maths.scope-1");
        std::map<std::string, std::string> hashParts; // nameCf -> what it resolved to (sorted for the hash)
        bool recursive = false; // previous_sample() of its own name with nothing before it (03 section 5.4)
        for (const auto& [begin, name] : refs) {
            const std::string nameCf = caseFold(name);
            if (scope->entries.count(nameCf) != 0 ||
                std::any_of(n.inputsInfo.begin(), n.inputsInfo.end(),
                            [&](const MathInput& in) { return caseFold(in.name) == nameCf; })) {
                continue;
            }
            std::vector<ResolutionStep> steps;
            Diagnostics lookupDiags;
            Found f;
            if ((n.kind == ItemKind::Filter || n.kind == ItemKind::ScaleOffset) && nameCf == n.key.nameCf) {
                f = selfFound;
                steps = selfSteps;
                lookupDiags = selfDiags;
            } else {
                aliasChain.clear();
                f = resolve(name, i, nullptr, aliasChain, steps, lookupDiags);
            }
            MathInput info;
            info.name = name;
            std::string part;
            std::optional<ScopeEntry> entry;
            switch (f.kind) {
            case Found::Kind::Logged:
            case Found::Kind::Maths: {
                entry = channelEntry(f, n, keyOwner);
                info.kind = f.kind == Found::Kind::Logged ? MathInput::Kind::Logged : MathInput::Kind::Maths;
                if (f.kind == Found::Kind::Maths) {
                    info.node = g_.idOf(*g_.nodes[f.node]);
                    part = "N" + g_.nodes[f.node]->identity;
                } else {
                    part = "L" + f.channel->desc.id.str();
                }
                if (f.self && !haveSelf) {
                    selfTarget = f;
                    haveSelf = true;
                }
                break;
            }
            case Found::Kind::Detail:
                entry = detailEntry(*f.detail);
                info.kind = MathInput::Kind::Detail;
                part = "D" + entry->text;
                break;
            case Found::Kind::Constant:
                entry = constantEntry(*f.constant);
                info.kind = MathInput::Kind::Constant;
                part = "C";
                break;
            case Found::Kind::Missing:
                info.kind = MathInput::Kind::Missing;
                part = "M" + f.error;
                if (f.error.empty() && nameCf == n.key.nameCf && g_.source != nullptr &&
                    readInPreviousSample(*n.ast, nameCf)) {
                    recursive = true;
                }
                if (!f.error.empty()) {
                    Diagnostic d;
                    d.code = "E-UNKNOWN-CHANNEL";
                    d.level = Diagnostic::Error;
                    if (begin != std::numeric_limits<std::uint32_t>::max()) {
                        d.span = Span{begin, static_cast<std::uint32_t>(begin + name.size() + 2)};
                    }
                    d.message = "'" + name + "': " + f.error;
                    n.diags.push_back(std::move(d));
                }
                break;
            }
            if (entry) {
                entry->trace = traceOf(name, steps, f);
                entry->diags = lookupDiags;
                const ScopeEntry& e = *entry;
                part += "|" + std::to_string(static_cast<int>(e.what)) + "|" + std::to_string(e.type.unit) +
                        "|" + (e.type.rate ? e.type.rate->toString() : std::string("-")) + "|" +
                        (e.type.integer ? "i" : "r") + "|" + std::to_string(static_cast<int>(e.interp)) +
                        "|" + std::to_string(static_cast<int>(e.dtype)) + "|" +
                        (e.number ? numberText(*e.number, false) : std::string("-")) + "|" + e.text + "|" +
                        e.key.nameCf;
                for (const Diagnostic& d : lookupDiags) {
                    part += "|" + d.code + ":" + d.message;
                }
                scope->entries.emplace(nameCf, *entry);
            }
            info.trace = traceOf(name, std::move(steps), f);
            n.inputsInfo.push_back(std::move(info));
            hashParts[nameCf] = std::move(part);
        }
        for (const auto& [nameCf, part] : hashParts) {
            scopeHash.updatePart(nameCf);
            scopeHash.updatePart(part);
        }
        n.scopeHash = scopeHash.digest();
        n.scope = scope;
        if (n.kind == ItemKind::Filter || n.kind == ItemKind::ScaleOffset) {
            haveSelf = selfFound.kind == Found::Kind::Logged || selfFound.kind == Found::Kind::Maths;
            selfTarget = selfFound;
            n.selfMaths = true;
        } else if (haveSelf) {
            n.selfMaths = true;
        }
        if (n.selfMaths) {
            n.resolvedInput = n.key;
        }
        if (recursive && !n.onCycle) {
            // 'A' = previous_sample('A') + 1 with no logged or earlier 'A': a recursive definition, a cycle
            // in 0.1 (03 section 5.4; recursive evaluation is the telemetry point evaluator's, v1.1).
            const std::string message = "cycle: '" + n.name +
                                        "' reads its own previous sample; recursive maths "
                                        "is evaluated from version 1.1";
            n.defError = graph::definitionError("E-CYCLE", message);
            Diagnostic d;
            d.code = "E-CYCLE";
            d.level = Diagnostic::Error;
            d.message = message;
            n.diags.push_back(std::move(d));
            return;
        }
        if (n.onCycle) {
            return; // E-CYCLE (build); nothing to compile
        }

        // Compile, unless the signature equals the previous build's.
        Hash128Stream sig;
        sig.updatePart("esses.maths.compileSig-2");
        sig.updatePart(aliases_);
        sig.updatePart(g_.source != nullptr ? "data" : "data-less");
        hashItem(sig, n.item);
        sig.updatePart(n.sourceText);
        updateHash(sig, n.ast->sourceHash); // the parse: differs with the power switch only where it applies
        updateHash(sig, n.scopeHash);
        const auto old = olds_.find(n.identity);
        n.compileSig = sig.digest();
        if (old != olds_.end() && old->second->compiled && old->second->compileSig == n.compileSig) {
            const MathNode& o = *old->second;
            n.compiled = true;
            n.compileDiags = o.compileDiags;
            n.compileError = o.compileError;
            n.planKeys = o.planKeys;
            n.planRate = o.planRate;
            n.storedUnit = o.storedUnit;
            n.dtype = o.dtype;
            n.integer = o.integer;
            n.outKind = o.outKind;
            n.checkOnly = o.checkOnly;
            n.ownSelectionDependent = o.ownSelectionDependent;
            n.rangeGroups = o.rangeGroups;
            n.defHash = o.defHash;
        } else {
            compileNode(n);
        }
        n.diags.insert(n.diags.end(), n.compileDiags.begin(), n.compileDiags.end());
        if (n.compileError) {
            n.defError = n.compileError;
            return;
        }
        // Only the inputs the plan reads are read (and versioned).
        std::vector<InputRef> read;
        for (const ChannelKey& k : n.planKeys) {
            const auto it =
                std::find_if(n.inputs.begin(), n.inputs.end(), [&](const InputRef& r) { return r.key == k; });
            if (it != n.inputs.end()) {
                read.push_back(*it);
            }
        }
        n.inputs = std::move(read);
        for (const InputRef& r : n.inputs) {
            if (r.fromNode) {
                n.upstream.push_back(r.node);
            } else if (r.channel != nullptr) {
                n.loggedRead.push_back(r.channel);
            }
        }
        n.rate = n.planRate ? *n.planRate : (layer_ != nullptr ? graph::highestRate(*layer_) : Rate{});
        if (!n.rate.valid() && g_.source == nullptr) {
            return; // without data nothing is evaluated: checked only (esses-cli maths check)
        }
        if (!n.rate.valid()) {
            n.defError = graph::definitionError(
                "E-EVAL", "the layer has no channel with a rate to evaluate '" + n.name + "' at");
            pushError(n);
            return;
        }
        itemDescriptor(n, haveSelf ? &selfTarget : nullptr);
    }

    void compileNode(MathNode& n) const {
        n.compiled = true;
        n.checkOnly = false;
        n.outKind = Kind::Series;
        n.compileDiags.clear();
        n.compileError.reset();
        Result<Plan> plan = graph::buildPlan(*n.ast, *n.scope, n.item, g_.compat, n.compileDiags);
        if (!plan && g_.source == nullptr &&
            plan.error().context.detail == std::string("E-UNKNOWN-CHANNEL") &&
            std::any_of(n.compileDiags.begin(), n.compileDiags.end(),
                        [](const Diagnostic& d) { return d.code == "I-UNRESOLVED"; })) {
            // Without data (esses-cli maths check) the names are I-UNRESOLVED, which compile() refuses to
            // lower: the item is checked, not compiled, and nothing else is wrong with it.
            n.compileDiags.erase(std::remove_if(n.compileDiags.begin(), n.compileDiags.end(),
                                                [](const Diagnostic& d) {
                                                    return d.code == "E-UNKNOWN-CHANNEL" &&
                                                           d.level == Diagnostic::Error;
                                                }),
                                 n.compileDiags.end());
            n.checkOnly = true;
            n.planKeys.clear();
            n.planRate.reset();
            n.storedUnit = units_ != nullptr ? graph::baseUnitOf(n.item.resultUnit, *units_) : units::kNoUnit;
            return;
        }
        if (!plan) {
            n.compileError = plan.error();
            if (!hasError(n.compileDiags)) {
                Diagnostic d;
                d.code = graph::isDiagnosticCode(plan.error().context.detail) ? *plan.error().context.detail
                                                                              : std::string("E-EVAL");
                d.level = Diagnostic::Error;
                d.message = plan.error().message;
                n.compileDiags.push_back(std::move(d));
            }
            return;
        }
        n.planKeys.clear();
        for (const PlanInput& in : plan->inputs) {
            n.planKeys.push_back(in.key);
        }
        n.planRate.reset();
        const std::optional<Rate> outRate = plan->out.rate;
        if (outRate.has_value() && outRate->valid()) {
            n.planRate = outRate;
        }
        n.dtype = plan->outDtype;
        n.integer = plan->out.integer || plan->out.kind == Kind::Bool;
        n.outKind = plan->out.kind;
        n.ownSelectionDependent = plan->selectionDependent;
        n.rangeGroups = plan->rangeGroups;
        const units::UnitId inferred =
            units_->contains(plan->out.unit) ? plan->out.unit : units::kNoUnit; // kNone: a composite unit
        switch (n.kind) {
        case ItemKind::Expression:
        case ItemKind::ScaleOffset: // computed in the offset's unit, converted to its base (D-186)
            n.storedUnit = n.item.resultUnit != units::kNoUnit ? graph::baseUnitOf(n.item.resultUnit, *units_)
                                                               : inferred;
            break;
        default:
            n.storedUnit = inferred;
            break;
        }
        // compile()'s defHash covers the canonical expression (comments and spacing do not count), the
        // kind, rate, result unit, interpolation, precision, enumeration, table data and compat switches.
        Hash128Stream h;
        h.updatePart("esses.maths.defHash-2");
        h.updatePart(graph::kEngineVersion);
        updateHash(h, plan->defHash);
        h.updatePart(graph::itemKindName(n.kind));
        n.defHash = h.digest();
    }

    void pushError(MathNode& n) const {
        if (!n.defError) {
            return;
        }
        for (const Diagnostic& d : n.diags) {
            if (d.level == Diagnostic::Error) {
                return;
            }
        }
        Diagnostic d;
        d.code = graph::isDiagnosticCode(n.defError->context.detail) ? *n.defError->context.detail
                                                                     : std::string("E-EVAL");
        d.level = Diagnostic::Error;
        d.message = n.defError->message;
        n.diags.push_back(std::move(d));
    }

    void itemDescriptor(MathNode& n, const Found* self) const {
        model::ChannelDescriptor d;
        const model::ChannelDescriptor* shadowed = self != nullptr ? shadowedDescriptor(*self) : nullptr;
        const bool declared = n.item.resultUnit != units::kNoUnit || n.item.quantity != units::kUnitless;
        if (shadowed != nullptr && (n.kind != ItemKind::Expression || !declared)) {
            // Self-maths keeps the shadowed channel's descriptor and display settings (03 section 8).
            d = *shadowed;
            d.statusText.clear();
            d.sourceEncoding = nullptr;
            d.enums.reset();
        } else {
            d.quantity = n.item.quantity;
            if (d.quantity == units::kUnitless && n.storedUnit != units::kNoUnit) {
                d.quantity = units_->quantityOf(n.storedUnit);
            }
            d.dps = n.item.display.dps;
            d.colourIndex = n.item.display.colourIndex;
            d.interp = n.item.interp;
            // Integer and enumerated results step (03 section 3.2 rule 11), and so does a per-lap or window
            // constant (a LapScalar is a Previous-interpolated series, D-021 and 03 section 3.2 rule 3).
            if ((n.integer || n.item.enumeration || n.outKind == Kind::LapScalar ||
                 n.outKind == Kind::Scalar) &&
                d.interp == store::Interp::Linear) {
                d.interp = store::Interp::Previous;
            }
            d.kind = n.item.enumeration ? model::SampleKind::Enum
                     : n.integer        ? model::SampleKind::Integer
                                        : model::SampleKind::Real;
        }
        d.id = mathsChannelId(g_.sourceId(), g_.layer, n);
        d.name = n.name;
        d.loggedName = n.name;
        d.unit = n.storedUnit;
        d.dtype = n.dtype;
        d.source = model::ChannelSource::Maths;
        d.rate = n.rate;
        n.descriptor = std::move(d);
    }

    MathGraph::Impl& g_;
    const model::DataLayer* layer_;
    const units::UnitRegistry* units_;
    std::string compat_;  // every switch: the parse cache key
    std::string aliases_; // the function aliases: the signatures (D-185)
    const std::unordered_map<std::string, std::unique_ptr<MathNode>>& olds_;
    std::vector<std::size_t> stack_;
    std::vector<std::size_t> order_;
    mutable std::unordered_map<const MathModule*, std::pair<bool, Diagnostics>> conditions_;
};

// Step 4: versions (03 section 7.2) and SelectionDependent inheritance, depth first over the upstream nodes.
class Versioner {
public:
    explicit Versioner(MathGraph::Impl& g) : g_(g), layer_(g.dataLayer()) {}

    void run(const std::vector<std::size_t>& order) {
        done_.assign(g_.nodes.size(), false);
        for (const std::size_t i : order) {
            visit(i);
        }
        for (std::size_t i = 0; i < g_.nodes.size(); ++i) {
            visit(i);
        }
    }

private:
    void visit(std::size_t i) {
        if (done_[i]) {
            return;
        }
        done_[i] = true; // set first: a cycle never recurses (nodes on it hash no upstream)
        MathNode& n = *g_.nodes[i];
        std::vector<Version> ups;
        n.selectionDependent = n.ownSelectionDependent;
        if (!n.onCycle) {
            for (const std::size_t u : n.upstream) {
                visit(u);
                ups.push_back(g_.nodes[u]->version);
                n.selectionDependent = n.selectionDependent || g_.nodes[u]->selectionDependent;
            }
        }
        std::sort(ups.begin(), ups.end());
        Hash128Stream h;
        h.updatePart("esses.maths.version-2");
        updateHash(h, n.defHash);
        h.updatePart(n.identity);
        h.updateU64(static_cast<std::uint64_t>(n.off));
        h.updateU64(n.ignored ? 1u : 0u);
        h.updateU64(n.chainIndex);
        h.updateU64(ups.size());
        for (const Version& v : ups) {
            updateHash(h, v);
        }
        h.updateU64(layer_ != nullptr ? 1u : 0u);
        if (layer_ != nullptr) {
            h.updateU64(static_cast<std::uint64_t>(layer_->duration));
        }
        h.updateU64(n.rate.num);
        h.updateU64(n.rate.den);
        if (n.builtin) {
            // Each logged input as the evaluation resolves it (channel name, then logged name); an absent one
            // too.
            h.updateU64(n.def.inputs.size());
            for (const std::string& input : n.def.inputs) {
                h.updatePart(caseFold(input));
                const model::Channel* channel = nullptr;
                if (layer_ != nullptr) {
                    channel = layer_->find(input);
                    if (channel == nullptr) {
                        channel = layer_->findLogged(input);
                    }
                }
                h.updateU64(channel != nullptr ? 1u : 0u);
                if (channel != nullptr) {
                    graph::updateChannel(h, *channel);
                }
            }
        } else {
            h.updateU64(n.inputs.size());
            for (const InputRef& r : n.inputs) {
                h.updatePart(r.key.nameCf);
                if (!r.fromNode && r.channel != nullptr) {
                    graph::updateChannel(h, *r.channel);
                }
            }
        }
        h.updateU64(n.rangeGroups.size());
        for (const std::string& path : n.rangeGroups) {
            h.updatePart(path);
            h.updateU64(layer_ != nullptr ? layer_->ranges.rangeVersion(path) : 0u);
        }
        updateHash(h, n.scopeHash);
        h.updatePart(n.defError ? n.defError->message : std::string());
        n.version = h.digest();
    }

    MathGraph::Impl& g_;
    const model::DataLayer* layer_;
    std::vector<bool> done_;
};

} // namespace

std::vector<ChannelKey> MathGraph::Impl::rebuild() {
    std::unordered_map<std::string, std::unique_ptr<MathNode>> olds;
    std::vector<ChannelKey> oldOrder;
    for (auto& n : nodes) {
        oldOrder.push_back(n->key);
        std::string identity = n->identity;
        olds.emplace(std::move(identity), std::move(n));
    }
    nodes.clear();
    for (auto& [key, parsed] : astCache) {
        parsed.used = false;
    }
    if (rangesFor != source.get()) {
        const model::DataLayer* l = dataLayer();
        ranges = l != nullptr ? analysis::RangeTables::build(*l) : nullptr;
        rangesFor = source.get();
    }

    Builder builder(*this, olds);
    builder.collect();
    builder.chain();
    builder.buildAll();
    Versioner(*this).run(builder.order());

    std::vector<ChannelKey> changed;
    const auto note = [&changed](const ChannelKey& k) {
        if (std::find(changed.begin(), changed.end(), k) == changed.end()) {
            changed.push_back(k);
        }
    };
    for (auto& np : nodes) {
        MathNode& n = *np;
        const auto it = olds.find(n.identity);
        MathNode* old = it != olds.end() ? it->second.get() : nullptr;
        if (old != nullptr && old->version == n.version) {
            n.status = std::move(old->status);
            n.handle = std::move(old->handle);
            n.handleSelection = old->handleSelection;
            n.selectionCache = std::move(old->selectionCache);
            n.failure = std::move(old->failure);
            n.failureSelection = old->failureSelection;
            n.inflight = std::move(old->inflight);
            n.inflightVersion = old->inflightVersion;
            n.inflightSelection = old->inflightSelection;
            n.memo = std::move(old->memo);
            n.memoVersion = old->memoVersion;
            n.memoSelection = old->memoSelection;
        } else {
            if (old != nullptr) {
                cancelInflight(*old);
            }
            if (n.ignored) {
                n.status.version = n.version;
                n.status.state = MathState::Warning;
                n.status.diags = n.diags;
                n.status.summary = n.diags.empty() ? std::string("ignored") : n.diags.front().message;
            } else if (n.off != Off::None) {
                n.status = graph::freshStatus(n.version, n.off, n.offSummary);
                n.status.diags = n.diags;
            } else if (n.defError) {
                n.status = graph::errorStatus(n.version, *n.defError, n.diags);
            } else {
                n.status = graph::freshStatus(n.version, Off::None, {});
                n.status.diags = n.diags; // the definition's warnings, shown before the first evaluation
            }
            note(n.key);
        }
        n.status.resolvedInput = n.resolvedInput;
        if (it != olds.end()) {
            olds.erase(it);
        }
    }
    for (auto& [identity, old] : olds) {
        cancelInflight(*old); // a node that went away
    }
    std::unordered_set<std::string> present;
    for (const auto& n : nodes) {
        present.insert(n->key.nameCf);
    }
    for (const ChannelKey& k : oldOrder) {
        if (present.count(k.nameCf) == 0) {
            note(k);
        }
    }
    for (auto it = astCache.begin(); it != astCache.end();) {
        it = it->second.used ? std::next(it) : astCache.erase(it);
    }
    return changed;
}

} // namespace esses::maths
