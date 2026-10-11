// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/hash.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

using esses::Hash128;
using esses::hash128;

namespace {
std::vector<std::byte> bytesOf(std::string_view s) {
    std::vector<std::byte> out;
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}
} // namespace

// Known vectors (XXH3-128, seed 0, canonical hex = high64 then low64). The empty-input value is the upstream
// sanity-test vector; the 1..3 byte values were re-derived independently from the published algorithm
// (scratch script, short-input path) and agree with the vendored library.
TEST_CASE("hash128: known XXH3-128 vectors", "[unit][hash]") {
    CHECK(hash128(std::string_view()).hex() == "99aa06d3014798d86001c324468d497f");
    CHECK(hash128(std::string_view()).hi == 0x99AA06D3014798D8ull);
    CHECK(hash128(std::string_view()).lo == 0x6001C324468D497Full);
    CHECK(hash128(std::string_view("a")).hex() == "a96faf705af16834e6c632b61e964e1f");
    CHECK(hash128(std::string_view("ab")).hex() == "89c65ebc828eebaca873719c24d5735c");
    CHECK(hash128(std::string_view("abc")).hex() == "06b05ab6733a618578af5f94892f3950");
}

TEST_CASE("hash128: span, string and stream forms agree", "[unit][hash]") {
    const std::string text =
        "The quick brown fox jumps over the lazy dog, 0123456789 times over and over again.";
    const auto bytes = bytesOf(text);
    const Hash128 a = hash128(std::span<const std::byte>(bytes));
    const Hash128 b = hash128(std::string_view(text));
    CHECK(a == b);
    esses::Hash128Stream s;
    s.update(std::string_view(text).substr(0, 10));
    s.update(std::span<const std::byte>(bytes).subspan(10));
    CHECK(s.digest() == a);
    s.reset();
    s.update(std::string_view(text));
    CHECK(s.digest() == a);
    esses::Hash128Stream moved = std::move(s);
    CHECK(moved.digest() == a);
}

TEST_CASE("hash128: parts are length-prefixed", "[unit][hash]") {
    CHECK(hash128({"ab", "c"}) != hash128({"a", "bc"}));
    CHECK(hash128({"abc"}) != hash128(std::string_view("abc")));
    CHECK(hash128({"", "x"}) != hash128({"x", ""}));
    CHECK(hash128({"source", "Normal", "Ground Speed"}) == hash128({"source", "Normal", "Ground Speed"}));
    esses::Hash128Stream s;
    s.updatePart("ab");
    s.updatePart("c");
    CHECK(s.digest() == hash128({"ab", "c"}));
    // Equivalent explicit form: u64 little-endian length, then the bytes.
    esses::Hash128Stream manual;
    manual.updateU64(2);
    manual.update(std::string_view("ab"));
    manual.updateU64(1);
    manual.update(std::string_view("c"));
    CHECK(manual.digest() == hash128({"ab", "c"}));
}

TEST_CASE("hash128: ordering and std::hash", "[unit][hash]") {
    const Hash128 x{1, 2};
    const Hash128 y{2, 1};
    CHECK(x > y); // hi compares first
    CHECK(Hash128{} == Hash128{0, 0});
    std::unordered_set<Hash128> set{x, y, x};
    CHECK(set.size() == 2u);
    CHECK(Hash128{0x0123456789abcdefull, 0xfedcba9876543210ull}.hex() == "fedcba98765432100123456789abcdef");
}
