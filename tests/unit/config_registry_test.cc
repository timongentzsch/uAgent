// Copyright 2026 Timon Gentzsch

#include "include/core/config_registry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "include/app/options.h"
#include "include/app/reference.h"
#include "include/app/self_description.h"
#include "include/cli.h"
#include "include/core/env.h"
#include "include/core/strings.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {

// Every runtime getter must read its default from the registry. Pairing the
// getter with its setting name here is what makes a silent drift impossible:
// changing one without the other fails this test.
struct GetterCheck {
  const char* environment;
  int64_t (*getter)();
};

constexpr GetterCheck kIntGetters[] = {
    {"UAGENT_TOOL_RESULT_CHARS", ToolResultCap},
    {"UAGENT_TOOL_TRACE_PROTECT_CHARS", ToolTraceProtectChars},
    {"UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS", ToolTracePruneMinChars},
    {"UAGENT_AUTO_COMPACT_PCT", AutoCompactPct},
    {"UAGENT_AUTO_COMPACT_TOKENS", AutoCompactTokens},
    {"UAGENT_TOOL_CONCURRENCY", ToolConcurrency},
    {"UAGENT_DEPTH", AgentDepth},
    {"UAGENT_SUBAGENT_MAX_STEPS", SubagentMaxSteps},
    {"UAGENT_SUBAGENT_MAX_TOOL_CALLS", SubagentMaxToolCalls},
    {"UAGENT_SUBAGENT_CALLS_PER_TURN", SubagentCallsPerTurn},
    {"UAGENT_MAX_TOKENS", MaxOutputTokens},
    {"UAGENT_READ_FILE_LINES", ReadFileLines},
    {"UAGENT_READ_FILE_MAX_LINES", ReadFileMaxLines},
    {"UAGENT_READ_FILE_BYTES", ReadFileBytes},
    {"UAGENT_EDIT_FILE_BYTES", EditFileBytes},
    {"UAGENT_LIST_DIR_ENTRIES", ListDirEntries},
    {"UAGENT_LIST_DIR_SCAN_ENTRIES", ListDirScanEntries},
    {"UAGENT_MEMORY_BYTES", MemoryBytes},
    {"UAGENT_MEMORY_FILES", MaxMemories},
    {"UAGENT_MEMORY_IDLE_SECONDS", MemoryIdleSeconds},
    {"UAGENT_MEMORY_EXTRACT_BYTES", MemoryExtractBytes},
    {"UAGENT_SKILL_BYTES", SkillBodyBytes},
    {"UAGENT_SKILL_DESC_BYTES", SkillDescriptionBytes},
    {"UAGENT_SKILLS", MaxSkills},
    {"UAGENT_GREP_RESULTS", GrepResults},
    {"UAGENT_BASH_LOG_BYTES", BashLogBytes},
    {"UAGENT_RUN_YIELD_MS", RunDefaultYieldMs},
    {"UAGENT_MAX_BACKGROUND_JOBS", MaxBackgroundJobs},
    {"UAGENT_MCP_CONFIG_BYTES", McpConfigBytes},
    {"UAGENT_MCP_DESC_CHARS", McpDescriptionChars},
    {"UAGENT_PENDING_ATTACHMENTS", MaxPendingAttachments},
    {"UAGENT_ATTACHMENT_MB", AttachmentLimitMb},
    {"UAGENT_TERMINAL_IMAGE_MB", TerminalImageLimitMb},
    {"UAGENT_IMAGE_MAX_COLUMNS", ImageMaxColumns},
    {"UAGENT_CONTEXT", ContextWindow},
    {"UAGENT_HISTORY_DAYS", HistoryDays},
    {"UAGENT_HISTORY_FILES", HistoryFiles},
    {"UAGENT_DEBUG_DAYS", DebugDays},
    {"UAGENT_DEBUG_FILES", DebugFiles},
    {"UAGENT_BG_DAYS", BgDays},
    {"UAGENT_BG_FILES", BgFiles},
    {"UAGENT_MCP_LOG_DAYS", McpLogDays},
    {"UAGENT_MCP_LOG_FILES", McpLogFiles},
    {"UAGENT_TERMINAL_DAYS", TerminalRecordDays},
};

std::vector<std::string> DirectRuntimeSettingLookups(std::string_view source) {
  constexpr std::string_view kFunctions[] = {
      "EnvStr", "EnvLong", "EnvDouble", "EnvBool", "getenv", "ReadStringArray"};
  std::vector<std::string> names;
  for (std::string_view function : kFunctions) {
    size_t offset = 0;
    while ((offset = source.find(function, offset)) != std::string_view::npos) {
      size_t cursor = offset + function.size();
      while (cursor < source.size() &&
             std::isspace(static_cast<unsigned char>(source[cursor]))) {
        ++cursor;
      }
      if (cursor >= source.size() || source[cursor] != '(') {
        offset += function.size();
        continue;
      }
      ++cursor;
      while (cursor < source.size() &&
             std::isspace(static_cast<unsigned char>(source[cursor]))) {
        ++cursor;
      }
      if (cursor >= source.size() || source[cursor] != '"') {
        offset += function.size();
        continue;
      }
      ++cursor;
      if (!source.substr(cursor).starts_with("UAGENT_")) {
        offset += function.size();
        continue;
      }
      size_t end = source.find('"', cursor);
      if (end == std::string_view::npos) break;
      names.emplace_back(source.substr(cursor, end - cursor));
      offset = end + 1;
    }
  }
  return names;
}

}  // namespace

void TestConfigRegistryContract() {
  // Uniqueness: a duplicated name would make lookup order decide behaviour.
  std::set<std::string_view> names;
  for (const ConfigDescriptor& descriptor : ConfigRegistry()) {
    CHECK(!descriptor.environment.empty());
    CHECK(names.insert(descriptor.environment).second);
    CHECK(!descriptor.category.empty());
    CHECK(!descriptor.description.empty());
    if (descriptor.type == ConfigType::kInt) {
      CHECK(descriptor.minimum <= descriptor.maximum);
      const int64_t* value = std::get_if<int64_t>(&descriptor.default_value);
      // A fixed default must itself be inside the bounds it advertises.
      CHECK(!value ||
            (*value >= descriptor.minimum && *value <= descriptor.maximum));
    }
  }

  // A RuntimeConfig-backed setting must name its field, and a reloadable one
  // must be RuntimeConfig-backed: nothing else is re-read at a turn boundary.
  for (const ConfigDescriptor& descriptor : ConfigRegistry()) {
    if (descriptor.reload == ReloadPolicy::kNextUserTurn) {
      CHECK(!descriptor.field.empty());
    }
    if (!descriptor.field.empty()) {
      CHECK(RuntimeConfigField(descriptor.environment) == descriptor.field);
    }
  }

  // Every getter returns exactly the registered default when unset.
  for (const GetterCheck& check : kIntGetters) {
    const ConfigDescriptor* descriptor =
        FindConfigDescriptor(check.environment);
    CHECK(descriptor != nullptr);
    if (!descriptor) continue;
    ScopedEnv cleared(check.environment);
    const int64_t* declared = std::get_if<int64_t>(&descriptor->default_value);
    CHECK(declared != nullptr);
    if (declared) CHECK(check.getter() == *declared);
  }

  // Bounds are enforced, not merely documented.
  ScopedEnv concurrency("UAGENT_TOOL_CONCURRENCY", "100000");
  CHECK(ToolConcurrency() ==
        FindConfigDescriptor("UAGENT_TOOL_CONCURRENCY")->maximum);
  ScopedEnv results("UAGENT_GREP_RESULTS", "0");
  CHECK(GrepResults() == FindConfigDescriptor("UAGENT_GREP_RESULTS")->minimum);

  // Secrets are declared, and every declared secret is redacted by the
  // diagnostic that feeds /context, /debug-config and uagent_info.
  ScopedEnv search_key("UAGENT_WEB_SEARCH_API_KEY", "canary-secret-value");
  json diagnostic = RuntimeConfig::FromEnvironment().DiagnosticJson();
  CHECK(JsonDump(diagnostic).find("canary-secret-value") == std::string::npos);
  CHECK(JsonValue(diagnostic, "web_search_api_key", "") == "<set>");

  // Direct runtime lookups are occasionally necessary at bootstrap, but they
  // still belong to the registry unless their name explicitly marks
  // process-internal plumbing. Scan the production source so a new bypass
  // fails here instead of silently escaping diagnostics and generated docs.
  const std::filesystem::path source_root = UAGENT_TEST_SOURCE_DIR;
  size_t matched_lookups = 0;
  size_t internal_lookups = 0;
  for (const char* directory : {"src", "include"}) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             source_root / directory)) {
      if (!entry.is_regular_file()) continue;
      std::string extension = entry.path().extension().string();
      if (extension != ".cc" && extension != ".h") continue;
      std::ifstream input(entry.path());
      std::string source((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
      for (const std::string& environment :
           DirectRuntimeSettingLookups(source)) {
        ++matched_lookups;
        if (environment.starts_with("UAGENT_INTERNAL_")) {
          ++internal_lookups;
          continue;
        }
        const ConfigDescriptor* descriptor = FindConfigDescriptor(environment);
        if (!descriptor) {
          std::cerr << "unregistered runtime setting " << environment << " in "
                    << entry.path() << '\n';
        }
        CHECK(descriptor != nullptr);
      }
    }
  }
  CHECK(matched_lookups >= 20);
  CHECK(internal_lookups > 0);
}

// The bug this pins: before strict parsing, every spelling except "0" read as
// true, so UAGENT_MEMORY=false switched memory *on*.
void TestStrictBooleanSettings() {
  bool value = false;
  for (const char* spelling : {"1", "true", "TRUE", "Yes", "on"}) {
    value = false;
    CHECK(ParseBool(spelling, value));
    CHECK(value);
  }
  for (const char* spelling : {"0", "false", "FALSE", "No", "off"}) {
    value = true;
    CHECK(ParseBool(spelling, value));
    CHECK(!value);
  }
  // An unreadable spelling reports failure and leaves the caller's default in
  // place — the same contract ParseInt64 and ParseFiniteDouble keep.
  for (const char* spelling : {"", "2", "maybe", "0x0", "true story"}) {
    value = true;
    CHECK(!ParseBool(spelling, value));
    CHECK(value);
    value = false;
    CHECK(!ParseBool(spelling, value));
    CHECK(!value);
  }

  // Environment path, through the registry default (UAGENT_MEMORY is true).
  const ConfigDescriptor* memory = FindConfigDescriptor("UAGENT_MEMORY");
  CHECK(memory != nullptr);
  if (!memory) return;
  {
    ScopedEnv unset("UAGENT_MEMORY");
    CHECK(BoolSetting(*memory));
  }
  for (const char* spelling : {"0", "false", "no", "off"}) {
    ScopedEnv disabled("UAGENT_MEMORY", spelling);
    CHECK(!BoolSetting(*memory));
  }
  for (const char* spelling : {"garbage", "2", ""}) {
    ScopedEnv confused("UAGENT_MEMORY", spelling);
    CHECK(BoolSetting(*memory));  // falls back to the registered default
  }

  // Config-file path: the same spellings through RuntimeConfig::FromValues.
  CHECK(!RuntimeConfig::FromValues({{"UAGENT_MEMORY", "off"}}).memory_enabled);
  CHECK(RuntimeConfig::FromValues({{"UAGENT_MEMORY", "ON"}}).memory_enabled);
  CHECK(RuntimeConfig::FromValues({{"UAGENT_MEMORY", "wat"}}).memory_enabled);
  CHECK(RuntimeConfig::FromValues({}).memory_enabled);
}

void TestSelfDescriptionSchemas() {
  json settings = ConfigSchemaJson();
  CHECK(settings.size() == ConfigRegistry().size());
  for (const json& setting : settings) {
    CHECK(!JsonValue(setting, "name", "").empty());
    CHECK(!JsonValue(setting, "takes_effect", "").empty());
    // A secret's value never appears in the schema; only its metadata does.
    CHECK(!setting.contains("active"));
  }

  // Flag and command registries drive both parsing and help, so every
  // documented entry must round-trip through the parser.
  json flags = CliSchemaJson();
  CHECK(!flags.empty());
  for (const json& flag : flags) {
    std::string name = JsonValue(flag, "flag", "");
    CHECK(!name.empty());
    bool known = false;
    for (const FlagSpec& spec : FlagRegistry()) known |= spec.flag == name;
    CHECK(known);
  }
  std::set<std::string_view> flag_names;
  for (const FlagSpec& spec : FlagRegistry()) {
    CHECK(flag_names.insert(spec.flag).second);
  }

  json commands = CommandSchemaJson();
  CHECK(!commands.empty());
  for (const json& command : commands) {
    std::string name = JsonValue(command, "command", "");
    ParsedSlashCommand parsed = ParseSlashCommand(name);
    CHECK(parsed.spec != nullptr);
  }
  std::set<std::string_view> command_names;
  for (const SlashCommandSpec& command : SlashCommandRegistry()) {
    CHECK(command_names.insert(command.name).second);
  }

  // The generated references are deterministic: the same registries must
  // produce byte-identical output, which is what lets CI diff them.
  std::vector<ReferenceFile> first = ReferenceFiles();
  std::vector<ReferenceFile> second = ReferenceFiles();
  CHECK(first.size() == second.size());
  for (size_t index = 0; index < first.size(); ++index) {
    CHECK(first[index].name == second[index].name);
    CHECK(first[index].contents == second[index].contents);
  }
  CHECK(ReferenceManifest().find(kVersion) != std::string::npos);
}

}  // namespace uagent
