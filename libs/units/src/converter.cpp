// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
#include <esses/units/converter.hpp>

#include <span>

namespace esses::units {

void Converter::applyInPlace(std::span<float> values) const noexcept {
    if (identity()) {
        return;
    }
    for (float& v : values) {
        v = (*this)(v);
    }
}

void Converter::applyInPlace(std::span<double> values) const noexcept {
    if (identity()) {
        return;
    }
    for (double& v : values) {
        v = (*this)(v);
    }
}

Converter Converter::inverse() const noexcept {
    return Converter{1.0 / a, -b / a};
}

Converter Converter::then(const Converter& next) const noexcept {
    return Converter{next.a * a, next.a * b + next.b};
}

} // namespace esses::units
