// Copyright 2026 Timon Gentzsch

#include <cstdint>
#include <limits>
#include <string>

#include "include/tools/memory.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestSafeJsonValues() {
  json values = {{"boolean", "true"},
                 {"integer", "12"},
                 {"negative", -1},
                 {"unsigned", uint64_t{7}},
                 {"overflow", std::numeric_limits<uint64_t>::max()},
                 {"number", json::array()},
                 {"string", 7},
                 {"object", "wrong"}};
  CHECK(!JsonValue(values, "boolean", false));
  CHECK(JsonValue(values, "integer", int64_t{9}) == 9);
  CHECK(JsonValue(values, "negative", uint32_t{5}) == 5);
  CHECK(JsonValue(values, "unsigned", uint32_t{0}) == 7);
  CHECK(JsonValue(values, "overflow", int64_t{5}) == 5);
  CHECK(JsonValue(values, "number", 1.5) == 1.5);
  CHECK(JsonValue(values, "string", "fallback") == "fallback");
  CHECK(JsonValue(values, "object", json::object()).is_string());

  Usage usage;
  usage.Add({{"prompt_tokens", "bad"},
             {"completion_tokens", json::array()},
             {"cost", "bad"}});
  CHECK(usage.input == 0);
  CHECK(usage.output == 0);
  CHECK(usage.cost == 0);
}

void TestProjectInstructionDiscovery() {
  namespace fs = std::filesystem;
  fs::path root = fs::temp_directory_path() /
                  ("uagent-instructions-test-" +
                   std::to_string(static_cast<int64_t>(getpid())));
  fs::path home = root / "home";
  fs::path repo = root / "repo";
  fs::path child = repo / "child";
  fs::path empty = child / "empty";
  fs::create_directories(home / ".uagent");
  fs::create_directories(home / ".uagent/memory/global");
  fs::create_directories(repo / ".git");
  fs::create_directories(empty);
  CHECK(ToolWriteFile((home / ".uagent/AGENTS.md").string(), "global")
            .output.starts_with("wrote "));
  CHECK(ToolWriteFile((home / ".uagent/memory/global/lesson.md").string(),
                      "remembered-evidence")
            .output.starts_with("wrote "));
  CHECK(ToolWriteFile((repo / "AGENTS.md").string(), "root-agent")
            .output.starts_with("wrote "));
  CHECK(ToolWriteFile((repo / "CLAUDE.md").string(), "root-claude")
            .output.starts_with("wrote "));
  CHECK(ToolWriteFile((child / "AGENTS.md").string(), "ignored-agent")
            .output.starts_with("wrote "));
  CHECK(ToolWriteFile((child / "AGENTS.override.md").string(), "child-override")
            .output.starts_with("wrote "));
  CHECK(ToolWriteFile((child / "CLAUDE.md").string(), "child-claude")
            .output.starts_with("wrote "));
  CHECK(ToolWriteFile((empty / "AGENTS.override.md").string(), " \n")
            .output.starts_with("wrote "));
  CHECK(ToolWriteFile((empty / "AGENTS.md").string(), "must-not-load")
            .output.starts_with("wrote "));

  ScopedEnv scoped_home("HOME", home.c_str());

  ProjectInstructions loaded =
      LoadProjectInstructions(child, size_t{32} * 1024);
  MemoryIndex memories =
      LoadMemoryIndex(child, size_t{32} * 1024 - loaded.text.size());
  loaded.memory_index = memories.text;
  loaded.memory_sources = memories.sources;
  loaded.truncated |= memories.truncated;
  MemoryIndex always = LoadAlwaysOnMemory(child, 64);
  CHECK(loaded.sources.size() == 3);  // one file per directory
  CHECK(loaded.memory_sources.size() == 1);
  CHECK(!loaded.truncated);
  CHECK(always.sources.size() == 1);
  CHECK(!always.truncated);
  CHECK(always.text.find("# global/lesson") != std::string::npos);
  CHECK(always.text.find("remembered-evidence") != std::string::npos);
  MemoryIndex capped_memory = LoadAlwaysOnMemory(child, 24);
  CHECK(capped_memory.truncated);
  CHECK(capped_memory.text.size() <= 24);

  // Under the cap the smaller global memory wins -- more standing preferences
  // fit that way -- and an entry that no longer fits is dropped whole rather
  // than cut mid-sentence. Made the newest as well as the largest, so an mtime
  // order would reach the opposite answer.
  CHECK(ToolWriteFile((home / ".uagent/memory/global/newer.md").string(),
                      std::string(200, 'n'))
            .output.starts_with("wrote "));
  fs::last_write_time(
      home / ".uagent/memory/global/lesson.md",
      fs::file_time_type::clock::now() - std::chrono::hours(24));
  MemoryIndex smallest_first = LoadAlwaysOnMemory(child, 64);
  CHECK(smallest_first.truncated);
  CHECK(smallest_first.sources.size() == 1);
  CHECK(smallest_first.text.find("remembered-evidence") != std::string::npos);
  CHECK(smallest_first.text.find(std::string(200, 'n')) == std::string::npos);
  CHECK(fs::remove(home / ".uagent/memory/global/newer.md"));
  CHECK(loaded.text.find("ignored-agent") == std::string::npos);
  // CLAUDE.md is a fallback, so it must not load beside an AGENTS file
  CHECK(loaded.text.find("root-claude") == std::string::npos);
  CHECK(loaded.text.find("child-claude") == std::string::npos);
  size_t global = loaded.text.find("global");
  size_t root_agent = loaded.text.find("root-agent");
  size_t child_override = loaded.text.find("child-override");
  CHECK(global < root_agent && root_agent < child_override);
  CHECK(loaded.text.find("remembered-evidence") == std::string::npos);
  CHECK(loaded.memory_index.find("global/lesson") != std::string::npos);
  // The index carries a global's opening clause, never a project memory's.
  CHECK(loaded.memory_index.find("global/lesson: remembered-evidence") !=
        std::string::npos);

  // ...but it is used when the directory has no AGENTS file
  fs::path only_claude = repo / "claude-only";
  fs::create_directories(only_claude);
  CHECK(ToolWriteFile((only_claude / "CLAUDE.md").string(), "claude-fallback")
            .output.starts_with("wrote "));
  CHECK(LoadProjectInstructions(only_claude, size_t{32} * 1024)
            .text.find("claude-fallback") != std::string::npos);

  ProjectInstructions shadowed =
      LoadProjectInstructions(empty, size_t{32} * 1024);
  CHECK(shadowed.text.find("must-not-load") == std::string::npos);
  ProjectInstructions capped = LoadProjectInstructions(child, 4);
  CHECK(capped.truncated);
  CHECK(capped.text == "glob");

  std::error_code ec;
  fs::remove_all(root, ec);
}

}  // namespace uagent
