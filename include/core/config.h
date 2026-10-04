// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_CONFIG_H_
#define UAGENT_INCLUDE_CORE_CONFIG_H_
// The KEY=value text format (.env files, and the config files of earlier
// versions) and workspace trust.

#include <istream>
#include <map>
#include <set>
#include <string>

#include "include/core/json.h"
#include "include/core/limits.h"

namespace uagent {

using EnvValues = std::map<std::string, std::string>;

// Parse without mutating the process. Only agent-owned keys are exported later;
// other entries remain available as interpolation sources without leaking every
// config value into child processes.
EnvValues ParseEnvValues(std::istream& input);

EnvValues ParseEnvValues(const std::string& text);

EnvValues ReadEnvValues(const std::string& path);

std::string ResolveEnvValue(const std::string& key, const EnvValues& values,
                            std::set<std::string>& resolving,
                            bool process_fallback = true);

std::string ExpandProcessEnv(const std::string& value);

bool AgentConfigKey(const std::string& key);

bool ProjectMcpPresent();

bool ProjectMcpSnapshot(json& snapshot, std::string& error);

// Trust covers what the workspace can change: the servers .mcp.json spawns.
// It is stored parsed, so a reformat keeps trust while any value change
// revokes it.
bool ProjectTrustSnapshot(json& snapshot, std::string& error);

inline constexpr char kTrustStoreFile[] = "trusted-projects.json";
inline constexpr size_t kTrustStoreBytes = size_t{16} * 1024 * 1024;

std::string TrustStorePath();

// Records one workspace under the store's cross-process lock, so trusting two
// workspaces at once cannot drop either record.
bool WriteTrustRecord(const std::string& root, json record, std::string& error);

json ReadTrustStore();

// A record of another format simply stops matching, so that workspace is
// asked once more. One written by an earlier version also holds the project
// config that was approved with it, which the settings import reads.
bool TrustRecordMatches(const json& record, const json& snapshot);

// The out-parameter carries the approved .mcp.json alone: MCP registration
// consumes it directly, so a file swap after approval cannot change which
// commands are spawned.
bool ProjectConfigTrusted(json* trusted_mcp = nullptr);

bool TrustProjectConfig(std::string& error, json* trusted_mcp = nullptr);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_CONFIG_H_
