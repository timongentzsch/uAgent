// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_TESTS_UNIT_TEST_SUPPORT_H_
#define UAGENT_TESTS_UNIT_TEST_SUPPORT_H_

#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "include/agent.h"
#include "include/app/options.h"
#include "include/app/runtime.h"
#include "include/cli.h"
#include "include/core/child_env.h"
#include "include/core/config.h"
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

extern int failures;
void Check(bool condition, const char* expression, int line);

void TestTextToolProtocol();
void TestToolResults();
void TestRegistries();
void TestOptions();
void TestLineNumberStripping();
void TestMarkdownMath();
void TestCapsAndEscaping();
void TestFileTools();
void TestTerminalSafety();
void TestTerminalInputDecoder();
void TestSseChunkPartitions();
void TestSseFraming();
void TestBackgroundValidation();
void TestActivitySessions();
void TestToolExecutionPolicy();
void TestOpenRouterServerSearch();
void TestAttachmentEncoding();
void TestGrepTool();
void TestPythonTool();
void TestRuntimeOwnershipHelpers();
void TestAgentConfigAllowlist();
void TestChildEnvironmentPolicy();
void TestModelPreference();
void TestProviderTemplates();
void TestNamedProviders();
void TestSafeJsonValues();
void TestProjectInstructionDiscovery();
void TestMcpContractHelpers();
void TestWorkspaceScopedSession();
void TestConversation();
void TestProjectTrustTracksSemanticConfig();
void TestScopedBaseAndMemory();
void TestSkillDiscovery();

}  // namespace uagent

#define CHECK(expression) ::uagent::Check((expression), #expression, __LINE__)

#endif  // UAGENT_TESTS_UNIT_TEST_SUPPORT_H_
