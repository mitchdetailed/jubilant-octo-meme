// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// caseFold is the single case-folding primitive (02 section 1 behaviour 4): every "case-insensitive" lookup
// means equality of caseFold results (channel names D-013, unit symbols, maths channel references).
//
// Specified: NFKC normalisation, then full Unicode case folding.
// Implemented ("NFKC-lite", no ICU dependency):
//   * Compatibility mappings for the characters that occur in channel and unit names: fullwidth ASCII
//     forms, the Unicode space characters (to U+0020), superscript and subscript digits and signs, the
//     micro sign, the Kelvin, Ohm and Angstrom signs, the degree-Celsius and degree-Fahrenheit signs, Latin
//     ligatures (ff, fi, fl, ffi, ffl, st), vulgar fractions, ellipsis, the trade-mark and numero signs, the
//     Latin compatibility digraphs (IJ, L-middle-dot, DZ/LJ/NJ forms) and long s.
//   * Full case folding (CaseFolding.txt status C + F) for Basic Latin, Latin-1, Latin Extended-A, the
//     regular pairs of Latin Extended-B, Latin Extended Additional, Greek and Coptic, Cyrillic (with the
//     supplement), Armenian and Georgian; sharp s folds to "ss", capital I with dot above to "i" + U+0307,
//     final sigma to sigma.
//   * Not implemented: canonical composition or decomposition of combining sequences (input is assumed to
//     be NFC, which is what virtually all producers write), Greek Extended, Cherokee, Deseret, the case
//     pairs of other scripts, enclosed and Roman-numeral forms. Such characters pass through unchanged.
//   * Invalid UTF-8 is replaced by U+FFFD before folding.
#pragma once

#include <esses/base/config.hpp>

#include <string>
#include <string_view>

namespace esses {

std::string caseFold(std::string_view utf8);
bool ciEquals(std::string_view a, std::string_view b);

} // namespace esses
