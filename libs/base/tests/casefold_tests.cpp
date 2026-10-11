// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// caseFold: the NFKC-lite + full case folding documented in casefold.hpp. Non-ASCII literals are written as
// UTF-8 escapes so the test does not depend on the source encoding.
#include <esses/base/casefold.hpp>

#include <catch2/catch_test_macros.hpp>

using esses::caseFold;
using esses::ciEquals;

TEST_CASE("casefold: ASCII", "[unit][casefold]") {
    CHECK(caseFold("Ground Speed") == "ground speed");
    CHECK(caseFold("KPH") == "kph");
    CHECK(caseFold("") == "");
    CHECK(caseFold("already_lower 123 !@#") == "already_lower 123 !@#");
    CHECK(ciEquals("Engine RPM", "engine rpm"));
    CHECK_FALSE(ciEquals("Engine RPM", "engine rpm "));
    CHECK_FALSE(ciEquals("a", "b"));
}

TEST_CASE("casefold: full folding of Latin, Greek, Cyrillic and Armenian", "[unit][casefold]") {
    CHECK(caseFold("Stra\xC3\x9F"
                   "e") == "strasse"); // sharp s
    CHECK(ciEquals("STRASSE", "Stra\xC3\x9F"
                              "e"));
    CHECK(ciEquals("\xE1\xBA\x9E", "ss"));                                             // capital sharp s
    CHECK(caseFold("\xC3\x84\xC3\x96\xC3\x9C") == "\xC3\xA4\xC3\xB6\xC3\xBC");         // AE OE UE umlauts
    CHECK(caseFold("\xC5\x81\xC3\xB3\x64\xC5\xBA") == "\xC5\x82\xC3\xB3\x64\xC5\xBA"); // Lodz with stroke
    CHECK(caseFold("\xC5\xB8") == "\xC3\xBF");                                         // Y with diaeresis
    CHECK(caseFold("\xC4\xB0") == "i\xCC\x87");                                        // I with dot above
    // Greek: final sigma folds to sigma, tonos letters fold to their lower-case forms.
    CHECK(ciEquals("\xCE\xA3\xCE\x8A\xCE\xA3\xCE\xA5\xCE\xA6\xCE\x9F\xCE\xA3",
                   "\xCF\x83\xCE\xAF\xCF\x83\xCF\x85\xCF\x86\xCE\xBF\xCF\x82"));
    CHECK(caseFold("\xCE\x94") == "\xCE\xB4"); // Delta
    // Cyrillic, including the U+0400 block offset.
    CHECK(caseFold("\xD0\x9C\xD0\x9E\xD0\xA1\xD0\x9A\xD0\x92\xD0\x90") ==
          "\xD0\xBC\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0");
    CHECK(caseFold("\xD0\x81") == "\xD1\x91");         // Io
    CHECK(caseFold("\xD3\x90") == "\xD3\x91");         // A with breve (pair range)
    CHECK(caseFold("\xD4\xB1") == "\xD5\xA1");         // Armenian Ayb
    CHECK(caseFold("\xE1\xB8\x80") == "\xE1\xB8\x81"); // Latin Extended Additional
}

TEST_CASE("casefold: NFKC-lite compatibility mappings", "[unit][casefold]") {
    CHECK(ciEquals("\xC2\xB5s", "\xCE\xBCs"));                      // micro sign == Greek mu
    CHECK(ciEquals("\xC2\xB5s", "\xCE\x9CS"));                      // ... and capital Mu
    CHECK(ciEquals("\xEF\xBC\xA1\xEF\xBC\xA2\xEF\xBC\xA3", "abc")); // fullwidth ABC
    CHECK(ciEquals("\xEF\xAC\x81le", "FILE"));                      // fi ligature
    CHECK(ciEquals("\xE2\x84\x83", "\xC2\xB0"
                                   "c"));                       // degree Celsius sign
    CHECK(ciEquals("\xE2\x84\xAA", "k"));                       // Kelvin sign
    CHECK(ciEquals("\xE2\x84\xA6", "\xCF\x89"));                // Ohm sign == omega
    CHECK(ciEquals("m\xC2\xB2", "m2"));                         // superscript two
    CHECK(ciEquals("H\xE2\x82\x82O", "h2o"));                   // subscript two
    CHECK(ciEquals("Brake\xC2\xA0Pressure", "brake pressure")); // no-break space
    CHECK(ciEquals("a\xE3\x80\x80"
                   "b",
                   "A B"));                             // ideographic space
    CHECK(ciEquals("\xC7\x85", "dz\xCC\x8C") == false); // composed z-caron is kept composed
    CHECK(ciEquals("\xC7\x85", "d\xC5\xBE"));           // Dz-caron digraph decomposes
    CHECK(ciEquals("\xC5\xBF", "S"));                   // long s
    CHECK(ciEquals("\xE2\x80\xA6", "..."));             // ellipsis
    CHECK(ciEquals("\xC2\xBD", "1\xE2\x81\x84"
                               "2")); // one half
}

TEST_CASE("casefold: invalid UTF-8 and unhandled scripts", "[unit][casefold]") {
    CHECK(caseFold("A\xFF"
                   "B") == "a\xEF\xBF\xBD"
                           "b");                                               // invalid byte becomes U+FFFD
    CHECK(caseFold("\xE6\x97\xA5\xE6\x9C\xAC") == "\xE6\x97\xA5\xE6\x9C\xAC"); // CJK passes through
    CHECK(caseFold(caseFold("Stra\xC3\x9F"
                            "e GROSS")) == caseFold("Stra\xC3\x9F"
                                                    "e GROSS")); // idempotent here
}
