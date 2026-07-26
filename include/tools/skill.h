// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_TOOLS_SKILL_H_
#define UAGENT_INCLUDE_TOOLS_SKILL_H_
// The skill tool. Its schema is the catalogue: one enum entry and one line of
// description per skill, which is all the model needs to choose. Calling it
// returns that skill's procedure, so bodies cost context only when used.

#include <string>
#include <utility>
#include <vector>

#include "include/core/json.h"
#include "include/core/skills.h"
#include "include/core/strings.h"
#include "include/tools/tool.h"

namespace uagent {

inline Tool SkillTool(std::vector<Skill> skills) {
  json names = json::array();
  std::string catalogue;
  for (const Skill& skill : skills) {
    names.push_back(skill.name);
    catalogue += "\n" + skill.name + ": " + skill.description;
  }
  json parameters = {
      {"type", "object"},
      {"properties",
       {{"name",
         {{"type", "string"},
          {"enum", std::move(names)},
          {"description", "which skill to open. Available:" + catalogue}}}}},
      {"required", json::array({"name"})}};
  Tool t = MakeTool(
      "skill",
      "Open a skill: a stored procedure for a task this project or user has "
      "written down. Read it before improvising when one matches.",
      std::move(parameters),
      [skills = std::move(skills)](const json& a, const ToolContext&) {
        std::string want = JsonValue(a, "name", "");
        for (const Skill& skill : skills) {
          if (skill.name == want) return ReadSkillBody(skill);
        }
        return "error: no such skill: " + TerminalSafe(want);
      });
  // Reading a procedure changes nothing; whatever the skill then asks for goes
  // through the tools that do require approval.
  t.parallel_safe = true;
  t.summary = [](const json& a) { return JsonValue(a, "name", ""); };
  return t;
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_TOOLS_SKILL_H_
