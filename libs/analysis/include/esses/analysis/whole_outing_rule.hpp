// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// WholeOutingRule: "circuit.whole-outing" (02 section 4.5) as an IRangeRule. It wraps esses_model's
// buildWholeOutingGroup (libs/model whole_outing.hpp, the M0 rule `esses-cli laps` used), so the two cannot
// drift: one Range of kind Outing per logged session of a layer in group "Outings", named after the "File
// Name" detail.
#pragma once

#include <esses/analysis/range_rule.hpp>
#include <esses/base/result.hpp>
#include <esses/model/whole_outing.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace esses::analysis {

class WholeOutingRule final : public IRangeRule {
public:
    explicit WholeOutingRule(model::WholeOutingOptions options = {}, std::string id = "circuit.whole-outing",
                             std::vector<std::string> layers = {"*"});
    // A "whole-outing" rule-1 document: produces (a root path), name ("'<detail>'"), abbrev (only
    // "{outingIndex}" is supported), trusted, groupFlags, layers. Invalid with context.detail otherwise.
    static Result<std::unique_ptr<WholeOutingRule>> fromDocument(const RuleDocument& document);

    std::string_view id() const override { return id_; }
    std::string_view kind() const override { return "whole-outing"; }
    int phase() const override { return 1; }
    bool appliesTo(std::string_view layerName) const override;
    Result<model::RangeGroupTree> run(const model::DataLayer& layer,
                                      const RuleContext& context) const override;

    const model::WholeOutingOptions& options() const noexcept { return options_; }

private:
    model::WholeOutingOptions options_;
    std::string id_;
    std::vector<std::string> layers_;
};

} // namespace esses::analysis
