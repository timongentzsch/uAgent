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

// Names rather than tools, so the same rule can be applied later by a rescan
// that has no access to the registry.
inline std::vector<std::string> ToolNames(const std::vector<Tool>& tools) {
  std::vector<std::string> names;
  names.reserve(tools.size());
  for (const Tool& tool : tools) names.push_back(tool.name);
  return names;
}

inline void KeepSupportedSkills(std::vector<Skill>& skills,
                                const std::vector<std::string>& tool_names) {
  std::erase_if(skills, [&](const Skill& skill) {
    return std::any_of(skill.required_tools.begin(), skill.required_tools.end(),
                       [&](const std::string& name) {
                         return std::find(tool_names.begin(), tool_names.end(),
                                          name) == tool_names.end();
                       });
  });
}

inline ToolResult OpenSkill(const Skill& skill, std::string arguments = {}) {
  SkillReadResult result = ReadSkillBody(skill, std::move(arguments));
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
      "After opening it, follow it only when its full workflow supports the "
      "requested outcome; otherwise discard it and continue. "
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
    if (!skill.argument_hint.empty()) {
      line += " [args: " + OneLine(skill.argument_hint, 64) + "]";
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

inline Tool SkillTool(std::vector<Skill> skills,
                      std::vector<std::string> tool_names) {
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
           "optional installed-catalogue filter; {} lists all names"}}},
        {"arguments",
         {{"type", "string"},
          {"maxLength", 4096},
          {"description", "optional skill arguments replacing $ARGUMENTS"}}}}},
      {"additionalProperties", false}};
  Tool t = MakeTool(
      "skill", std::move(description), std::move(parameters),
      [skills = std::move(skills), tool_names = std::move(tool_names)](
          const json& a, const ToolContext&) -> ToolResult {
        std::string want = JsonValue(a, "name", "");
        std::string arguments = JsonValue(a, "arguments", "");
        if (!want.empty()) {
          for (const Skill& skill : skills) {
            if (skill.name == want) return OpenSkill(skill, arguments);
          }
          // A skill written during this session is absent from the catalogue
          // the request advertises, which is fixed for the whole process to
          // keep the prompt prefix cacheable. Look again on disk before
          // refusing a name, so authoring a skill and using it does not need
          // a restart.
          std::vector<Skill> current = LoadSkills(CanonicalCwd());
          KeepSupportedSkills(current, tool_names);
          for (const Skill& skill : current) {
            if (skill.name == want) return OpenSkill(skill, arguments);
          }
          // The advertised names are what the model actually saw, so they
          // make the better suggestion when both lists have a near match.
          std::string suggestion = SkillNameSuggestion(skills, want);
          if (suggestion.empty())
            suggestion = SkillNameSuggestion(current, want);
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
        if (matches == 1) return OpenSkill(*match, arguments);
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
