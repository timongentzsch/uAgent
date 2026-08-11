// Copyright 2026 Timon Gentzsch

#include <curl/curl.h>

#include <iostream>

#include "tests/unit/test_support.h"

namespace uagent {

int failures = 0;

void Check(bool condition, const char* expression, int line) {
  if (condition) return;
  std::cerr << "FAIL line " << line << ": " << expression << '\n';
  ++failures;
}

int RunTests() {
  std::setlocale(LC_CTYPE, "");
  curl_global_init(CURL_GLOBAL_DEFAULT);
  TestTextToolProtocol();
  TestToolResults();
  TestRegistries();
  TestOptions();
  TestLineNumberStripping();
  TestMarkdownMath();
  TestCapsAndEscaping();
  TestFileTools();
  TestTerminalSafety();
  TestTerminalInputDecoder();
  TestSseChunkPartitions();
  TestSseFraming();
  TestBackgroundValidation();
  TestToolExecutionPolicy();
  TestOpenRouterServerSearch();
  TestAttachmentEncoding();
  TestGrepTool();
  TestPythonTool();
  TestRuntimeOwnershipHelpers();
  TestAgentConfigAllowlist();
  TestChildEnvironmentPolicy();
  TestModelPreference();
  TestProviderTemplates();
  TestNamedProviders();
  TestSafeJsonValues();
  TestProjectInstructionDiscovery();
  TestMcpContractHelpers();
  TestConversationAndContextPolicy();
  TestWorkspaceScopedSession();
  TestProjectTrustTracksSemanticConfig();
  TestScopedBaseAndMemory();
  TestSkillDiscovery();
  curl_global_cleanup();
  if (failures) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "all core tests passed\n";
  return 0;
}

}  // namespace uagent

int main() { return uagent::RunTests(); }
