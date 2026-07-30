// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SKILL_H_
#define UAGENT_INCLUDE_TOOLS_SKILL_H_
// Deferred skill discovery. The fixed schema stays cheap as the catalogue
// grows; a short query opens a sole match directly and lists ambiguous matches.

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

inline Tool SkillTool(std::vector<Skill> skills) {
  json parameters = {
      {"type", "object"},
      {"properties",
       {{"name", {{"type", "string"}, {"description", "exact skill name"}}},
        {"query",
         {{"type", "string"},
          {"description",
           "short topic; opens a sole match or lists matches"}}}}},
      {"additionalProperties", false}};
  Tool t = MakeTool(
      "skill", "Find or open a stored procedure before improvising.",
      std::move(parameters),
      [skills = std::move(skills)](const json& a,
                                   const ToolContext&) -> ToolResult {
        std::string want = JsonValue(a, "name", "");
        if (!want.empty()) {
          for (const Skill& skill : skills) {
            if (skill.name == want) return OpenSkill(skill);
          }
          return ToolFailure(ToolErrorCode::kNotFound,
                             "error: no such skill: " + TerminalSafe(want));
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
          return ToolFailure(ToolErrorCode::kNotFound,
                             "error: no skill matches: " + TerminalSafe(query));
        }
        return ToolSuccess(std::move(catalogue));
      });
  // Reading a procedure changes nothing; whatever the skill then asks for goes
  // through the tools that do require approval.
  t.parallel_safe = true;
  t.summary = [](const json& a) {
    std::string name = JsonValue(a, "name", "");
    return name.empty() ? JsonValue(a, "query", "list") : name;
  };
  return t;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SKILL_H_
