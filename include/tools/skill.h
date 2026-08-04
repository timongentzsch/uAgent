// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SKILL_H_
#define UAGENT_INCLUDE_TOOLS_SKILL_H_
// Progressive skill disclosure. Names and bounded descriptions are advertised
// with the tool; the full body arrives only after the model selects one.

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/skills.h"
#include "include/core/strings.h"
#include "include/tools/tool.h"

namespace uagent {

inline bool SkillMatches(const Skill& skill, std::string_view query) {
  std::string needle = AsciiLower(Trim(std::string(query)));
  if (needle.empty()) return true;
  return AsciiLower(skill.name).find(needle) != std::string::npos ||
         AsciiLower(skill.description).find(needle) != std::string::npos;
}

inline ToolResult OpenSkill(const Skill& skill) {
  SkillReadResult result = ReadSkillBody(skill);
  return result.ok
             ? ToolSuccess(std::move(result.output))
             : ToolFailure(ToolErrorCode::kInternal, std::move(result.output));
}

inline std::string SkillCatalogue(const std::vector<Skill>& skills,
                                  std::string_view query) {
  constexpr size_t kCatalogueChars = 8 * 1024;
  constexpr std::string_view kMore = "\n[more matches; narrow query]";
  bool descriptions = !Trim(std::string(query)).empty();
  std::string out;
  for (const Skill& skill : skills) {
    if (!SkillMatches(skill, query)) continue;
    std::string line =
        skill.name +
        (descriptions ? ": " + OneLine(skill.description, 96) : "");
    if (!out.empty()) line.insert(0, "\n");
    if (out.size() + line.size() + kMore.size() > kCatalogueChars) {
      out += kMore;
      break;
    }
    out += line;
  }
  return out;
}

inline std::string SkillToolDescription(const std::vector<Skill>& skills) {
  // Codex caps its initial skill catalogue at 8,000 characters. Keep the same
  // hard ceiling here while retaining every name whenever normal filesystem
  // limits allow it; descriptions are the first thing shortened.
  constexpr size_t kMaxChars = 8000;
  constexpr std::string_view kIntro =
      "Load an installed skill when the user's task matches its description. "
      "This catalogue contains installed skills only; full instructions load "
      "after selection.\n\nAvailable skills:\n";
  constexpr std::string_view kMore =
      "- [more installed skills; call with {} to list names]";
  std::string out(kIntro);
  size_t fixed = out.size();
  for (const Skill& skill : skills) fixed += 5 + skill.name.size();
  size_t description_bytes =
      fixed < kMaxChars && !skills.empty()
          ? std::min(static_cast<size_t>(SkillDescriptionBytes()),
                     (kMaxChars - fixed) / skills.size())
          : 0;
  for (size_t index = 0; index < skills.size(); ++index) {
    const Skill& skill = skills[index];
    std::string line = "- " + skill.name;
    if (description_bytes > 0) {
      std::string description =
          Utf8Prefix(OneLine(skill.description), description_bytes);
      if (!description.empty()) line += ": " + description;
    }
    line += '\n';
    size_t suffix = index + 1 < skills.size() ? kMore.size() : 0;
    if (out.size() + line.size() + suffix > kMaxChars) {
      out += kMore;
      break;
    }
    out += line;
  }
  return out;
}

inline std::string SkillNameSuggestion(const std::vector<Skill>& skills,
                                       std::string want) {
  want = AsciiLower(std::move(want));
  std::replace(want.begin(), want.end(), '_', '-');
  for (const Skill& skill : skills) {
    if (AsciiLower(skill.name) == want) return skill.name;
  }
  return "";
}

inline Tool SkillTool(std::vector<Skill> skills) {
  std::string description = SkillToolDescription(skills);
  json parameters = {
      {"type", "object"},
      {"properties",
       {{"name",
         {{"type", "string"},
          {"description", "exact installed skill name listed above"}}},
        {"query",
         {{"type", "string"},
          {"description",
           "optional installed-catalogue filter; {} lists all names"}}}}},
      {"additionalProperties", false}};
  Tool t = MakeTool(
      "skill", std::move(description), std::move(parameters),
      [skills = std::move(skills)](const json& a,
                                   const ToolContext&) -> ToolResult {
        std::string want = JsonValue(a, "name", "");
        if (!want.empty()) {
          for (const Skill& skill : skills) {
            if (skill.name == want) return OpenSkill(skill);
          }
          std::string suggestion = SkillNameSuggestion(skills, want);
          std::string error =
              "error: no such installed skill: " + TerminalSafe(want);
          if (!suggestion.empty()) {
            error += "; did you mean " + TerminalSafe(suggestion) + "?";
          }
          error += "; call skill with {} to list installed names";
          return ToolFailure(ToolErrorCode::kNotFound, std::move(error));
        }

        std::string query = JsonValue(a, "query", "");
        const Skill* match = nullptr;
        size_t matches = 0;
        for (const Skill& skill : skills) {
          if (!SkillMatches(skill, query)) continue;
          match = &skill;
          ++matches;
        }
        if (matches == 1) return OpenSkill(*match);
        std::string catalogue = SkillCatalogue(skills, query);
        if (catalogue.empty()) {
          return ToolFailure(
              ToolErrorCode::kNotFound,
              "error: no installed skill matches: " + TerminalSafe(query) +
                  "; call skill with {} to list installed "
                  "names");
        }
        return ToolSuccess(std::move(catalogue));
      });
  // Reading a procedure changes nothing; whatever the skill then asks for goes
  // through the tools that do require approval.
  t.parallel_safe = true;
  t.capabilities = Capability(ToolCapability::kInspect);
  t.retain_output = true;
  t.result_chars = SkillBodyBytes() + 1024;
  t.summary = [](const json& a) {
    std::string name = JsonValue(a, "name", "");
    return name.empty() ? JsonValue(a, "query", "list") : name;
  };
  return t;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SKILL_H_
