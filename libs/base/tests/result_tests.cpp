// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/result.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <sstream>
#include <string>

using esses::Error;
using esses::ErrorCode;
using esses::ErrorContext;
using esses::Failure;
using esses::Result;

namespace {

Result<int> parsePositive(int v) {
    if (v <= 0) {
        return esses::fail(ErrorCode::Invalid, "not positive");
    }
    return v;
}

Result<std::string> greeting() {
    return "hello"; // converts through the forwarding constructor
}

Result<void> check(bool good) {
    if (!good) {
        return esses::fail(ErrorCode::Io, "cannot open");
    }
    return esses::ok();
}

} // namespace

TEST_CASE("result: success and failure", "[unit][result]") {
    const Result<int> a = parsePositive(3);
    REQUIRE(a.ok());
    CHECK(static_cast<bool>(a));
    CHECK(a.value() == 3);
    CHECK(*a == 3);
    const Result<int> b = parsePositive(-1);
    REQUIRE_FALSE(b.ok());
    CHECK(b.error().code == ErrorCode::Invalid);
    CHECK(b.error().message == "not positive");
    CHECK(b.valueOr(7) == 7);
    CHECK(greeting().value() == "hello");
    CHECK(greeting()->size() == 5u);
    CHECK(esses::fail<double>(ErrorCode::Parse, "x").error().code == ErrorCode::Parse);
    CHECK(esses::ok(5).value() == 5);
    const std::string s = "copy";
    Result<std::string> fromLvalue =
        esses::ok(s); // decays: Result<std::string>, not Result<const std::string&>
    CHECK(fromLvalue.value() == "copy");
}

TEST_CASE("result: void results", "[unit][result]") {
    CHECK(check(true).ok());
    const Result<void> r = check(false);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Io);
    r.orElse([](const Error&) { return esses::ok(); }).value(); // recovers
}

TEST_CASE("result: move-only values", "[unit][result]") {
    Result<std::unique_ptr<int>> r = std::make_unique<int>(4);
    REQUIRE(r.ok());
    std::unique_ptr<int> p = std::move(r).value();
    CHECK(*p == 4);
}

TEST_CASE("result: map, andThen and orElse", "[unit][result]") {
    const auto doubled = parsePositive(5).map([](int v) { return v * 2; });
    CHECK(doubled.value() == 10);
    const auto failed = parsePositive(0).map([](int v) { return v * 2; });
    CHECK(failed.error().code == ErrorCode::Invalid);
    const auto chained = parsePositive(5).andThen([](int v) { return parsePositive(v - 10); });
    CHECK(chained.error().message == "not positive");
    const auto snake = parsePositive(5).and_then([](int v) { return parsePositive(v + 1); });
    CHECK(snake.value() == 6);
    const auto recovered = parsePositive(-3).orElse([](const Error&) { return Result<int>(1); });
    CHECK(recovered.value() == 1);
    const auto toVoid = parsePositive(2).map([](int) {});
    CHECK(toVoid.ok());
    const auto fromVoid = check(true).map([] { return 9; });
    CHECK(fromVoid.value() == 9);
    const auto voidChain = check(false).andThen([] { return parsePositive(1); });
    CHECK(voidChain.error().code == ErrorCode::Io);
    std::string moved = Result<std::string>("abc").map([](std::string&& v) { return v + "d"; }).value();
    CHECK(moved == "abcd");
}

TEST_CASE("result: error codes and toString", "[unit][result]") {
    CHECK(esses::errorCodeName(ErrorCode::Ok) == "Ok");
    CHECK(esses::errorCodeName(ErrorCode::OutOfBudget) == "OutOfBudget");
    CHECK(static_cast<int>(ErrorCode::Internal) == 11); // mirrors esses_status 0..11 (D-051)
    ErrorContext ctx;
    ctx.path = std::filesystem::path("logs") / "bathurst_r1.ld";
    ctx.offset = 0x1F3A;
    const Error e{ErrorCode::Format, "unexpected channel header", ctx};
    CHECK(e.toString() == "Format: unexpected channel header at 0x1F3A (bathurst_r1.ld)");
    ErrorContext more;
    more.channel = "Ground Speed";
    more.detail = "unit.family";
    more.span = esses::TextSpan{3, 7};
    CHECK(Error{ErrorCode::Invalid, "bad unit", more}.toString() ==
          "Invalid: bad unit (channel 'Ground Speed'; span [3,7); unit.family)");
    std::ostringstream os;
    os << Error{ErrorCode::Cancelled, "stop", {}};
    CHECK(os.str() == "Cancelled: stop");
}
