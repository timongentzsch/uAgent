// Copyright 2026 Timon Gentzsch

#include "include/core/math.h"

#include <algorithm>
#include <string>

#include "tests/unit/test_support.h"

namespace uagent {

TEST_CASE(TestMathTransliteration) {
  CHECK(MathCommand("Phi") == "Φ");
  CHECK(MathCommand("Pi") == "Π");
  CHECK(MathCommand("nosuchcommand").empty());

  CHECK(TransliterateMath("\\alpha + \\beta") == "α + β");
  CHECK(TransliterateMath("x \\to \\infty") == "x → ∞");
  CHECK(TransliterateMath("\\frac{a}{b}") == "(a)⁄(b)");
  CHECK(TransliterateMath("\\sqrt{2}") == "√(2)");
  CHECK(TransliterateMath("x^2") == "x²");
  CHECK(TransliterateMath("a_1") == "a₁");
  CHECK(TransliterateMath("x^{10}") == "x¹⁰");
  CHECK(TransliterateMath("x^{ab}") == "x^(ab)");
  // \quad is a wide space: it widens the gap instead of collapsing into it.
  CHECK(TransliterateMath("a \\quad b") == "a  b");
  CHECK(TransliterateMath("\\left(x\\right)") == "(x)");
  CHECK(TransliterateMath("\\unknown") == "\\unknown");
  CHECK(TransliterateMath("50\\% \\$") == "50% $");
  CHECK(TransliterateMath("").empty());
}

}  // namespace uagent
