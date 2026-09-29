// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_CONFIG_H_
#define UAGENT_INCLUDE_CORE_CONFIG_H_
// Private configuration and workspace trust. Shell exports beat a trusted
// ./.uagent/.config, which beats ~/.uagent/.config; project .env files are
// application data and are never imported.

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

std::string ResolveEnvValue(const std::string& key,
                            const EnvValues& values,
                            std::set<std::string>& resolving,
                            bool process_fallback = true);

std::string ExpandProcessEnv(const std::string& value);

bool AgentConfigKey(const std::string& key);

bool ProjectMcpPresent();

bool ProjectAgentConfigPresent();

bool ProjectMcpSnapshot(json& snapshot, std::string& error);

// Trust covers exactly what the workspace can change: the servers .mcp.json
// spawns and the settings ./.uagent/.config exports. Both are stored parsed, so
// a reformat keeps trust while any value change revokes it. ReadEnvValues
// returns an ordered map, so the recorded object is stable.
bool ProjectTrustSnapshot(json& snapshot, std::string& error);

inline constexpr char kTrustStoreFile[] = "trusted-projects.json";
inline constexpr size_t kTrustStoreBytes = size_t{16} * 1024 * 1024;

std::string TrustStorePath();

// Records one workspace under the store's cross-process lock, so trusting two
// workspaces at once cannot drop either record.
bool WriteTrustRecord(const std::string& root, json record,
                      std::string& error);

json ReadTrustStore();

// Records written before both surfaces were covered carry an older format and
// simply stop matching, so those workspaces are asked once more.
bool TrustRecordMatches(const json& record, const json& snapshot);

// The out-parameter carries the approved .mcp.json alone: MCP registration
// consumes it directly, so a file swap after approval cannot change which
// commands are spawned.
bool ProjectConfigTrusted(json* trusted_mcp = nullptr);

// Re-record trust after a person approved an exact change to
// ./.uagent/.config. The .mcp.json half is carried over from the existing
// record and re-checked against disk first, so this can never extend trust to
// servers nobody approved. Failing leaves the workspace to be confirmed again,
// which is the safe direction.
bool RestampProjectConfigTrust(std::string& error);

bool TrustProjectConfig(std::string& error,
                        json* trusted_mcp = nullptr);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_CONFIG_H_
