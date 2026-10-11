// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// ByteReader (outline section 1.1): bounds-checked little/big-endian reads over a byte span. Reads past the
// end return 0 (or an empty span/string) and set a sticky error; status() reports the first failing offset as
// fail(Format), so a parser can read a whole header and check once.
#pragma once

#include <esses/base/config.hpp>
#include <esses/base/result.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>

namespace esses {

enum class Endian : std::uint8_t { Little, Big };

namespace detail {
template <class U> constexpr U byteSwap(U v) noexcept {
    static_assert(std::is_unsigned_v<U>);
    if constexpr (sizeof(U) == 1) {
        return v;
    } else {
        U r = 0;
        for (std::size_t i = 0; i < sizeof(U); ++i) {
            r = static_cast<U>((r << 8) | ((v >> (8 * i)) & 0xFFu));
        }
        return r;
    }
}
} // namespace detail

// Loads an arithmetic value of type T stored with endianness e at p (no alignment requirement).
template <class T> T loadAs(const std::byte* p, Endian e) noexcept {
    static_assert(std::is_arithmetic_v<T>);
    using U = std::conditional_t<
        sizeof(T) == 1, std::uint8_t,
        std::conditional_t<sizeof(T) == 2, std::uint16_t,
                           std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
    static_assert(sizeof(U) == sizeof(T));
    U raw = 0;
    std::memcpy(&raw, p, sizeof(U));
    const bool nativeLittle = std::endian::native == std::endian::little;
    if ((e == Endian::Little) != nativeLittle) {
        raw = detail::byteSwap(raw);
    }
    return std::bit_cast<T>(raw);
}

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> input, Endian byteOrder = Endian::Little) noexcept
        : data_(input), endian_(byteOrder) {}

    std::size_t size() const noexcept { return data_.size(); }
    std::size_t pos() const noexcept { return pos_; }
    std::size_t remaining() const noexcept { return pos_ <= data_.size() ? data_.size() - pos_ : 0; }
    bool ok() const noexcept { return !failed_; }
    Endian endian() const noexcept { return endian_; }
    void setEndian(Endian e) noexcept { endian_ = e; }

    void seek(std::size_t offset) noexcept; // beyond the end -> error
    void skip(std::size_t count) noexcept;

    template <class T> T read() noexcept { return readAs<T>(endian_); }
    template <class T> T readAs(Endian e) noexcept {
        if (!take(sizeof(T))) {
            return T{};
        }
        return loadAs<T>(data_.data() + pos_ - sizeof(T), e);
    }
    // Absolute read that does not move the cursor (still sets the error on overrun).
    template <class T> T peekAt(std::size_t offset) noexcept {
        if (offset > data_.size() || data_.size() - offset < sizeof(T)) {
            setError(offset);
            return T{};
        }
        return loadAs<T>(data_.data() + offset, endian_);
    }

    std::uint8_t u8() noexcept { return read<std::uint8_t>(); }
    std::uint16_t u16() noexcept { return read<std::uint16_t>(); }
    std::uint32_t u32() noexcept { return read<std::uint32_t>(); }
    std::uint64_t u64() noexcept { return read<std::uint64_t>(); }
    std::int8_t i8() noexcept { return read<std::int8_t>(); }
    std::int16_t i16() noexcept { return read<std::int16_t>(); }
    std::int32_t i32() noexcept { return read<std::int32_t>(); }
    std::int64_t i64() noexcept { return read<std::int64_t>(); }
    float f32() noexcept { return read<float>(); }
    double f64() noexcept { return read<double>(); }

    std::uint16_t u16le() noexcept { return readAs<std::uint16_t>(Endian::Little); }
    std::uint32_t u32le() noexcept { return readAs<std::uint32_t>(Endian::Little); }
    std::uint64_t u64le() noexcept { return readAs<std::uint64_t>(Endian::Little); }
    std::uint16_t u16be() noexcept { return readAs<std::uint16_t>(Endian::Big); }
    std::uint32_t u32be() noexcept { return readAs<std::uint32_t>(Endian::Big); }
    std::uint64_t u64be() noexcept { return readAs<std::uint64_t>(Endian::Big); }

    std::span<const std::byte> bytes(std::size_t count) noexcept;
    // Reads `count` bytes and returns them up to the first NUL (fixed-width name fields). Bytes are not
    // validated or transcoded.
    std::string fixedString(std::size_t count);

    // ok() or fail(Format, "read past end of buffer", {offset = first failing offset}).
    Result<void> status() const;

private:
    bool take(std::size_t count) noexcept;
    void setError(std::size_t offset) noexcept;

    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    Endian endian_;
    bool failed_ = false;
    std::size_t errorOffset_ = 0;
};

} // namespace esses
