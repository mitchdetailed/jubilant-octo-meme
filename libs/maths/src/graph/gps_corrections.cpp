// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// esses.gps_corrections v1 (03 section 9.2, D-016, D-024): from M2 only step 1, the zero-fix validity step,
// which runs whatever the module's `enabled` flag says: wherever latitude and longitude are both exactly 0 (a
// receiver without a fix), both channels are invalid. The outputs shadow the inputs ("shadow", the default:
// self-maths, every consumer reads the cleaned channels) or are new channels named with a prefix. A layer
// without both channels shows the outputs Disabled ("Disabled (no GPS channels)"), never an error. Steps 2-4
// (status channel, duplicate removal, time shift) and the Corrected GPS dialog are v1.0; their settings are
// validated and kept but not applied.

#include <esses/base/casefold.hpp>
#include <esses/maths/gps_corrections.hpp>
#include <esses/maths/whole_signal.hpp>
#include <esses/model/layer.hpp>
#include <esses/model/source.hpp>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace esses::maths {

namespace {

using analysis::UniformSignal;

// The settings object of 03 section 9.2; OpaqueJson::parse makes it canonical.
constexpr std::string_view kDefaultSettings = R"({
  "latitude": "GPS Latitude", "longitude": "GPS Longitude", "speed": "GPS Speed",
  "additionalChannels": ["GPS Heading", "GPS Altitude"],
  "statusChannel": {"name": "GPS Sats Used", "validWhen": ">= 4"},
  "duplicateRemoval": {"enabled": true},
  "timeShift": {"percentOfUpdatePeriod": 80},
  "outputs": "shadow"
})";

constexpr std::string_view kNoGps = "Disabled (no GPS channels)";

struct GpsSettings {
    std::string latitude = "GPS Latitude";
    std::string longitude = "GPS Longitude";
    std::optional<std::string> prefix; // nullopt: "shadow"
};

Error settingsError(std::string message, std::string pointer) {
    ErrorContext c;
    c.detail = std::move(pointer);
    c.schemaId = std::string(GpsCorrectionsModule::kSettingsSchemaId);
    return Error{ErrorCode::Invalid, std::move(message), std::move(c)};
}

Diagnostic settingsDiagnostic(const Error& error) {
    Diagnostic d;
    d.code = "E-GPS-SETTINGS";
    d.level = Diagnostic::Error;
    d.message = error.message;
    if (error.context.detail) {
        d.hint = "at " + (error.context.detail->empty() ? std::string("the settings object")
                                                        : "'" + *error.context.detail + "'");
    }
    return d;
}

Result<GpsSettings> parseSettings(const model::OpaqueJson& settings) {
    GpsSettings s;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(settings.compact());
    } catch (const std::exception& e) {
        return Failure{settingsError(std::string("the settings are not JSON: ") + e.what(), "")};
    }
    if (!j.is_object()) {
        return Failure{settingsError("the settings of esses.gps_corrections are an object", "")};
    }
    const auto name = [&](const char* key, std::string& out) -> Result<void> {
        if (!j.contains(key)) {
            return ok();
        }
        const nlohmann::json& v = j.at(key);
        if (!v.is_string() || v.get<std::string>().find_first_not_of(" \t") == std::string::npos) {
            return Failure{settingsError(std::string(key) + " is a channel name", std::string("/") + key)};
        }
        out = v.get<std::string>();
        return ok();
    };
    if (Result<void> r = name("latitude", s.latitude); !r) {
        return Failure{r.error()};
    }
    if (Result<void> r = name("longitude", s.longitude); !r) {
        return Failure{r.error()};
    }
    if (caseFold(s.latitude) == caseFold(s.longitude)) {
        return Failure{settingsError("latitude and longitude need two different channels", "/longitude")};
    }
    if (j.contains("outputs")) {
        const nlohmann::json& o = j.at("outputs");
        if (o.is_string() && o.get<std::string>() == "shadow") {
            s.prefix.reset();
        } else if (o.is_object() && o.contains("prefix") && o.at("prefix").is_string() &&
                   !o.at("prefix").get<std::string>().empty()) {
            s.prefix = o.at("prefix").get<std::string>();
        } else {
            return Failure{settingsError(R"(outputs is "shadow" or {"prefix": text})", "/outputs")};
        }
    }
    return s;
}

const model::Channel* findChannel(const model::DataLayer& layer, const std::string& name) {
    const model::Channel* ch = layer.find(name);
    return ch != nullptr ? ch : layer.findLogged(name);
}

// One output: `own` with the zero fixes of (latitude, longitude) invalid.
Result<UniformSignal> zeroFix(const EvalContext& ctx, const GpsSettings& s, bool latitude,
                              const units::Converter& convert) {
    const model::DataLayer* layer = ctx.source != nullptr ? ctx.source->layer(ctx.layer) : nullptr;
    const model::Channel* lat = layer != nullptr ? findChannel(*layer, s.latitude) : nullptr;
    const model::Channel* lon = layer != nullptr ? findChannel(*layer, s.longitude) : nullptr;
    if (lat == nullptr || lon == nullptr) {
        ErrorContext c;
        c.detail = "E-GPS-NO-CHANNELS";
        return fail(ErrorCode::NotFound, "no '" + s.latitude + "' and '" + s.longitude + "' channels",
                    std::move(c));
    }
    const model::Channel& ownChannel = latitude ? *lat : *lon;
    const model::Channel& otherChannel = latitude ? *lon : *lat;
    Result<UniformSignal> own = readWholeSignal(ownChannel.series, ctx.cancel);
    if (!own) {
        return own;
    }
    Result<UniformSignal> other = readWholeSignal(otherChannel.series, ctx.cancel);
    if (!other) {
        return Failure{other.error()};
    }
    UniformSignal& out = *own;
    const UniformSignal& o = *other;
    const bool sameGrid = out.rate == o.rate && out.t0 == o.t0;
    std::vector<std::uint8_t> valid = out.valid;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (!out.validAt(i) || out.values[i] != 0.0) {
            continue;
        }
        std::size_t j = i;
        if (!sameGrid) {
            const SampleIdx k = o.rate.indexAt(out.rate.timeAt(static_cast<SampleIdx>(i), out.t0), o.t0);
            if (k < 0 || static_cast<std::size_t>(k) >= o.size()) {
                continue;
            }
            j = static_cast<std::size_t>(k);
        } else if (j >= o.size()) {
            continue;
        }
        if (o.validAt(j) && o.values[j] == 0.0) {
            if (valid.empty()) {
                valid.assign(out.size(), 1);
            }
            valid[i] = 0;
            out.values[i] = std::numeric_limits<double>::quiet_NaN();
        }
    }
    out.valid = std::move(valid);
    if (!convert.identity()) {
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (out.validAt(i)) {
                out.values[i] = convert(out.values[i]);
            }
        }
    }
    return out;
}

Result<std::string> readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    ErrorContext c;
    c.path = path;
    if (!in) {
        return fail(ErrorCode::Io, "cannot open the settings schema", std::move(c));
    }
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
        return fail(ErrorCode::Io, "cannot read the settings schema", std::move(c));
    }
    return text;
}

} // namespace

model::OpaqueJson GpsCorrectionsModule::defaultSettings() const {
    Result<model::OpaqueJson> parsed = model::OpaqueJson::parse(kDefaultSettings);
    return parsed ? std::move(parsed).value() : model::OpaqueJson{};
}

Result<std::vector<BuiltinOutput>> GpsCorrectionsModule::expand(const model::OpaqueJson& settings,
                                                                const units::UnitRegistry& units,
                                                                Diagnostics& diags) const {
    const model::OpaqueJson effective = settings.isNull() ? defaultSettings() : settings;
    if (!schema_.isNull()) {
        if (Result<void> v = validateSettings(*this, effective); !v) {
            diags.push_back(settingsDiagnostic(v.error()));
            return Failure{v.error()};
        }
    }
    Result<GpsSettings> parsed = parseSettings(effective);
    if (!parsed) {
        diags.push_back(settingsDiagnostic(parsed.error()));
        return Failure{parsed.error()};
    }
    auto s = std::make_shared<const GpsSettings>(std::move(parsed).value());
    const std::optional<units::QuantityId> angle = units.quantityByName("Angle");
    std::vector<BuiltinOutput> outputs;
    for (const bool latitude : {true, false}) {
        const std::string& input = latitude ? s->latitude : s->longitude;
        BuiltinOutput o;
        o.name = s->prefix ? *s->prefix + input : input;
        o.inputs = {s->latitude, s->longitude};
        o.required = {s->latitude, s->longitude};
        o.missingSummary = std::string(kNoGps);
        o.dtype = store::Dtype::F64; // 03 section 9.2 item 4
        o.interp = store::Interp::Linear;
        o.display.dps = 6;
        o.display.colourIndex = -1;
        if (s->prefix && angle) {
            // A new channel: the values in the Angle base unit (a shadowing output keeps the input's unit).
            o.quantity = *angle;
            o.resultUnit = units.quantity(*angle).base;
        }
        const bool prefixed = s->prefix.has_value();
        o.evaluate = [s, latitude, prefixed](const EvalContext& ctx,
                                             std::span<const UniformSignal> /*upstream*/,
                                             Diagnostics& /*out*/) -> Result<UniformSignal> {
            units::Converter toBase;
            if (prefixed && ctx.units != nullptr && ctx.source != nullptr) {
                if (const model::DataLayer* layer = ctx.source->layer(ctx.layer)) {
                    if (const model::Channel* ch =
                            findChannel(*layer, latitude ? s->latitude : s->longitude)) {
                        toBase = ctx.units->toBase(ch->desc.unit);
                    }
                }
            }
            return zeroFix(ctx, *s, latitude, toBase);
        };
        outputs.push_back(std::move(o));
    }
    return outputs;
}

Result<void> GpsCorrectionsModule::loadSchema(const std::filesystem::path& schemasDir) {
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

} // namespace esses::maths
