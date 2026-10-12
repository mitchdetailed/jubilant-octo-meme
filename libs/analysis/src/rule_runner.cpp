// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/analysis/beacon_pair_rule.hpp>
#include <esses/analysis/rule_runner.hpp>
#include <esses/analysis/whole_outing_rule.hpp>
#include <esses/base/utf8.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>

namespace esses::analysis {

namespace {

void stripAnnotations(model::RangeGroupTree& tree) {
    tree.forEach([](model::RangeGroup& g) {
        for (model::Range& r : g.ranges) {
            r.stint = 0;
            r.pitStop = false;
        }
    });
}

// Copies the phase-2 annotations of `previous` onto the ranges of `next` with the same group path, id, start
// and end, so a phase-1 rerun that leaves a lap unchanged keeps its annotations until phase 2 lands again.
void carryAnnotations(model::RangeGroupTree& next, const model::RangeGroupTree& previous) {
    next.forEach([&](model::RangeGroup& g) {
        const model::RangeGroup* old = previous.find(g.path);
        if (old == nullptr || old->ranges.empty()) {
            return;
        }
        std::unordered_map<model::RangeId, const model::Range*> byId;
        byId.reserve(old->ranges.size());
        for (const model::Range& r : old->ranges) {
            byId.emplace(r.id, &r);
        }
        for (model::Range& r : g.ranges) {
            const auto it = byId.find(r.id);
            if (it != byId.end() && it->second->start == r.start && it->second->end == r.end) {
                r.stint = it->second->stint;
                r.pitStop = it->second->pitStop;
            }
        }
    });
}

// RangeGroupTree::inheritVersions with the annotations ignored: annotations never bump a group version, so
// they invalidate no maths (D-140).
void inheritVersionsIgnoringAnnotations(model::RangeGroupTree& next, const model::RangeGroupTree& previous) {
    model::RangeGroupTree a = next;
    model::RangeGroupTree b = previous;
    stripAnnotations(a);
    stripAnnotations(b);
    a.inheritVersions(b);
    std::map<std::string, std::uint64_t, std::less<>> versions;
    a.forEach([&](const model::RangeGroup& g) { versions.emplace(g.path, g.version); });
    next.forEach([&](model::RangeGroup& g) {
        const auto it = versions.find(g.path);
        g.version = it != versions.end() ? it->second : 1;
    });
}

} // namespace

RuleRunner::RuleRunner(std::vector<std::shared_ptr<const IRangeRule>> rules) : rules_(std::move(rules)) {}

Result<RuleRunner> RuleRunner::fromRuleIds(const std::filesystem::path& rulesDir,
                                           std::span<const std::string> ruleIds,
                                           std::vector<std::string>* skipped) {
    std::vector<std::shared_ptr<const IRangeRule>> rules;
    for (const std::string& id : ruleIds) {
        const std::filesystem::path path = rulesDir / pathFromUtf8(id + ".rule.json");
        auto doc = RuleDocument::load(path);
        if (!doc) {
            return Failure{doc.error()};
        }
        if (doc->id != id) {
            Error e{
                ErrorCode::Invalid, "rule file '" + pathToUtf8(path) + "' declares id '" + doc->id + "'", {}};
            e.context.path = path;
            e.context.detail = "/id";
            return Failure{std::move(e)};
        }
        auto rule = makeRangeRule(*doc);
        if (!rule) {
            if (rule.error().code == ErrorCode::Unsupported) {
                if (skipped != nullptr) {
                    skipped->push_back(id);
                }
                continue;
            }
            Error e = rule.error();
            e.context.path = path;
            return Failure{std::move(e)};
        }
        rules.push_back(std::shared_ptr<const IRangeRule>(std::move(rule).value()));
    }
    return RuleRunner(std::move(rules));
}

RuleRunner RuleRunner::circuitDefault() {
    std::vector<std::shared_ptr<const IRangeRule>> rules;
    rules.push_back(std::make_shared<const WholeOutingRule>());
    rules.push_back(std::make_shared<const BeaconPairRule>(BeaconPairSettings::circuitDefault()));
    return RuleRunner(std::move(rules));
}

model::SidecarApplyReport RuleRunner::run(model::DataSource& src) const {
    model::SidecarApplyReport report;
    const RuleContext context{src.details};
    for (model::DataLayer& layer : src.layers) {
        const model::RangeGroupTree previous = layer.ranges;
        model::RangeGroupTree next;
        for (const std::shared_ptr<const IRangeRule>& rule : rules_) {
            if (!rule || rule->phase() != 1 || !rule->appliesTo(layer.name)) {
                continue;
            }
            auto produced = rule->run(layer, context);
            if (!produced) {
                continue; // a rule never fails on valid markers; a failing rule produces nothing
            }
            for (model::RangeGroup& g : produced->roots) {
                next.setGroup(std::move(g)); // a group whose parent is missing is dropped
            }
        }
        layer.ranges = std::move(next);
        report.append(src.sidecar.applyRangeOverrides(layer));
        carryAnnotations(layer.ranges, previous);
        inheritVersionsIgnoringAnnotations(layer.ranges, previous);
    }
    return report;
}

model::DataSource::RangeRebuild RuleRunner::rebuildFunction() const {
    return [rules = rules_](model::DataSource& src) { return RuleRunner(rules).run(src); };
}

Result<model::RangeGroupTree> RuleRunner::runPhase2(const model::DataSource& src, std::string_view layer,
                                                    const std::optional<store::Series>& speed,
                                                    const CancelToken& cancel) const {
    const model::DataLayer* l = src.layer(layer);
    if (l == nullptr) {
        Error e{ErrorCode::NotFound, "no layer '" + std::string(layer) + "' in the source", {}};
        e.context.layer = std::string(layer);
        return Failure{std::move(e)};
    }
    model::RangeGroupTree tree = l->ranges;
    for (const std::shared_ptr<const IRangeRule>& rule : rules_) {
        const auto* beacons = dynamic_cast<const BeaconPairRule*>(rule.get());
        if (beacons == nullptr || !beacons->appliesTo(l->name)) {
            continue;
        }
        model::RangeGroup* laps = tree.find(beacons->settings().produces);
        if (laps == nullptr) {
            continue;
        }
        if (auto annotated = beacons->annotate(*l, speed, *laps, cancel); !annotated) {
            return Failure{annotated.error()};
        }
    }
    return tree;
}

} // namespace esses::analysis
