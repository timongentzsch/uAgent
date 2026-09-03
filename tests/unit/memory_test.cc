// Copyright 2026 Timon Gentzsch

#include "include/tools/memory.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "tests/unit/test_support.h"

namespace uagent {
namespace {

void WriteGlobal(const std::filesystem::path& dir, const std::string& name,
                 size_t bytes) {
  std::ofstream out(dir / (name + ".md"));
  out << std::string(bytes, 'x') << "\n";
}

}  // namespace

// The always-on slice is the only memory every request pays for, so which
// entries it admits is worth pinning. Nothing covered these loaders before.
void TestMemoryAlwaysOnSelection() {
  namespace fs = std::filesystem;
  const char* prior_home = getenv("HOME");
  const std::string saved_home = prior_home ? prior_home : "";
  fs::path root = fs::temp_directory_path() / "uagent-memory-test";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::path globals = root / ".uagent" / "memory" / "global";
  fs::create_directories(globals, ec);
  setenv("HOME", root.c_str(), 1);

  WriteGlobal(globals, "small_a", 40);
  WriteGlobal(globals, "small_b", 40);
  WriteGlobal(globals, "enormous", 4000);

  // The largest entry is also the newest, so an mtime order and a size order
  // disagree. A consolidation pass or an editor save rewrites mtimes; it must
  // not decide which standing preferences reach the model.
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

  // Ordering is content-derived, so touching a file changes nothing.
  fs::last_write_time(globals / "small_a.md", now + std::chrono::hours(1), ec);
  MemoryIndex touched = LoadAlwaysOnMemory(root, 400);
  CHECK(touched.text == always.text);

  // The index carries every memory regardless, so what the slice drops is
  // still reachable -- and each line says enough to judge whether it is worth
  // fetching, which a bare key does not.
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
  // A hook never costs the index its budget: the cap still binds.
  MemoryIndex tight = LoadMemoryIndex(root, 40);
  CHECK(tight.truncated);
  CHECK(tight.text.size() <= 40);

  if (prior_home) {
    setenv("HOME", saved_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  fs::remove_all(root, ec);
}

}  // namespace uagent
