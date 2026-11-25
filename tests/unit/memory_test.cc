// Copyright 2026 Timon Gentzsch

#include "include/tools/memory.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "include/core/config.h"
#include "tests/unit/test_support.h"

namespace uagent {
namespace {

void WriteGlobal(const std::filesystem::path& dir, const std::string& name,
                 size_t bytes) {
  std::ofstream out(dir / (name + ".md"));
  out << std::string(bytes, 'x') << "\n";
}
}  // namespace

TEST_CASE(TestForgottenMemoryCutoff) {
  namespace fs = std::filesystem;
  TestWorkspace fixture("memory-forget");
  const fs::path source = fixture.root / "conversation";
  std::ofstream(source) << "synthetic evidence";
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(source, now - std::chrono::hours(1));
  auto save = [&](const fs::path& workspace, const std::string& key,
                  const std::string& folded = "") {
    ObservedMemories observed;
    LoadMemoryIndex(workspace, 4096, {}, &observed);
    if (!folded.empty()) observed.at(folded).whole = true;
    MemoryEvent outcome;
    CHECK(SaveExtractedMemory(key, "Updated rule", folded, workspace, source,
                              observed, outcome)
              .Ok());
    return outcome.action;
  };
  const auto& workspace = fixture.workspace;
  CHECK(ToolMemoryAction("set", "project/first", "Initial rule").Ok());
  const auto first =
      MemoryControl({{"action", "get"}, {"key", "project/first"}}, workspace);
  REQUIRE(first.contains("item"));
  CHECK(!MemoryControl({{"action", "rename"},
                        {"key", "project/first"},
                        {"target", "project/renamed"},
                        {"revision", first["item"]["revision"]}},
                       workspace)
             .contains("error"));
  CHECK(save(workspace, "project/after-rename") == "created");
  CHECK(save(workspace, "project/consolidated", "project/renamed") ==
        "updated");
  CHECK(save(workspace, "project/after-merge") == "created");
  CHECK(ToolMemoryAction("forget", "project/consolidated", std::nullopt).Ok());
  fs::remove(fixture.home / ".uagent/memory/events.jsonl");
  // Rewording a key cannot resurrect an old fact even after journal rotation.
  CHECK(save(workspace, "project/alias") == "no_change");
  CHECK(save(workspace, "project/consolidated") == "no_change");
  CHECK(save(workspace, "project/after-merge") == "unchanged");
  CHECK(
      ToolMemoryAction("set", "project/consolidated", "Human correction").Ok());
  CHECK(save(workspace, "project/consolidated") == "no_change");
  CHECK(save(workspace, "project/after-merge", "project/consolidated") ==
        "no_change");
  CHECK(save(workspace, "global/unrelated") == "created");
  const auto other = fixture.root / "other";
  fs::create_directory(other);
  CHECK(save(other, "project/alias") == "created");
  fs::last_write_time(source, now + std::chrono::hours(1));
  CHECK(save(workspace, "project/alias") == "created");
  CHECK(save(workspace, "project/consolidated") == "updated");
}

TEST_CASE(TestMemoryAlwaysOnSelection) {
  namespace fs = std::filesystem;
  TestWorkspace workspace("memory-test");
  const fs::path root = workspace.home;
  std::error_code ec;
  fs::path globals = root / ".uagent" / "memory" / "global";
  fs::create_directories(globals);

  WriteGlobal(globals, "small_a", 40);
  WriteGlobal(globals, "small_b", 40);
  WriteGlobal(globals, "enormous", 4000);

  // The largest entry is also the newest, so an mtime order and a size order
  // disagree.
  auto now = fs::file_time_type::clock::now();
  fs::last_write_time(globals / "enormous.md", now, ec);
  fs::last_write_time(globals / "small_a.md", now - std::chrono::hours(48), ec);
  fs::last_write_time(globals / "small_b.md", now - std::chrono::hours(24), ec);

  MemoryIndex always = LoadAlwaysOnMemory(root, 400);
  CHECK(always.text.find("global/small_a") != std::string::npos);
  CHECK(always.text.find("global/small_b") != std::string::npos);
  CHECK(always.text.find("global/enormous") == std::string::npos);
  // Skipped rather than cut in half.
  CHECK(always.truncated);
  CHECK(always.text.size() <= 400);

  fs::last_write_time(globals / "small_a.md", now + std::chrono::hours(1), ec);
  MemoryIndex touched = LoadAlwaysOnMemory(root, 400);
  CHECK(touched.text == always.text);

  // The index lists what the slice drops, each with its opening clause.
  {
    std::ofstream out(globals / "hooked.md");
    out << "Prefer the measured number. Everything after this is elaboration "
           "that the index has no room for.\n";
  }
  MemoryIndex index = LoadMemoryIndex(root, 4096);
  CHECK(index.text.find("global/enormous") != std::string::npos);
  CHECK(index.text.find("- global/hooked: Prefer the measured number\n") !=
        std::string::npos);
  CHECK(index.text.find("elaboration") == std::string::npos);
  CHECK(!index.truncated);
  MemoryIndex tight = LoadMemoryIndex(root, 40);
  CHECK(tight.truncated);
  CHECK(tight.text.size() <= 40);
}

// The agent and the Library read one store; the 32-per-scope limit governs
// writes only.
TEST_CASE(TestMemoryOneView) {
  namespace fs = std::filesystem;
  TestWorkspace workspace("memory-one-view");
  const fs::path root = workspace.home;
  fs::path globals = root / ".uagent" / "memory" / "global";
  fs::create_directories(globals);

  for (int i = 0; i < 36; ++i) WriteGlobal(globals, "m" + std::to_string(i), 8);
  WriteGlobal(globals, "zz-oversized", 2600);
  size_t listed = 0;
  for (const MemoryEntry& memory : ListMemories(root)) {
    listed += memory.key.starts_with("global/");
  }
  CHECK(listed == 37);
  CHECK(ToolMemoryAction("list", "", std::nullopt)
            .output.find("global/zz-oversized") != std::string::npos);
  CHECK(ToolMemoryAction("set", "global/one-more", std::string("x")).error ==
        ToolErrorCode::kLimitExceeded);

  ToolResult read =
      ToolMemoryAction("get", "global/zz-oversized", std::nullopt);
  CHECK(read.Ok());
  CHECK(read.output.find("[memory truncated; set it again within the limit]") !=
        std::string::npos);
  CHECK(ToolMemoryAction("set", "global/zz-oversized", std::string("short"))
            .Ok());
  CHECK(ToolMemoryAction("forget", "global/zz-oversized", std::nullopt).Ok());
}
}  // namespace uagent
