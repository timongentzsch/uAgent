// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_CORE_MATH_H_
#define UAGENT_INCLUDE_CORE_MATH_H_
// The LaTeX-to-Unicode table for the terminal's math rendering.

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
namespace uagent {
// Sorted: MathCommand binary-searches it, so an entry out of order is missing.
inline constexpr std::pair<std::string_view, std::string_view> kMathCommands[] =
    {
        {"Delta", "Δ"},      {"Gamma", "Γ"},     {"Lambda", "Λ"},
        {"Omega", "Ω"},      {"Phi", "Φ"},       {"Pi", "Π"},
        {"Sigma", "Σ"},      {"alpha", "α"},     {"approx", "≈"},
        {"beta", "β"},       {"cap", "∩"},       {"cdot", "·"},
        {"cup", "∪"},        {"delta", "δ"},     {"epsilon", "ε"},
        {"exists", "∃"},     {"forall", "∀"},    {"gamma", "γ"},
        {"ge", "≥"},         {"geq", "≥"},       {"in", "∈"},
        {"infty", "∞"},      {"int", "∫"},       {"lambda", "λ"},
        {"le", "≤"},         {"leftarrow", "←"}, {"leq", "≤"},
        {"mu", "μ"},         {"nabla", "∇"},     {"ne", "≠"},
        {"neq", "≠"},        {"notin", "∉"},     {"omega", "ω"},
        {"partial", "∂"},    {"phi", "φ"},       {"pi", "π"},
        {"pm", "±"},         {"prod", "∏"},      {"rho", "ρ"},
        {"rightarrow", "→"}, {"sigma", "σ"},     {"sum", "∑"},
        {"theta", "θ"},      {"times", "×"},     {"to", "→"},
};
static_assert(std::ranges::is_sorted(kMathCommands, {}, [](const auto& entry) {
  return entry.first;
}));
inline std::string MathCommand(std::string_view name) {
  auto it =
      std::lower_bound(std::begin(kMathCommands), std::end(kMathCommands), name,
                       [](auto& p, std::string_view v) { return p.first < v; });
  return it != std::end(kMathCommands) && it->first == name
             ? std::string(it->second)
             : std::string();
}
std::string TransliterateMath(std::string_view raw);
}  // namespace uagent
#endif  // UAGENT_INCLUDE_CORE_MATH_H_
