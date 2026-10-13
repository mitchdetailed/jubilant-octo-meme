// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include "rule_json.hpp"

#include <esses/analysis/beacon_pair_rule.hpp>
#include <esses/analysis/range_rule.hpp>
#include <esses/analysis/whole_outing_rule.hpp>
#include <esses/base/casefold.hpp>
#include <esses/base/json_nlohmann.hpp>
#include <esses/model/ids.hpp>

#include <utility>

namespace esses::analysis {

namespace {

using detail::stringArray;
using jsonio::asInt32;
using jsonio::asString;
using jsonio::ObjectReader;
using jsonio::violate;

} // namespace

bool layerListMatches(std::span<const std::string> layers, std::string_view layerName) {
    const std::string wanted = caseFold(model::canonicalLayerName(layerName));
    for (const std::string& l : layers) {
        if (l == "*" || caseFold(model::canonicalLayerName(l)) == wanted) {
            return true;
        }
    }
    return false;
}

Result<RuleDocument> RuleDocument::parse(std::string_view json) {
    auto parsed = jsonio::parseJson(json);
    if (!parsed) {
        return Failure{parsed.error()};
    }
    auto opaque = model::OpaqueJson::parse(json);
    if (!opaque) {
        return Failure{opaque.error()};
    }
    return jsonio::guardJson([&]() -> Result<RuleDocument> {
        const nlohmann::json& j = *parsed;
        if (!j.is_object()) {
            violate("", "a rule-1 document is a JSON object");
        }
        jsonio::checkSchemaId(j, "rule", 1);
        RuleDocument doc;
        ObjectReader r(j, "");
        doc.id = asString(r.required("id"), r.child("id"));
        if (doc.id.empty()) {
            violate(r.child("id"), "the rule id is empty");
        }
        doc.kind = asString(r.required("kind"), r.child("kind"));
        if (doc.kind.empty()) {
            violate(r.child("kind"), "the rule kind is empty");
        }
        if (const nlohmann::json* p = r.optional("phase")) {
            doc.phase = asInt32(*p, r.child("phase"));
            if (doc.phase != 1 && doc.phase != 2) {
                violate(r.child("phase"), "phase is 1 or 2");
            }
        }
        if (const nlohmann::json* p = r.optional("produces")) {
            doc.produces = asString(*p, r.child("produces"));
        }
        if (const nlohmann::json* p = r.optional("parent")) {
            doc.parent = asString(*p, r.child("parent"));
        }
        if (!doc.parent.empty() && !doc.produces.empty() && doc.produces.find('{') == std::string::npos &&
            model::parentGroupPath(doc.produces) != doc.parent) {
            violate(r.child("parent"),
                    "'" + doc.parent + "' is not the parent group of '" + doc.produces + "'");
        }
        if (const nlohmann::json* p = r.optional("layers")) {
            doc.layers = stringArray(*p, r.child("layers"));
            if (doc.layers.empty()) {
                violate(r.child("layers"), "a rule applies to at least one layer");
            }
        } else {
            doc.layers = {"*"};
        }
        doc.json = std::move(opaque).value();
        return doc;
    });
}

Result<RuleDocument> RuleDocument::load(const std::filesystem::path& path) {
    auto text = jsonio::readFile(path);
    if (!text) {
        return Failure{text.error()};
    }
    auto doc = parse(*text);
    if (!doc) {
        Error e = doc.error();
        e.context.path = path;
        return Failure{std::move(e)};
    }
    return doc;
}

bool RuleDocument::appliesTo(std::string_view layerName) const {
    return layerListMatches(layers, layerName);
}

Result<std::unique_ptr<IRangeRule>> makeRangeRule(const RuleDocument& document) {
    if (document.kind == "beacon-pair") {
        auto rule = BeaconPairRule::fromDocument(document);
        if (!rule) {
            return Failure{rule.error()};
        }
        return std::unique_ptr<IRangeRule>(std::move(rule).value());
    }
    if (document.kind == "whole-outing") {
        auto rule = WholeOutingRule::fromDocument(document);
        if (!rule) {
            return Failure{rule.error()};
        }
        return std::unique_ptr<IRangeRule>(std::move(rule).value());
    }
    if (document.kind == "track-sections" || document.kind == "detail-group") {
        Error e{ErrorCode::Unsupported,
                "rule kind '" + document.kind + "' (" + document.id +
                    ") is not implemented yet (track-sections: M3; detail-group: later)",
                {}};
        e.context.detail = document.kind;
        return Failure{std::move(e)};
    }
    Error e{ErrorCode::Invalid, "unknown rule kind '" + document.kind + "' (" + document.id + ")", {}};
    e.context.detail = "/kind";
    return Failure{std::move(e)};
}

} // namespace esses::analysis
