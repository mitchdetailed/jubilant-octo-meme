// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT

#include <esses/base/casefold.hpp>
#include <esses/base/paths.hpp>
#include <esses/maths/corrected_speed_distance.hpp>
#include <esses/maths/whole_signal.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/source.hpp>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace esses::maths {

namespace {

using analysis::CorrectedDistance;
using analysis::CorrectedSpeed;
using analysis::CorrectedSpeedSettings;
using analysis::UniformSignal;

// The settings object of 03 section 9.1 (method fastestWheel); OpaqueJson::parse makes it canonical.
constexpr std::string_view kDefaultSettings = R"({
  "method": "fastestWheel",
  "fastestWheel": {"channels": ["Wheel Speed FL", "Wheel Speed FR"]},
  "alternateList": {"channels": ["Ground Speed", "Wheel Speed FL", "Wheel Speed FR", "Wheel Speed Front",
                                 "Wheel Speed", "Wheel Speed RL", "Wheel Speed RR", "Wheel Speed Rear",
                                 "Drive Speed", "GPS Speed", "Speed"]},
  "gCorrection": {"speedChannel": "Wheel Speed FL", "longGChannel": "G Force Long",
                  "lockThreshold": {"v": 2.0, "unit": "m/s"}, "recoverSeconds": 0.5},
  "clamp": {"min": {"v": 0, "unit": "km/h"}, "minInvalidate": false, "max": null, "maxInvalidate": false},
  "distance": {"source": "integrate", "loggedChannel": "Lap Distance", "integration": "trapezoid"},
  "outputs": {"speed": "Corr Speed", "distance": "Corr Dist"}
})";

Diagnostic settingsDiagnostic(const Error& error) {
    Diagnostic d;
    d.code = "E-CSD-SETTINGS";
    d.level = Diagnostic::Error;
    d.message = error.message;
    if (error.context.detail) {
        d.hint = "at " + (error.context.detail->empty() ? std::string("the settings object")
                                                        : "'" + *error.context.detail + "'");
    }
    return d;
}

Diagnostic::Level levelOfCode(std::string_view code) {
    if (code.starts_with("I-")) {
        return Diagnostic::Info;
    }
    return Diagnostic::Warning; // a successful evaluation reports no errors
}

std::string describeCode(std::string_view code) {
    if (code == "W-CSD-FALLBACK") {
        return "a fastestWheel channel is missing; Corr Speed uses the alternate list";
    }
    return std::string(code);
}

void appendUnique(std::vector<std::string>& out, const std::string& name) {
    const std::string folded = caseFold(name);
    for (const std::string& have : out) {
        if (caseFold(have) == folded) {
            return;
        }
    }
    out.push_back(name);
}

std::vector<std::string> speedInputs(const CorrectedSpeedSettings& s) {
    std::vector<std::string> out;
    switch (s.method) {
    case analysis::SpeedMethod::FastestWheel:
        for (const std::string& c : s.fastestWheelChannels) {
            appendUnique(out, c);
        }
        for (const std::string& c : s.alternateListChannels) { // the fallback of 03 section 9.1 item 1
            appendUnique(out, c);
        }
        break;
    case analysis::SpeedMethod::AlternateList:
        for (const std::string& c : s.alternateListChannels) {
            appendUnique(out, c);
        }
        break;
    case analysis::SpeedMethod::GCorrection:
        appendUnique(out, s.gCorrection.speedChannel);
        appendUnique(out, s.gCorrection.longGChannel);
        break;
    }
    return out;
}

ErrorContext detailContext(std::string detail) {
    ErrorContext c;
    c.detail = std::move(detail);
    return c;
}

const model::DataLayer* layerOf(const EvalContext& ctx) {
    return ctx.source != nullptr ? ctx.source->layer(ctx.layer) : nullptr;
}

Result<std::string> readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    ErrorContext c;
    c.path = path;
    if (!in) {
        return fail(ErrorCode::Io, "cannot open the settings schema", std::move(c));
    }
    std::string text;
    try {
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } catch (const std::ios_base::failure&) {
        // libstdc++'s filebuf throws a failed read (a directory: EISDIR) whatever the exception mask
        in.setstate(std::ios_base::badbit);
    }
    if (in.bad()) {
        return fail(ErrorCode::Io, "cannot read the settings schema", std::move(c));
    }
    return text;
}

} // namespace

model::OpaqueJson CorrectedSpeedDistanceModule::defaultSettings() const {
    Result<model::OpaqueJson> parsed = model::OpaqueJson::parse(kDefaultSettings);
    return parsed ? std::move(parsed).value() : model::OpaqueJson{};
}

Result<std::vector<BuiltinOutput>> CorrectedSpeedDistanceModule::expand(const model::OpaqueJson& settings,
                                                                        const units::UnitRegistry& units,
                                                                        Diagnostics& diags) const {
    const model::OpaqueJson effective = settings.isNull() ? defaultSettings() : settings;
    if (!schema_.isNull()) {
        if (Result<void> v = validateSettings(effective); !v) {
            diags.push_back(settingsDiagnostic(v.error()));
            return Failure{v.error()};
        }
    }
    Result<CorrectedSpeedSettings> parsed = CorrectedSpeedSettings::fromJson(effective, units);
    if (!parsed) {
        diags.push_back(settingsDiagnostic(parsed.error()));
        return Failure{parsed.error()};
    }
    auto s = std::make_shared<const CorrectedSpeedSettings>(std::move(parsed).value());
    if (s->speedOutput.empty() || s->distanceOutput.empty() ||
        caseFold(s->speedOutput) == caseFold(s->distanceOutput)) {
        Error e{ErrorCode::Invalid, "the speed and distance outputs need two different names",
                detailContext("/outputs/distance")};
        e.context.schemaId = std::string(kSettingsSchemaId);
        diags.push_back(settingsDiagnostic(e));
        return Failure{std::move(e)};
    }

    const std::optional<units::QuantityId> speedQ = units.quantityByName("Speed");
    const std::optional<units::QuantityId> distQ = units.quantityByName("Distance");
    const std::optional<units::UnitId> mps = speedQ ? units.find("m/s", *speedQ) : std::nullopt;
    const std::optional<units::UnitId> metre = distQ ? units.find("m", *distQ) : std::nullopt;
    if (!mps || !metre) {
        return fail(ErrorCode::NotFound, "the unit table lacks Speed [m/s] or Distance [m]");
    }

    BuiltinOutput speed;
    speed.name = s->speedOutput;
    speed.inputs = speedInputs(*s);
    speed.quantity = *speedQ;
    speed.resultUnit = *mps;
    // Automatic colour; the display unit follows the first input channel (03 section 9.1 item 5).
    speed.display.colourIndex = -1;
    speed.interp = store::Interp::Linear;
    speed.dtype = store::Dtype::F32;
    speed.evaluate = [s](const EvalContext& ctx, std::span<const UniformSignal> /*upstream*/,
                         Diagnostics& out) -> Result<UniformSignal> {
        const model::DataLayer* layer = layerOf(ctx);
        if (layer == nullptr) {
            return fail(ErrorCode::NotFound, "the source has no layer '" + ctx.layer + "'",
                        detailContext("E-CSD-NO-SPEED"));
        }
        if (ctx.units == nullptr) {
            return fail(ErrorCode::Invalid, "no unit registry for the corrected speed");
        }
        Result<analysis::CorrectedSpeedResult> r =
            CorrectedSpeed::compute(*s, *layer, *ctx.units, ctx.cancel);
        if (!r) {
            return Failure{r.error()};
        }
        for (const std::string& code : r->diagnostics) {
            Diagnostic d;
            d.code = code;
            d.level = levelOfCode(code);
            d.message = describeCode(code);
            out.push_back(std::move(d));
        }
        if (!r->inputs.empty()) {
            Diagnostic d;
            d.code = "I-CSD-INPUTS";
            d.level = Diagnostic::Info;
            d.message = s->speedOutput + " from";
            for (std::size_t i = 0; i < r->inputs.size(); ++i) {
                d.message += (i == 0 ? " " : ", ") + r->inputs[i];
            }
            out.push_back(std::move(d));
        }
        return std::move(r->speedMps);
    };

    BuiltinOutput distance;
    distance.name = s->distanceOutput;
    distance.quantity = *distQ;
    distance.resultUnit = *metre;
    distance.display.colourIndex = -1; // automatic; the display unit follows the speed unit (item 6)
    distance.interp = store::Interp::Linear;
    distance.dtype = store::Dtype::F64;
    if (s->distanceSource == analysis::DistanceSource::Integrate) {
        distance.upstream = {s->speedOutput};
        distance.evaluate = [s](const EvalContext& /*ctx*/, std::span<const UniformSignal> upstream,
                                Diagnostics& /*out*/) -> Result<UniformSignal> {
            if (upstream.size() != 1) {
                return fail(ErrorCode::Internal, "Corr Dist needs exactly one upstream signal");
            }
            return CorrectedDistance::integrate(upstream.front(), s->integration);
        };
    } else {
        distance.inputs = {s->loggedDistanceChannel};
        const units::QuantityId distanceQuantity = *distQ;
        distance.evaluate = [s, distanceQuantity](const EvalContext& ctx, std::span<const UniformSignal>,
                                                  Diagnostics&) -> Result<UniformSignal> {
            const model::DataLayer* layer = layerOf(ctx);
            const model::Channel* channel =
                layer != nullptr ? layer->find(s->loggedDistanceChannel) : nullptr;
            if (channel == nullptr) {
                return fail(ErrorCode::NotFound,
                            "no channel '" + s->loggedDistanceChannel + "' for Corr Dist",
                            detailContext("E-CSD-NO-DISTANCE"));
            }
            units::Converter toMetres;
            if (ctx.units != nullptr && channel->desc.unit != units::kNoUnit &&
                ctx.units->contains(channel->desc.unit)) {
                if (ctx.units->quantityOf(channel->desc.unit) != distanceQuantity) {
                    return fail(ErrorCode::Invalid,
                                "channel '" + s->loggedDistanceChannel + "' is not a distance",
                                detailContext("E-CSD-NO-DISTANCE"));
                }
                toMetres = ctx.units->toBase(channel->desc.unit);
            } // a unitless lap distance is taken as metres
            Result<UniformSignal> logged = readWholeSignal(channel->series, ctx.cancel, toMetres);
            if (!logged) {
                return logged;
            }
            return CorrectedDistance::fromLogged(*logged);
        };
    }

    std::vector<BuiltinOutput> outputs;
    outputs.push_back(std::move(speed));
    outputs.push_back(std::move(distance));
    return outputs;
}

Result<void> CorrectedSpeedDistanceModule::loadSchema(const std::filesystem::path& schemasDir) {
    const std::filesystem::path path = schemasDir / std::filesystem::path(std::string(kSettingsSchemaFile));
    Result<std::string> text = readText(path);
    if (!text) {
        return Failure{text.error()};
    }
    ErrorContext context;
    context.path = path;
    context.schemaId = std::string(kSettingsSchemaId);
    Result<model::OpaqueJson> parsed = model::OpaqueJson::parse(*text);
    if (!parsed) {
        Error e = parsed.error();
        e.context.path = path;
        e.context.schemaId = context.schemaId;
        return Failure{std::move(e)};
    }
    try {
        const nlohmann::json doc = nlohmann::json::parse(parsed->compact());
        const std::string expectedId = "urn:esses:" + std::string(kSettingsSchemaId);
        if (!doc.is_object() || doc.value("$id", std::string()) != expectedId ||
            doc.value("$schema", std::string()) != "http://json-schema.org/draft-07/schema#") {
            return fail(ErrorCode::Format, "not the draft-07 schema " + expectedId, std::move(context));
        }
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(doc);
    } catch (const std::exception& e) {
        return fail(ErrorCode::Format, std::string("not a usable settings schema: ") + e.what(),
                    std::move(context));
    }
    schema_ = std::move(parsed).value();
    return ok();
}

Result<void> CorrectedSpeedDistanceModule::validateSettings(const model::OpaqueJson& settings) const {
    return maths::validateSettings(*this, settings);
}

std::filesystem::path CorrectedSpeedDistanceModule::findSchemasDir(const std::filesystem::path& dataDir) {
    std::vector<std::filesystem::path> candidates;
    if (!dataDir.empty()) {
        candidates.push_back(dataDir / "schemas");
    }
    if (const std::filesystem::path exe = Paths::executableDir(); !exe.empty()) {
        candidates.push_back(exe / "data" / "schemas");
    }
#ifdef ESSES_MATHS_SOURCE_DIR
    candidates.push_back(std::filesystem::path(ESSES_MATHS_SOURCE_DIR) / "schemas");
#endif
    const std::filesystem::path file{std::string(kSettingsSchemaFile)};
    for (const std::filesystem::path& dir : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(dir / file, ec)) {
            return dir;
        }
    }
    return {};
}

} // namespace esses::maths
