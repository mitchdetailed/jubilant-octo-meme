// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/uuid.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <unordered_set>

using esses::Uuid;

TEST_CASE("uuid: canonical text round-trips", "[unit][uuid]") {
    const std::string text = "0123abcd-4567-89ef-0a1b-2c3d4e5f6789";
    const Uuid u = Uuid::parse(text);
    CHECK(u.str() == text);
    CHECK(Uuid::parse("{0123ABCD-4567-89EF-0A1B-2C3D4E5F6789}") == u);
    CHECK_FALSE(u.isNil());
    CHECK(Uuid{}.isNil());
    CHECK(Uuid{}.str() == "00000000-0000-0000-0000-000000000000");
}

TEST_CASE("uuid: malformed text", "[unit][uuid]") {
    for (const char* bad : {"", "0123abcd45674-89ef-0a1b-2c3d4e5f6789", "0123abcd-4567-89ef-0a1b-2c3d4e5f678",
                            "0123abcd-4567-89ef-0a1b-2c3d4e5f678g", "{0123abcd-4567-89ef-0a1b-2c3d4e5f6789",
                            "0123abcd-4567-89ef-0a1b_2c3d4e5f6789"}) {
        INFO(bad);
        CHECK_FALSE(Uuid::tryParse(bad).has_value());
        CHECK(Uuid::parse(bad).isNil());
    }
}

TEST_CASE("uuid: deterministic ids from Hash128 carry version and variant bits", "[unit][uuid]") {
    const esses::Hash128 h = esses::hash128({"source", "Normal", "Ground Speed"});
    const Uuid a = esses::uuidFromHash(h);
    const Uuid b = esses::uuidFromHash(h);
    CHECK(a == b);
    CHECK(a.version() == 5);
    CHECK((a.b[8] & 0xC0u) == 0x80u);
    CHECK(esses::uuidFromHash(esses::Hash128{0, 0}).str() == "00000000-0000-5000-8000-000000000000");
    CHECK(esses::uuidFromHash(esses::Hash128{0xffffffffffffffffull, 0xffffffffffffffffull}, 8).str() ==
          "ffffffff-ffff-8fff-bfff-ffffffffffff");
}

TEST_CASE("uuid: random v4 ids are distinct", "[unit][uuid]") {
    std::unordered_set<Uuid> seen;
    for (int i = 0; i < 1000; ++i) {
        const Uuid u = esses::uuidRandom();
        CHECK(u.version() == 4);
        CHECK((u.b[8] & 0xC0u) == 0x80u);
        seen.insert(u);
    }
    CHECK(seen.size() == 1000u);
    std::set<Uuid> ordered(seen.begin(), seen.end()); // operator<=> compiles and orders
    CHECK(ordered.size() == 1000u);
}
