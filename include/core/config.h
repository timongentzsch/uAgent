// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_CONFIG_H_
#define UAGENT_INCLUDE_CORE_CONFIG_H_
// $NAME references in setting values, and folder trust.

#include <map>
#include <set>
#include <string>

#include "include/core/json.h"
#include "include/core/limits.h"

namespace uagent {

using EnvValues = std::map<std::string, std::string>;

// `key`'s value with $NAME and ${NAME} resolved and $$ read as $. An unknown
// name resolves to nothing, or with `keep_unknown` stays as written.
std::string ResolveEnvValue(const std::string& key, const EnvValues& values,
                            std::set<std::string>& resolving,
                            bool process_fallback = true,
                            bool keep_unknown = false);

std::string ExpandProcessEnv(const std::string& value);

bool AgentConfigKey(const std::string& key);

bool ProjectMcpPresent();

bool ProjectMcpSnapshot(json& snapshot, std::string& error);

// Trust covers the servers .mcp.json spawns. It is stored parsed, so a
// reformat keeps trust while any value change revokes it.
bool ProjectTrustSnapshot(json& snapshot, std::string& error);

inline constexpr char kTrustStoreFile[] = "trusted-projects.json";
inline constexpr size_t kTrustStoreBytes = MiB(16);
inline constexpr int kTrustRecordFormat = 3;

std::string TrustStorePath();

// Records one folder under the store's cross-process lock.
bool WriteTrustRecord(const std::string& root, json record, std::string& error);

json ReadTrustStore();

// A record of another format never matches, so that folder is asked again.
bool TrustRecordMatches(const json& record, const json& snapshot);

// `trusted_mcp` receives the approved .mcp.json: registering from it means a
// file swap after approval cannot change which commands are spawned.
bool ProjectConfigTrusted(json* trusted_mcp = nullptr);

bool TrustProjectConfig(std::string& error, json* trusted_mcp = nullptr);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_CONFIG_H_
