// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/byte_reader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

using esses::ByteReader;
using esses::Endian;

namespace {
std::vector<std::byte> make(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}
} // namespace

TEST_CASE("byte reader: little- and big-endian reads", "[unit][bytereader]") {
    const auto data =
        make({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x00, 0x00, 0x80, 0x3F, 0xFF, 0xFE});
    ByteReader r(data);
    CHECK(r.u16() == 0x0201u);
    CHECK(r.u16be() == 0x0304u);
    CHECK(r.u32le() == 0x08070605u);
    CHECK(r.f32() == 1.0f);
    CHECK(r.i16() == -257); // 0xFEFF little-endian
    CHECK(r.remaining() == 0u);
    CHECK(r.ok());
    CHECK(r.status().ok());

    ByteReader be(data, Endian::Big);
    CHECK(be.u32() == 0x01020304u);
    CHECK(be.u64le() == 0x3F80000008070605ull); // explicit little-endian read on a big-endian reader
    CHECK(be.peekAt<std::uint16_t>(0) == 0x0102u);
    CHECK(esses::loadAs<std::uint32_t>(data.data(), Endian::Little) == 0x04030201u);
    CHECK(esses::loadAs<std::uint32_t>(data.data(), Endian::Big) == 0x01020304u);
    CHECK(esses::loadAs<double>(make({0, 0, 0, 0, 0, 0, 0xF0, 0x3F}).data(), Endian::Little) == 1.0);
}

TEST_CASE("byte reader: overruns set a sticky error with the first offset", "[unit][bytereader]") {
    const auto data = make({1, 2, 3});
    ByteReader r(data);
    CHECK(r.u16() == 0x0201u);
    CHECK(r.u32() == 0u); // only one byte left
    CHECK_FALSE(r.ok());
    CHECK(r.u8() == 0u); // sticky
    const esses::Result<void> st = r.status();
    REQUIRE_FALSE(st.ok());
    CHECK(st.error().code == esses::ErrorCode::Format);
    CHECK(st.error().context.offset == std::optional<std::uint64_t>(2));

    ByteReader s(data);
    s.seek(4);
    CHECK_FALSE(s.ok());
    ByteReader t(data);
    t.seek(3);
    CHECK(t.ok());
    CHECK(t.remaining() == 0u);
    CHECK(t.bytes(1).empty());
    CHECK_FALSE(t.ok());
}

TEST_CASE("byte reader: fixed-width strings stop at the first NUL", "[unit][bytereader]") {
    const auto data = make({'S', 'p', 'e', 'e', 'd', 0, 'x', 'y', 'A', 'B'});
    ByteReader r(data);
    CHECK(r.fixedString(8) == "Speed");
    CHECK(r.pos() == 8u);
    CHECK(r.fixedString(2) == "AB");
    CHECK(r.fixedString(1).empty());
    CHECK_FALSE(r.ok());
}
