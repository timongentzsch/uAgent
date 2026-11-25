// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MCP_SKILLS_H_
#define UAGENT_INCLUDE_MCP_SKILLS_H_
// Skills a server serves (io.modelcontextprotocol/skills): its listing,
// named per server, and files read only on demand and checked against it.

#include <cstdint>
#include <string>

#include "include/core/json.h"
#include "include/mcp/server.h"
#include "include/tools/tool.h"

namespace uagent {

// Keeps the listed skills whose manifest can be verified; a note says why
// each other one is left out.
void McpSetSkills(McpServer& server, const json& listed);

// What a person is shown of a server's skills: [{name, description}].
json McpSkillRows(const McpServer& server);

// The served skill `<server>:<path>` and its live server, or nulls.
const McpSkill* McpFindSkill(const McpRuntime& runtime, const std::string& name,
                             McpServer** server);

// SKILL.md (empty `file`) or one file the manifest names, relative to the
// skill's folder, read from its server and checked against its size and
// SHA-256; marked with the server it came from.
ToolResult McpReadSkill(McpServer& server, const McpSkill& skill,
                        const std::string& file, int64_t timeout,
                        const ToolContext& context);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MCP_SKILLS_H_
