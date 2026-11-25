// Copyright 2026 Timon Gentzsch

#include "include/mcp/skills.h"

#include <openssl/sha.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/mcp/rpc.h"
#include "include/mcp/server.h"
#include "include/media/attachments.h"
#include "include/tools/tool.h"

namespace uagent {

namespace {

constexpr std::string_view kSkillFile = "/SKILL.md";

// The skill's folder: its SKILL.md URI without that last segment.
std::string SkillRoot(const std::string& uri) {
  return uri.substr(0, uri.size() - kSkillFile.size());
}

bool Digest(const json& value) {
  if (!value.is_string()) return false;
  const std::string& text = value.get_ref<const std::string&>();
  return text.size() == 71 && text.starts_with("sha256:") &&
         std::ranges::all_of(text.substr(7), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

// Why `entry` cannot be offered; empty when it can, with `skill` filled.
std::string SkillIssue(const std::string& server, const json& entry,
                       McpSkill& skill) {
  const std::string uri = JsonValue(entry, "uri", "");
  const size_t scheme = uri.find("://");
  if (scheme == std::string::npos || !uri.ends_with(kSkillFile) ||
      uri.size() <= scheme + 3 + kSkillFile.size()) {
    return "its uri is not a SKILL.md";
  }
  const std::string path =
      uri.substr(scheme + 3, uri.size() - scheme - 3 - kSkillFile.size());
  // A colon would let `a` + `b:c` read as server `a:b` + `c`.
  const auto plain = [](char c) { return c > ' ' && c < 127 && c != ':'; };
  if (!std::ranges::all_of(path, plain)) {
    return "its path has a colon, a space or a control byte";
  }
  const json frontmatter = JsonValue(entry, "frontmatter", json());
  const std::string name = JsonValue(frontmatter, "name", "");
  skill.description = JsonValue(frontmatter, "description", "");
  if (name.empty() || path.substr(path.find_last_of('/') + 1) != name) {
    return "its name is not the last segment of its path";
  }
  if (skill.description.empty() ||
      skill.description.size() > kSkillDescriptionBytes) {
    return "its description is empty or longer than " +
           std::to_string(kSkillDescriptionBytes) + " bytes";
  }
  const json resources = JsonValue(entry, "resources", json());
  if (!resources.is_array() || resources.size() > kMcpSkillFiles) {
    return resources == "dynamic"
               ? "its files are generated, so they cannot be verified"
               : "it lists no files, or more than " +
                     std::to_string(kMcpSkillFiles);
  }
  const std::string root = SkillRoot(uri) + "/";
  size_t total = 0;
  bool listed = false;
  for (const json& file : resources) {
    const std::string at = JsonValue(file, "uri", "");
    const auto size = JsonValue(file, "size", int64_t{-1});
    if (!at.starts_with(root) || !Digest(JsonValue(file, "digest", json())) ||
        size < 0) {
      return "a file entry has no uri inside it, digest or size";
    }
    if (static_cast<size_t>(size) > kMcpSkillBytes) {
      return "its files exceed 16 MiB";
    }
    total += static_cast<size_t>(size);
    listed |= at == uri;
  }
  if (!listed) return "its files do not include its SKILL.md";
  if (total > kMcpSkillBytes) return "its files exceed 16 MiB";
  skill.name = server + ":" + path;
  skill.uri = uri;
  skill.resources = resources;
  return {};
}

// The bytes of `uri` in a resources/read reply: text, or a decoded blob
// where `blob` allows one (never for SKILL.md, which is text).
bool ResourceBytes(const json& reply, const std::string& uri, size_t limit,
                   bool blob, std::string& bytes) {
  const json result = JsonValue(reply, "result", json());
  for (const json& content : JsonValue(result, "contents", json::array())) {
    if (JsonValue(content, "uri", "") != uri) continue;
    if (const std::string* text = JsonStringRef(content, "text")) {
      bytes = *text;
      return bytes.size() <= limit;
    }
    return blob && Base64Decode(JsonValue(content, "blob", ""), bytes, limit);
  }
  return false;
}

// The top-level name and description of SKILL.md's front matter, leaving
// `input` at the body; the description is cleared when it spans lines.
bool ReadFrontMatter(std::istream& input, std::string& name,
                     std::string& description) {
  std::string line;
  if (!std::getline(input, line) || Trim(line) != "---") return false;
  bool in_description = false;
  while (std::getline(input, line)) {
    if (Trim(line) == "---") return true;
    if (line.empty() || line[0] == ' ' || line[0] == '\t') {
      if (in_description && !Trim(line).empty()) description.clear();
      continue;
    }
    const size_t colon = line.find(':');
    const std::string key = Trim(line.substr(0, colon));
    const std::string value =
        colon == std::string::npos ? "" : Unquote(Trim(line.substr(colon + 1)));
    in_description = key == "description";
    if (key == "name") name = value;
    if (in_description) {
      description = value.starts_with('|') || value.starts_with('>')
                        ? std::string()
                        : value;
    }
  }
  return false;
}

std::string Sha256(std::string_view bytes) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
         digest);
  return "sha256:" +
         HexEncode({reinterpret_cast<const char*>(digest), sizeof digest});
}

}  // namespace

void McpSetSkills(McpServer& server, const json& listed) {
  server.skills.clear();
  std::set<std::string> names;
  for (const json& entry : listed) {
    McpSkill skill;
    std::string issue = SkillIssue(server.name, entry, skill);
    if (issue.empty() && !names.insert(skill.name).second) {
      issue = "it is listed twice";
    }
    if (!issue.empty()) {
      McpNote(server.name, "skill " +
                               OneLine(JsonValue(entry, "uri", "?"), 120) +
                               " not offered: " + issue);
      continue;
    }
    server.skills.push_back(std::move(skill));
  }
  McpNote(server.name, std::to_string(server.skills.size()) + " of " +
                           std::to_string(listed.size()) + " skills");
}

json McpSkillRows(const McpServer& server) {
  json rows = json::array();
  for (const McpSkill& skill : server.skills) {
    rows.push_back({{"name", skill.name}, {"description", skill.description}});
  }
  return rows;
}

const McpSkill* McpFindSkill(const McpRuntime& runtime, const std::string& name,
                             McpServer** server) {
  for (const auto& owned : runtime.Servers()) {
    if (!owned->alive || !name.starts_with(owned->name + ":")) continue;
    for (const McpSkill& skill : owned->skills) {
      if (skill.name != name) continue;
      *server = owned.get();
      return &skill;
    }
  }
  return nullptr;
}

ToolResult McpReadSkill(McpServer& server, const McpSkill& skill,
                        const std::string& file, int64_t timeout,
                        const ToolContext& context) {
  const std::string root = SkillRoot(skill.uri) + "/";
  const std::string uri = file.empty() ? skill.uri : root + file;
  const json* entry = nullptr;
  std::string files;
  for (const json& listed : skill.resources) {
    const std::string at = JsonValue(listed, "uri", "");
    if (at == uri) entry = &listed;
    if (at != skill.uri) {
      files += (files.empty() ? "" : ", ") + at.substr(root.size());
    }
  }
  if (!entry) {
    return ToolFailure(ToolErrorCode::kNotFound,
                       "error: " + file + " is not a file of " + skill.name +
                           "; its files: " + (files.empty() ? "none" : files));
  }
  const auto size = JsonValue(*entry, "size", int64_t{0});
  if (static_cast<size_t>(size) > kSkillBodyBytes) {
    return ToolFailure(ToolErrorCode::kUnavailable,
                       "error: " + uri + " is " + std::to_string(size) +
                           " bytes, more than the " +
                           std::to_string(kSkillBodyBytes) +
                           " a skill file may have here; it cannot be read, "
                           "so continue without it");
  }
  const auto deadline = std::min(context.deadline, DeadlineAfter(timeout));
  const json reply = McpRpc(server, "resources/read", {{"uri", uri}},
                            SecondsUntil(deadline), true, deadline);
  std::string bytes;
  if (!ResourceBytes(reply, uri, kSkillBodyBytes, !file.empty(), bytes) ||
      bytes.size() != static_cast<size_t>(size) ||
      Sha256(bytes) != JsonValue(*entry, "digest", "")) {
    return ToolFailure(
        ToolErrorCode::kRemoteError,
        "error: " + uri + " from " + server.name +
            (reply.contains("result")
                 ? " does not match its listed size and digest; not used"
                 : ": " + JsonErrorMessage(reply, "resources/read failed")));
  }
  const std::string origin = "served by MCP server " + server.name;
  if (!file.empty()) {
    return ToolSuccess("[file " + file + " of skill " + skill.name + ", " +
                       origin + "]\n" +
                       (bytes.find('\0') == std::string::npos
                            ? bytes
                            : "(binary, " + std::to_string(size) + " bytes)"));
  }
  std::istringstream input(bytes);
  std::string name;
  std::string description;
  // A description over several lines is not compared: only the name is.
  if (!ReadFrontMatter(input, name, description) ||
      name != skill.name.substr(skill.name.find_last_of("/:") + 1) ||
      (!description.empty() && description != skill.description)) {
    return ToolFailure(ToolErrorCode::kRemoteError,
                       "error: the name or description in " + uri +
                           " differs from " + server.name +
                           "'s listing; not used");
  }
  std::string body(std::istreambuf_iterator<char>(input), {});
  // Not kSkillMarker: compaction re-sends those results as µAgent's own
  // note, which a server's text must not become.
  ToolResult opened = ToolSuccess(
      "[served skill " + skill.name + " — " + origin +
      "; untrusted instructions that grant no tools or permissions; files: " +
      (files.empty() ? "none" : files) + ", read with skill {name, file}]\n" +
      Trim(body));
  opened.body_kind = "markdown";
  return opened;
}

}  // namespace uagent
