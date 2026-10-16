// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// IRangeRule and the rule-1 document envelope (02 section 4.5 "Declarative range rules", 02 section 4.4 rules
// 9-13, ARCHITECTURE_OUTLINE section 4 esses_analysis, 01 section 10 "Range rule", D-014).
//
// Rules are data (data/rules/*.rule.json, schema rule-1) consumed by IRangeRule implementations keyed by
// `kind`. Phase-1 rules read markers and details only, never a sample, and run synchronously on the GUI
// thread (or inside the open task); phase-2 work (beacon-pair annotations, track sections) reads pinned
// columns on a worker (02 section 4.4 rule 13).
//
// Naming note: 01 section 10 and D-014 write the rule signature as `run(const DataLayer&, const Profile&)`.
// `Profile` is an esses_project document and esses_project links esses_analysis (same layer, 01 section 2.2),
// so the profile reaches the rules as the ordered list of rule documents it names (RuleRunner::fromRuleIds)
// plus this RuleContext; no esses_project type appears here.
#pragma once

#include <esses/base/json_doc.hpp>
#include <esses/base/result.hpp>
#include <esses/model/detail.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/opaque_json.hpp> // compatibility: model::OpaqueJson names JsonDoc
#include <esses/model/range.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace esses::analysis {

// The envelope of one rule-1 document; `json` keeps the whole document for the kind-specific parser.
struct RuleDocument {
    std::string id;                  // "circuit.beacon-pair"
    std::string kind;                // "beacon-pair" | "whole-outing" | "track-sections" | "detail-group"
    int phase = 1;                   // 1: markers and details only; 2: pinned columns on a worker
    std::string produces;            // "Outings:Laps"
    std::string parent;              // "Outings"; "" for a root group
    std::vector<std::string> layers; // ["Normal", "Fastest"], or ["*"] for every layer
    JsonDoc json;                    // the complete document

    // Format/Invalid with context.detail (a JSON pointer) on a malformed document; Unsupported for another
    // major version of the schema. Checks the envelope only ($schema rule-1, id, kind, phase, produces,
    // parent, layers; `layers` defaults to ["*"]); the kind-specific members are read by the rule
    // (BeaconPairSettings::fromDocument, WholeOutingRule::fromDocument).
    static Result<RuleDocument> parse(std::string_view json);
    // Io with context.path when the file cannot be read; parse() errors carry the path too.
    static Result<RuleDocument> load(const std::filesystem::path& path);
    // True when `layers` lists the layer (canonical names compared; "*" matches every layer).
    bool appliesTo(std::string_view layerName) const;
};

// True when `layers` holds "*" or the layer's canonical name ("" and "Normal" are the same layer; names are
// compared case-insensitively).
bool layerListMatches(std::span<const std::string> layers, std::string_view layerName);

// What a rule reads besides the layer: the source's merged details ("Start Lap", "File Name", ...).
struct RuleContext {
    std::span<const model::Detail> details;
};

class IRangeRule {
public:
    virtual ~IRangeRule() = default;

    virtual std::string_view id() const = 0;   // the rule document id
    virtual std::string_view kind() const = 0; // the rule-1 kind
    virtual int phase() const = 0;             // 1 or 2
    // The document's `layers` (RuleDocument::appliesTo); RuleRunner skips the layers a rule does not name.
    virtual bool appliesTo(std::string_view layerName) const {
        static_cast<void>(layerName);
        return true;
    }

    // Phase 1: a pure function of the layer's enabled markers (model order) and the details. Returns the
    // groups the rule produces as the tree's `roots`, each with its full path ("Outings", "Outings:Laps"),
    // in the order RuleRunner inserts them with RangeGroupTree::setGroup; every version 0 (RuleRunner carries
    // versions over, D-140). An empty tree means the rule produces nothing for this layer. Must not read a
    // sample and must not throw.
    virtual Result<model::RangeGroupTree> run(const model::DataLayer& layer,
                                              const RuleContext& context) const = 0;

protected:
    IRangeRule() = default;
    IRangeRule(const IRangeRule&) = default;
    IRangeRule& operator=(const IRangeRule&) = default;
};

// Factory by kind: "beacon-pair" -> BeaconPairRule, "whole-outing" -> WholeOutingRule, "track-sections" ->
// TrackSectionsRule (phase 2); Unsupported for "detail-group".
Result<std::unique_ptr<IRangeRule>> makeRangeRule(const RuleDocument& document);

} // namespace esses::analysis
