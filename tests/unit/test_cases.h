// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_TESTS_UNIT_TEST_CASES_H_
#define UAGENT_TESTS_UNIT_TEST_CASES_H_
namespace uagent {
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
  X(TestDiffLineColoring)                      \
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
  X(TestPromptScopes)                          \
  X(TestPromptRequestParity)                   \
  X(TestSseFraming)                            \
  X(TestWireCacheParity)                       \
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
#endif
