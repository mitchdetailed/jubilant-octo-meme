// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/store/types.hpp>

namespace esses::store {

std::string_view dtypeName(Dtype d) noexcept {
    switch (d) {
    case Dtype::F32:
        return "F32";
    case Dtype::F64:
        return "F64";
    case Dtype::I32:
        return "I32";
    case Dtype::I64:
        return "I64";
    }
    return "?";
}

std::string_view validityName(Validity v) noexcept {
    switch (v) {
    case Validity::AllValid:
        return "AllValid";
    case Validity::Bitmap:
        return "Bitmap";
    case Validity::AllInvalid:
        return "AllInvalid";
    }
    return "?";
}

std::string_view interpName(Interp i) noexcept {
    switch (i) {
    case Interp::Linear:
        return "Linear";
    case Interp::Closest:
        return "Closest";
    case Interp::Previous:
        return "Previous";
    case Interp::Next:
        return "Next";
    }
    return "?";
}

std::string_view sampleKindName(SampleKind k) noexcept {
    switch (k) {
    case SampleKind::Real:
        return "Real";
    case SampleKind::Integer:
        return "Integer";
    case SampleKind::Enum:
        return "Enum";
    case SampleKind::Bool:
        return "Bool";
    case SampleKind::Bitfield:
        return "Bitfield";
    case SampleKind::Text:
        return "Text";
    }
    return "?";
}

} // namespace esses::store
