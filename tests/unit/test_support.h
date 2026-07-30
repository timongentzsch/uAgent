// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_TESTS_UNIT_TEST_SUPPORT_H_
#define UAGENT_TESTS_UNIT_TEST_SUPPORT_H_

#include <clocale>
#include <cstdio>
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
void TestSseChunkPartitions();
void TestSseFraming();
void TestBackgroundValidation();
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
void TestConversationAndContextPolicy();
void TestProjectTrustTracksSemanticConfig();
void TestScopedBaseAndMemory();
void TestSkillDiscovery();

}  // namespace uagent

#define CHECK(expression) ::uagent::Check((expression), #expression, __LINE__)

#endif  // UAGENT_TESTS_UNIT_TEST_SUPPORT_H_
