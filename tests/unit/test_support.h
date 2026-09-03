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

// Scale a millisecond deadline for an instrumented build, mirroring the Python
// harness's budget(). A sanitizer leg starts and renders several times slower
// than a plain one, so a fixed PTY round-trip budget there is a coin flip; CI
// raises the multiplier for those jobs rather than each deadline being retuned
// by hand.
inline int64_t BudgetMs(int64_t milliseconds) {
  const char* scale = std::getenv("UAGENT_TEST_TIMEOUT_SCALE");
  if (scale == nullptr) return milliseconds;
  char* end = nullptr;
  const double factor = std::strtod(scale, &end);
  if (end == scale || factor <= 1.0) return milliseconds;
  return static_cast<int64_t>(static_cast<double>(milliseconds) * factor);
}

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
// Records a failure and returns whether the condition held; the run continues
// so one execution reports every failure rather than the first.
bool Check(bool condition, const char* expression, const char* file, int line);

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
    // Short reads are the caller's business: resizing to what arrived keeps
    // the captured text honest instead of padding it with the NULs above.
    output.resize(fread(output.data(), 1, output.size(), capture));
  }
  fclose(capture);
  return output;
}

// The suite in run order: declarations and dispatch expand from this list.
#define UAGENT_TESTS(X)                        \
  X(TestForeignToolMarkup)                     \
  X(TestToolResults)                           \
  X(TestRegistries)                            \
  X(TestCommandAndDisplayRegistries)           \
  X(TestModelCatalogParsing)                   \
  X(TestOptions)                               \
  X(TestMarkdownBlankLines)                    \
  X(TestTableRetroErasesRenderedRows)          \
  X(TestInteractiveTranscriptFraming)          \
  X(TestMarkdownMath)                          \
  X(TestCapsAndEscaping)                       \
  X(TestFileTools)                             \
  X(TestMathTransliteration)                   \
  X(TestHostedSearchStatusRow)                 \
  X(TestActivityBar)                           \
  X(TestStatusBarDropsByPriority)              \
  X(TestPollCollapse)                          \
  X(TestTerminalSafety)                        \
  X(TestTerminalInputDecoder)                  \
  X(TestSseChunkPartitions)                    \
  X(TestChatCompletionAnnotationDeduplication) \
  X(TestConfigDocumentPreservesFile)           \
  X(TestConfigProposalAndCommit)               \
  X(TestProjectConfigTrustRestamp)             \
  X(TestConfigRegistryContract)                \
  X(TestSandboxPolicy)                         \
  X(TestSandboxRendering)                      \
  X(TestSandboxTrampolineArgs)                 \
  X(TestSandboxProbe)                          \
  X(TestStrictBooleanSettings)                 \
  X(TestSelfDescriptionSchemas)                \
  X(TestSseFraming)                            \
  X(TestWireAdapters)                          \
  X(TestWireStreams)                           \
  X(TestWireStreamHostedSearch)                \
  X(TestWireStreamMalformedValues)             \
  X(TestBackgroundValidation)                  \
  X(TestSignalAndFileWatch)                    \
  X(TestActivityBufferAndAdmission)            \
  X(TestActivityStateGraph)                    \
  X(TestDetachedActivityOwnership)             \
  X(TestActivitySessions)                      \
  X(TestActivityDescriptorAndInputPolicy)      \
  X(TestActivityWaitAndDelivery)               \
  X(TestCollaboratorMail)                      \
  X(TestToolExecutionPolicy)                   \
  X(TestOpenRouterServerSearch)                \
  X(TestAttachmentEncoding)                    \
  X(TestGrepTool)                              \
  X(TestPythonTool)                            \
  X(TestMemoryAlwaysOnSelection)               \
  X(TestRuntimeOwnershipHelpers)               \
  X(TestAgentConfigAllowlist)                  \
  X(TestEffectiveConfigReload)                 \
  X(TestChildEnvironmentPolicy)                \
  X(TestModelPreference)                       \
  X(TestProviderTemplates)                     \
  X(TestNamedProviders)                        \
  X(TestSafeJsonValues)                        \
  X(TestProjectInstructionDiscovery)           \
  X(TestMcpContractHelpers)                    \
  X(TestConversation)                          \
  X(TestObservabilityEvents)                   \
  X(TestWorkspaceScopedSession)                \
  X(TestProjectTrustTracksSemanticConfig)      \
  X(TestScopedBaseAndMemory)                   \
  X(TestSkillDiscovery)

#define UAGENT_DECLARE_TEST(name) void name();
UAGENT_TESTS(UAGENT_DECLARE_TEST)
#undef UAGENT_DECLARE_TEST

}  // namespace uagent

#define CHECK(expression) \
  ::uagent::Check((expression), #expression, __FILE__, __LINE__)

// CHECK for a condition the rest of the test body dereferences: a failed CHECK
// keeps going, so `CHECK(opt.has_value()); use(*opt);` crashes the whole binary
// instead of reporting one failed case. REQUIRE reports and leaves.
#define REQUIRE(expression)                                              \
  do {                                                                   \
    if (!::uagent::Check((expression), #expression, __FILE__, __LINE__)) \
      return;                                                            \
  } while (0)

#endif  // UAGENT_TESTS_UNIT_TEST_SUPPORT_H_
