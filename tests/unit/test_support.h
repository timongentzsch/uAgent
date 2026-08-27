// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_TESTS_UNIT_TEST_SUPPORT_H_
#define UAGENT_TESTS_UNIT_TEST_SUPPORT_H_

#include <unistd.h>

#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/app/options.h"
#include "include/app/runtime.h"
#include "include/cli.h"
#include "include/core/child_env.h"
#include "include/core/config.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/project.h"
#include "include/core/skills.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/mcp/register.h"
#include "include/mcp/server.h"
#include "include/media.h"
#include "include/providers.h"
#include "include/tools/files.h"
#include "include/tools/jobs.h"
#include "include/tools/process.h"
#include "include/tools/registry.h"
#include "include/tools/shell.h"
#include "include/tools/skill.h"
#include "include/tools/subagent.h"
#include "include/tools/tool.h"
#include "include/ui/tool_output.h"

namespace uagent {

class TestWorkspace {
 public:
  explicit TestWorkspace(const std::string& name)
      : root(
            std::filesystem::temp_directory_path() /
            ("uagent-" + name + "-test-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()))),
        workspace(root / "workspace"),
        home(root / "home"),
        original_(std::filesystem::current_path()) {
    const char* value = std::getenv("HOME");
    had_home_ = value != nullptr;
    if (value) prior_home_ = value;
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    std::filesystem::current_path(workspace);
    workspace = std::filesystem::current_path();
  }

  ~TestWorkspace() {
    std::error_code ignored;
    std::filesystem::current_path(original_, ignored);
    if (had_home_) {
      setenv("HOME", prior_home_.c_str(), 1);
    } else {
      unsetenv("HOME");
    }
    std::filesystem::remove_all(root, ignored);
  }

  TestWorkspace(const TestWorkspace&) = delete;
  TestWorkspace& operator=(const TestWorkspace&) = delete;

  std::filesystem::path root;
  std::filesystem::path workspace;
  std::filesystem::path home;

 private:
  std::filesystem::path original_;
  std::string prior_home_;
  bool had_home_ = false;
};

// Sets an environment variable immediately and restores the inherited value
// (or its absence) when the scope ends, so a test may read back what it set.
class ScopedEnv {
 public:
  ScopedEnv(const char* key, const char* value) : key_(key) {
    const char* prior = std::getenv(key);
    had_ = prior != nullptr;
    if (prior) prior_ = prior;
    if (value) {
      setenv(key, value, 1);
    } else {
      unsetenv(key);
    }
  }
  ScopedEnv(const char* key, const std::string& value)
      : ScopedEnv(key, value.c_str()) {}
  // Unsets for the scope.
  explicit ScopedEnv(const char* key) : ScopedEnv(key, nullptr) {}

  ~ScopedEnv() {
    if (had_) {
      setenv(key_, prior_.c_str(), 1);
    } else {
      unsetenv(key_);
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  const char* key_;
  std::string prior_;
  bool had_ = false;
};

// Blocks until the activity behind `job` has drained its output, or until the
// timeout expires; returns whether it drained.
inline bool WaitForActivityDrain(
    ProcessSupervisor& supervisor, const BgJob& job,
    std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  if (!job.session) return false;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(job.session->mutex);
      if (job.session->state == ActivityState::kDrained) return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) return false;
    const uint64_t generation = supervisor.Generation();
    supervisor.WaitForChange(generation, deadline);
  }
}

extern int failures;
void Check(bool condition, const char* expression, const char* file, int line);

template <typename Writer>
std::string CaptureStdout(Writer&& writer, bool color = false,
                          bool tty = true) {
  fflush(stdout);
  int saved = dup(STDOUT_FILENO);
  FILE* capture = tmpfile();
  if (saved < 0 || !capture) {
    if (saved >= 0) close(saved);
    if (capture) fclose(capture);
    return {};
  }
  dup2(fileno(capture), STDOUT_FILENO);
  bool prior_tty = g_tty;
  bool prior_color = g_color;
  g_tty = tty;
  g_color = color;
  std::forward<Writer>(writer)();
  fflush(stdout);
  g_tty = prior_tty;
  g_color = prior_color;
  dup2(saved, STDOUT_FILENO);
  close(saved);
  fseek(capture, 0, SEEK_END);
  const int64_t bytes = static_cast<int64_t>(ftell(capture));
  fseek(capture, 0, SEEK_SET);
  std::string output(bytes > 0 ? static_cast<size_t>(bytes) : 0, '\0');
  if (!output.empty()) {
    (void)fread(output.data(), 1, output.size(), capture);
  }
  fclose(capture);
  return output;
}

// The suite in run order: declarations and dispatch expand from this list.
#define UAGENT_TESTS(X)                   \
  X(TestTextToolProtocol)                 \
  X(TestToolResults)                      \
  X(TestRegistries)                       \
  X(TestCommandAndDisplayRegistries)      \
  X(TestModelCatalogParsing)              \
  X(TestOptions)                          \
  X(TestMarkdownBlankLines)               \
  X(TestTableRetroErasesRenderedRows)     \
  X(TestInteractiveTranscriptFraming)     \
  X(TestMarkdownMath)                     \
  X(TestCapsAndEscaping)                  \
  X(TestFileTools)                        \
  X(TestMathTransliteration)              \
  X(TestActivityBar)                      \
  X(TestPollCollapse)                     \
  X(TestTerminalSafety)                   \
  X(TestTerminalInputDecoder)             \
  X(TestSseChunkPartitions)               \
  X(TestConfigDocumentPreservesFile)      \
  X(TestConfigProposalAndCommit)          \
  X(TestProjectConfigTrustRestamp)        \
  X(TestConfigRegistryContract)           \
  X(TestSelfDescriptionSchemas)           \
  X(TestSseFraming)                       \
  X(TestWireAdapters)                     \
  X(TestWireStreams)                      \
  X(TestBackgroundValidation)              \
  X(TestSignalAndFileWatch)                \
  X(TestActivityBufferAndAdmission)        \
  X(TestActivitySessions)                  \
  X(TestActivityDescriptorAndInputPolicy)  \
  X(TestActivityWaitAndDelivery)           \
  X(TestToolExecutionPolicy)              \
  X(TestOpenRouterServerSearch)           \
  X(TestAttachmentEncoding)               \
  X(TestGrepTool)                         \
  X(TestPythonTool)                       \
  X(TestRuntimeOwnershipHelpers)          \
  X(TestAgentConfigAllowlist)             \
  X(TestEffectiveConfigReload)            \
  X(TestChildEnvironmentPolicy)           \
  X(TestModelPreference)                  \
  X(TestProviderTemplates)                \
  X(TestNamedProviders)                   \
  X(TestSafeJsonValues)                   \
  X(TestProjectInstructionDiscovery)      \
  X(TestMcpContractHelpers)               \
  X(TestConversation)                     \
  X(TestObservabilityEvents)              \
  X(TestWorkspaceScopedSession)           \
  X(TestProjectTrustTracksSemanticConfig) \
  X(TestScopedBaseAndMemory)              \
  X(TestSkillDiscovery)

#define UAGENT_DECLARE_TEST(name) void name();
UAGENT_TESTS(UAGENT_DECLARE_TEST)
#undef UAGENT_DECLARE_TEST

}  // namespace uagent

#define CHECK(expression) \
  ::uagent::Check((expression), #expression, __FILE__, __LINE__)

#endif  // UAGENT_TESTS_UNIT_TEST_SUPPORT_H_
