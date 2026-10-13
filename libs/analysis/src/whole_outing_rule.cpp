// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "rule_json.hpp"

#include <esses/analysis/whole_outing_rule.hpp>
#include <esses/base/json_nlohmann.hpp>

#include <utility>
#include <vector>

namespace esses::analysis {

WholeOutingRule::WholeOutingRule(model::WholeOutingOptions options, std::string id,
                                 std::vector<std::string> layers)
    : options_(std::move(options)), id_(std::move(id)), layers_(std::move(layers)) {}

Result<std::unique_ptr<WholeOutingRule>> WholeOutingRule::fromDocument(const RuleDocument& document) {
    if (document.kind != "whole-outing") {
        Error e{ErrorCode::Invalid,
                "rule '" + document.id + "' is of kind '" + document.kind + "', not whole-outing",
                {}};
        e.context.detail = "/kind";
        return Failure{std::move(e)};
    }
    auto parsed = jsonio::parseJson(document.json.compact());
    if (!parsed) {
        return Failure{parsed.error()};
    }
    return jsonio::guardJson([&]() -> Result<std::unique_ptr<WholeOutingRule>> {
        using jsonio::asBool;
        using jsonio::asString;
        using jsonio::violate;
        const nlohmann::json& j = *parsed;
        if (!j.is_object()) {
            violate("", "a rule-1 document is a JSON object");
        }
        jsonio::ObjectReader r(j, "");
        detail::skipEnvelope(r);
        r.skip("id");
        r.skip("kind");
        r.skip("phase");
        r.skip("layers");
        model::WholeOutingOptions o;
        if (const nlohmann::json* p = r.optional("produces")) {
            o.groupPath = asString(*p, r.child("produces"));
            if (o.groupPath.empty() || o.groupPath.find(':') != std::string::npos) {
                violate(r.child("produces"), "a whole-outing rule produces a root group");
            }
        }
        if (const nlohmann::json* p = r.optional("parent")) {
            if (!asString(*p, r.child("parent")).empty()) {
                violate(r.child("parent"), "a whole-outing rule produces a root group");
            }
        }
        if (const nlohmann::json* p = r.optional("name")) {
            const std::string& name = asString(*p, r.child("name"));
            if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'') {
                o.nameDetail = name.substr(1, name.size() - 2); // "'File Name'": the detail
            } else if (!name.empty()) {
                o.nameDetail.clear(); // a literal name
                o.fallbackName = name;
            } else {
                violate(r.child("name"), "name is \"'<detail>'\" or a literal name");
            }
        }
        if (const nlohmann::json* p = r.optional("abbrev")) {
            if (asString(*p, r.child("abbrev")) != "{outingIndex}") {
                violate(r.child("abbrev"), "only \"{outingIndex}\" is supported");
            }
        }
        if (const nlohmann::json* p = r.optional("trusted")) {
            o.trusted = asBool(*p, r.child("trusted"));
        }
        if (const nlohmann::json* p = r.optional("groupFlags")) {
            jsonio::ObjectReader g(*p, r.child("groupFlags"));
            if (const nlohmann::json* q = g.optional("supportsTrackGeneration")) {
                o.supportsTrackGeneration = asBool(*q, g.child("supportsTrackGeneration"));
            }
            if (const nlohmann::json* q = g.optional("supportsReporting")) {
                o.supportsReporting = asBool(*q, g.child("supportsReporting"));
            }
            g.closed();
        }
        r.closed();
        std::vector<std::string> layers =
            document.layers.empty() ? std::vector<std::string>{"*"} : document.layers;
        return std::make_unique<WholeOutingRule>(std::move(o), document.id, std::move(layers));
    });
}

bool WholeOutingRule::appliesTo(std::string_view layerName) const {
    return layerListMatches(layers_, layerName);
}

Result<model::RangeGroupTree> WholeOutingRule::run(const model::DataLayer& layer,
                                                   const RuleContext& context) const {
    const std::vector<model::Detail> details(context.details.begin(), context.details.end());
    model::RangeGroupTree tree;
    tree.roots.push_back(model::buildWholeOutingGroup(layer, details, options_));
    return tree;
}

} // namespace esses::analysis
