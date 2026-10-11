// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/base/byte_reader.hpp>

#include <algorithm>

namespace esses {

void ByteReader::setError(std::size_t offset) noexcept {
    if (!failed_) {
        failed_ = true;
        errorOffset_ = offset;
    }
}

bool ByteReader::take(std::size_t count) noexcept {
    if (failed_ || pos_ > data_.size() || data_.size() - pos_ < count) {
        setError(pos_);
        return false;
    }
    pos_ += count;
    return true;
}

void ByteReader::seek(std::size_t offset) noexcept {
    if (offset > data_.size()) {
        setError(offset);
        return;
    }
    pos_ = offset;
}

void ByteReader::skip(std::size_t count) noexcept {
    static_cast<void>(take(count));
}

std::span<const std::byte> ByteReader::bytes(std::size_t count) noexcept {
    if (!take(count)) {
        return {};
    }
    return data_.subspan(pos_ - count, count);
}

std::string ByteReader::fixedString(std::size_t count) {
    const std::span<const std::byte> raw = bytes(count);
    const auto* begin = reinterpret_cast<const char*>(raw.data());
    const auto* nul = std::find(begin, begin + raw.size(), '\0');
    return {begin, nul};
}

Result<void> ByteReader::status() const {
    if (!failed_) {
        return ::esses::ok(); // not ByteReader::ok()
    }
    ErrorContext ctx;
    ctx.offset = errorOffset_;
    return fail(ErrorCode::Format, "read past end of buffer", std::move(ctx));
}

} // namespace esses
