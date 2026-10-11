// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// Smoke tests of the vendored third-party targets (cmake/third_party.cmake): each library links, its headers
// compile warning-free under the project's warning flags (/W4 /WX in the ci presets) through SYSTEM includes,
// and the features the design relies on work (08 section 3.7.2).
#include <CLI/CLI11.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fast_float/fast_float.h>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include <string>
#include <string_view>
#include <system_error>

TEST_CASE("nlohmann_json: parse, access and dump round-trip", "[unit]") {
    const auto doc = nlohmann::json::parse(R"({"name":"Oil Temp","rate":12.5,"ok":true,"unit":"\u00b0C"})");
    CHECK(doc.at("name").get<std::string>() == "Oil Temp");
    CHECK(doc.at("rate").get<double>() == 12.5);
    CHECK(doc.at("ok").get<bool>());
    CHECK(doc.at("unit").get<std::string>() == "\xC2\xB0"
                                               "C");
    CHECK(nlohmann::json::parse(doc.dump()).dump() == doc.dump());
    CHECK_THROWS_AS(nlohmann::json::parse("{\"unterminated\": "), nlohmann::json::parse_error);
}

TEST_CASE("json_schema_validator: a draft-7 schema accepts and rejects documents", "[unit]") {
    const nlohmann::json schema = nlohmann::json::parse(R"({
        "$schema": "http://json-schema.org/draft-07/schema#",
        "type": "object",
        "required": ["schemaVersion"],
        "properties": {"schemaVersion": {"type": "integer", "minimum": 1}}
    })");
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(schema);

    nlohmann::json_schema::basic_error_handler accepted;
    validator.validate(nlohmann::json::parse(R"({"schemaVersion": 1})"), accepted);
    CHECK_FALSE(static_cast<bool>(accepted));

    nlohmann::json_schema::basic_error_handler rejected;
    validator.validate(nlohmann::json::parse(R"({"schemaVersion": 0})"), rejected);
    CHECK(static_cast<bool>(rejected));

    CHECK_THROWS(validator.validate(nlohmann::json::parse("{}")));
}

TEST_CASE("cli11: options, flags, positionals and required options", "[unit]") {
    CLI::App app{"third-party smoke test"};
    double rate = 0.0;
    bool json = false;
    std::string input;
    app.add_option("--rate", rate, "sample rate in Hz")->required();
    app.add_flag("--json", json, "machine-readable output");
    app.add_option("input", input, "input file");
    REQUIRE_NOTHROW(app.parse("--rate 12.5 --json run.ld", false));
    CHECK(rate == 12.5);
    CHECK(json);
    CHECK(input == "run.ld");

    CLI::App strict{"strict"};
    double unused = 0.0;
    strict.add_option("--rate", unused)->required();
    CHECK_THROWS_AS(strict.parse("", false), CLI::RequiredError);
}

TEST_CASE("fast_float: exact parsing, decimal comma and errors", "[unit]") {
    double value = 0.0;
    const std::string_view text = "-1.25e-3";
    auto result = fast_float::from_chars(text.data(), text.data() + text.size(), value);
    CHECK(result.ec == std::errc());
    CHECK(result.ptr == text.data() + text.size());
    CHECK(value == -1.25e-3);

    const std::string_view comma = "3,5";
    const fast_float::parse_options options{fast_float::chars_format::general, ','};
    result = fast_float::from_chars_advanced(comma.data(), comma.data() + comma.size(), value, options);
    CHECK(result.ec == std::errc());
    CHECK(value == 3.5);

    float single = 0.0f;
    const std::string_view tenth = "0.1";
    CHECK(fast_float::from_chars(tenth.data(), tenth.data() + tenth.size(), single).ec == std::errc());
    CHECK(single == 0.1f);

    const std::string_view junk = "abc";
    CHECK(fast_float::from_chars(junk.data(), junk.data() + junk.size(), value).ec ==
          std::errc::invalid_argument);
}

TEST_CASE("pugixml: parse a document and read attributes", "[unit]") {
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed =
        doc.load_string(R"(<root locale="en_AU"><layers><details count="2"/></layers></root>)");
    REQUIRE(parsed);
    CHECK(std::string(doc.child("root").attribute("locale").value()) == "en_AU");
    CHECK(doc.child("root").child("layers").child("details").attribute("count").as_int() == 2);
    CHECK_FALSE(doc.load_string("<unclosed>"));
}
