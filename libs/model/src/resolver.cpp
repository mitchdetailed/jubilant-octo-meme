// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/casefold.hpp>
#include <esses/model/resolver.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace esses::model {

namespace {

constexpr std::string_view kArrow = "\xE2\x86\x92"; // U+2192 RIGHTWARDS ARROW
constexpr std::string_view kExact = "exact";
constexpr std::string_view kRenamedAtLoad = "renamed at load";
constexpr std::string_view kSkipped = "skipped: ";

std::string quoteName(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '\'';
    out += s;
    out += '\'';
    return out;
}

} // namespace

std::string_view resolutionStepKindName(ResolutionStep::Kind k) noexcept {
    switch (k) {
    case ResolutionStep::Kind::Dictionary:
        return "dictionary";
    case ResolutionStep::Kind::Mapping:
        return "mapping";
    case ResolutionStep::Kind::Alias:
        return "alias";
    case ResolutionStep::Kind::CaseFold:
        return "case-fold";
    case ResolutionStep::Kind::LoggedName:
        return "logged name";
    case ResolutionStep::Kind::Missing:
        return "missing";
    }
    return "missing";
}

std::string ResolutionTrace::summary() const {
    std::string out = quoteName(requested);
    const auto hop = [&out](std::string_view kind, std::string_view to) {
        out += ' ';
        out += kArrow;
        out += ' ';
        out += kind;
        out += ' ';
        out += kArrow;
        out += ' ';
        out += quoteName(to);
    };
    for (const ResolutionStep& s : steps) {
        if (std::string_view(s.note).starts_with(kSkipped)) {
            continue;
        }
        switch (s.kind) {
        case ResolutionStep::Kind::Mapping:
            if (s.note != kRenamedAtLoad) {
                hop(resolutionStepKindName(s.kind), s.to);
            }
            break;
        case ResolutionStep::Kind::Alias:
        case ResolutionStep::Kind::LoggedName:
            hop(resolutionStepKindName(s.kind), s.to);
            break;
        case ResolutionStep::Kind::CaseFold:
            if (s.note != kExact) {
                hop(resolutionStepKindName(s.kind), s.to);
            }
            break;
        case ResolutionStep::Kind::Dictionary:
            out += " [dictionary: logged as " + quoteName(s.from) + "]";
            break;
        case ResolutionStep::Kind::Missing:
            out += ' ';
            out += kArrow;
            out += " missing";
            if (!s.note.empty()) {
                out += " (" + s.note + ")";
            }
            break;
        }
    }
    if (resolved() && !unitLabel.empty()) {
        out += " (" + unitLabel + ")";
    }
    return out;
}

struct ChannelResolver::Outcome {
    const Channel* channel = nullptr;
    const AliasRow* alias = nullptr; // the outermost alias expanded on the way, if any
    std::string error;               // "Error: alias cycle ..." stops the resolution
};

ChannelResolver::ChannelResolver(MappingTable mappings, AliasTable aliases,
                                 const units::UnitRegistry* registry)
    : mappings_(std::move(mappings)), aliases_(std::move(aliases)), registry_(registry) {
    for (std::size_t i = 0; i < mappings_.rows.size(); ++i) {
        mappingIndex_.emplace(caseFold(mappings_.rows[i].from), i);
    }
    for (std::size_t i = 0; i < aliases_.rows.size(); ++i) {
        aliasIndex_.emplace(caseFold(aliases_.rows[i].name), i);
    }
}

const MappingRow* ChannelResolver::findMapping(std::string_view from) const {
    const auto it = mappingIndex_.find(caseFold(from));
    return it == mappingIndex_.end() ? nullptr : &mappings_.rows[it->second];
}

const AliasRow* ChannelResolver::findAlias(std::string_view name) const {
    const auto it = aliasIndex_.find(caseFold(name));
    return it == aliasIndex_.end() ? nullptr : &aliases_.rows[it->second];
}

void ChannelResolver::lookupChannel(std::string_view name, const DataLayer& layer, Outcome& out,
                                    std::vector<ResolutionStep>& steps) const {
    if (const Channel* c = layer.find(name)) {
        steps.push_back(ResolutionStep{ResolutionStep::Kind::CaseFold, std::string(name), c->desc.name,
                                       c->desc.name == name ? std::string(kExact) : std::string()});
        if (c->desc.loggedName != c->desc.name && !c->desc.loggedName.empty()) {
            const MappingRow* m = findMapping(c->desc.loggedName);
            if (m != nullptr && ciEquals(m->to, c->desc.name)) {
                steps.push_back(ResolutionStep{ResolutionStep::Kind::Mapping, c->desc.loggedName,
                                               c->desc.name, std::string(kRenamedAtLoad)});
            } else {
                steps.push_back(
                    ResolutionStep{ResolutionStep::Kind::Dictionary, c->desc.loggedName, c->desc.name, {}});
            }
        }
        out.channel = c;
        return;
    }
    if (const Channel* c = layer.findLogged(name)) {
        steps.push_back(
            ResolutionStep{ResolutionStep::Kind::LoggedName, std::string(name), c->desc.name, {}});
        out.channel = c;
        return;
    }
    steps.push_back(ResolutionStep{ResolutionStep::Kind::Missing, std::string(name), {}, {}});
}

ChannelResolver::Outcome ChannelResolver::resolveName(std::string_view name, const DataLayer& layer,
                                                      const AliasRow* excluded,
                                                      std::vector<const AliasRow*>& chain,
                                                      std::vector<ResolutionStep>& steps) const {
    Outcome out;
    std::string current(name);

    // 1. Mappings: one hop, from -> to.
    if (const MappingRow* m = findMapping(current); m != nullptr && !ciEquals(m->to, current)) {
        steps.push_back(ResolutionStep{ResolutionStep::Kind::Mapping, current, m->to, {}});
        current = m->to;
    }

    // 2. Aliases (the alias being expanded is invisible to its own candidates).
    const AliasRow* alias = findAlias(current);
    if (alias != nullptr && alias != excluded) {
        if (std::find(chain.begin(), chain.end(), alias) != chain.end()) {
            std::string cycle;
            const auto from = std::find(chain.begin(), chain.end(), alias);
            for (auto it = from; it != chain.end(); ++it) {
                cycle += quoteName((*it)->name) + " " + std::string(kArrow) + " ";
            }
            cycle += quoteName(alias->name);
            out.error = "Error: alias cycle " + cycle;
            return out;
        }
        chain.push_back(alias);
        for (const AliasCandidate& cand : alias->candidates) {
            if (!cand.enabled) {
                steps.push_back(ResolutionStep{ResolutionStep::Kind::Alias, alias->name, cand.name,
                                               std::string(kSkipped) + "disabled"});
                continue;
            }
            std::vector<ResolutionStep> sub;
            Outcome found = resolveName(cand.name, layer, alias, chain, sub);
            if (!found.error.empty()) {
                chain.pop_back();
                return found;
            }
            if (found.channel == nullptr) {
                steps.push_back(ResolutionStep{ResolutionStep::Kind::Alias, alias->name, cand.name,
                                               std::string(kSkipped) + "missing"});
                continue;
            }
            if (found.channel->status == ChannelStatus::Error) {
                steps.push_back(ResolutionStep{ResolutionStep::Kind::Alias, alias->name, cand.name,
                                               std::string(kSkipped) + "status Error (W-ALIAS-FALLBACK)"});
                continue;
            }
            steps.push_back(ResolutionStep{ResolutionStep::Kind::Alias, alias->name, cand.name, {}});
            steps.insert(steps.end(), std::make_move_iterator(sub.begin()),
                         std::make_move_iterator(sub.end()));
            chain.pop_back();
            out.channel = found.channel;
            out.alias = alias;
            return out;
        }
        chain.pop_back();
        steps.push_back(ResolutionStep{ResolutionStep::Kind::Missing,
                                       alias->name,
                                       {},
                                       "no candidate of alias " + quoteName(alias->name) + " resolved"});
        return out;
    }

    // 3-5. Canonical name, logged name, missing.
    lookupChannel(current, layer, out, steps);
    return out;
}

ChannelResolver::Resolution ChannelResolver::resolve(std::string_view name, const DataLayer& layer,
                                                     const SourceId& source) const {
    Resolution res;
    res.trace.requested = std::string(name);
    std::vector<const AliasRow*> chain;
    Outcome out = resolveName(name, layer, nullptr, chain, res.trace.steps);
    if (!out.error.empty()) {
        res.trace.steps.push_back(
            ResolutionStep{ResolutionStep::Kind::Missing, std::string(name), {}, out.error});
        return res;
    }
    if (out.channel == nullptr) {
        return res;
    }
    res.channel = out.channel;
    res.trace.result = out.channel->desc.id;
    if (registry_ != nullptr && out.channel->desc.unit != units::kNoUnit &&
        registry_->contains(out.channel->desc.unit)) {
        res.trace.unitLabel = std::string(registry_->unit(out.channel->desc.unit).label());
    }
    if (out.alias != nullptr) {
        res.alias = makeAliasDescriptor(out.channel->desc, *out.alias, source, layer.name);
    }
    return res;
}

ChannelDescriptor makeAliasDescriptor(const ChannelDescriptor& target, const AliasRow& alias,
                                      const SourceId& source, std::string_view layer) {
    ChannelDescriptor d = target;
    d.id = channelIdOther(ChannelSource::Alias, source, layer, alias.name);
    d.name = alias.name;
    d.shortName.clear();
    d.source = ChannelSource::Alias;
    d.colourIndex = alias.colourIndex;
    d.dps = alias.dps.value_or(target.dps);
    d.sourceEncoding.reset();
    return d;
}

std::vector<MappingOutcome> applyMappings(DataLayer& layer, const MappingTable& mappings,
                                          const SourceId& source, const units::UnitRegistry* registry) {
    std::vector<MappingOutcome> outcomes;
    const ChannelResolver index(mappings, {}, nullptr);
    for (std::size_t i = 0; i < layer.channels.size(); ++i) {
        ChannelDescriptor& d = layer.channels[i].desc;
        if (d.source != ChannelSource::Logged) {
            continue;
        }
        const MappingRow* row = index.findMapping(d.loggedName);
        if (row == nullptr) {
            continue;
        }
        MappingOutcome o;
        o.channel = static_cast<std::uint32_t>(i);
        o.from = d.loggedName;
        o.to = row->to;
        d.name = row->to;
        d.shortName.clear();
        if (row->unit && (!row->unitlessOnly || d.unit == units::kNoUnit)) {
            d.unit = *row->unit;
            if (registry != nullptr && registry->contains(*row->unit)) {
                d.quantity = registry->quantityOf(*row->unit);
            }
            d.source = ChannelSource::Mapped;
            d.id = channelIdOther(ChannelSource::Mapped, source, layer.name, d.name);
            o.unitStamped = true;
        }
        outcomes.push_back(std::move(o));
    }
    layer.rebuildIndex();
    return outcomes;
}

} // namespace esses::model
