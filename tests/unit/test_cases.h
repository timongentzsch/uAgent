// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_TESTS_UNIT_TEST_CASES_H_
#define UAGENT_TESTS_UNIT_TEST_CASES_H_
namespace uagent {
// The suite in run order: declarations and dispatch expand from this list.
#define UAGENT_TESTS(X)                          \
  X(TestForeignToolMarkup)                       \
  X(TestToolResults)                             \
  X(TestRegistries)                              \
  X(TestCommandAndDisplayRegistries)             \
  X(TestModelCatalogParsing)                     \
  X(TestModelQueryMatching)                      \
  X(TestOptions)                                 \
  X(TestMarkdownBlankLines)                      \
  X(TestTableRetroErasesRenderedRows)            \
  X(TestInteractiveTranscriptFraming)            \
  X(TestMarkdownMath)                            \
  X(TestCapsAndEscaping)                         \
  X(TestFileTools)                               \
  X(TestMathTransliteration)                     \
  X(TestHostedSearchStatusRow)                   \
  X(TestActivityBar)                             \
  X(TestDiffLineColoring)                        \
  X(TestReplayBlocksMirrorLiveRows)              \
  X(TestStatusBarDropsByPriority)                \
  X(TestPollCollapse)                            \
  X(TestTerminalSafety)                          \
  X(TestTerminalInputDecoder)                    \
  X(TestSseChunkPartitions)                      \
  X(TestChatCompletionAnnotationDeduplication)   \
  X(TestConfigDocumentPreservesFile)             \
  X(TestConfigProposalAndCommit)                 \
  X(TestProjectConfigTrustRestamp)               \
  X(TestConfigRegistryContract)                  \
  X(TestRuntimeConfigCoherence)                  \
  X(TestSessionCommandKinds)                     \
  X(TestSessionCommandFields)                    \
  X(TestSessionCommandRejects)                   \
  X(TestReceiptLog)                              \
  X(TestReceiptLogBackpressure)                  \
  X(TestHostCommandKinds)                        \
  X(TestSandboxPolicy)                           \
  X(TestSandboxRendering)                        \
  X(TestSandboxTrampolineArgs)                   \
  X(TestSandboxProbe)                            \
  X(TestStrictBooleanSettings)                   \
  X(TestSelfDescriptionSchemas)                  \
  X(TestPromptScopes)                            \
  X(TestPromptRequestParity)                     \
  X(TestSseFraming)                              \
  X(TestWireCacheParity)                         \
  X(TestWireAdapters)                            \
  X(TestWireStreams)                             \
  X(TestWireStreamHostedSearch)                  \
  X(TestWireStreamMalformedValues)               \
  X(TestBackgroundValidation)                    \
  X(TestSignalAndFileWatch)                      \
  X(TestActivityBufferAndAdmission)              \
  X(TestActivityStateGraph)                      \
  X(TestDetachedActivityOwnership)               \
  X(TestActivitySessions)                        \
  X(TestActivityDescriptorAndInputPolicy)        \
  X(TestActivityWaitAndDelivery)                 \
  X(TestCollaboratorMail)                        \
  X(TestSessionMail)                             \
  X(TestSessionLinks)                            \
  X(TestToolExecutionPolicy)                     \
  X(TestBlockingWaitCalls)                       \
  X(TestOpenRouterServerSearch)                  \
  X(TestAttachmentEncoding)                      \
  X(TestVectorAndHeicAttachments)                \
  X(TestAudioVideoAttachments)                   \
  X(TestWorkerBinaryIdentity)                    \
  X(TestGrepTool)                                \
  X(TestPythonTool)                              \
  X(TestMemoryAlwaysOnSelection)                 \
  X(TestEarlyTurnInterruption)                   \
  X(TestRuntimeOwnershipHelpers)                 \
  X(TestAgentConfigAllowlist)                    \
  X(TestEffectiveConfigReload)                   \
  X(TestChildEnvironmentPolicy)                  \
  X(TestModelPreference)                         \
  X(TestEffectiveImageModel)                     \
  X(TestProviderTemplates)                       \
  X(TestNamedProviders)                          \
  X(TestSafeJsonValues)                          \
  X(TestProjectInstructionDiscovery)             \
  X(TestMcpContractHelpers)                      \
  X(TestConversation)                            \
  X(TestForkAtTurnAndLineage)                    \
  X(TestRewindAndShare)                          \
  X(TestSessionPrefixMatch)                      \
  X(TestTitleModelDefault)                       \
  X(TestAttachmentDeliveryAnnouncements)         \
  X(TestDisplayFactEvictionKeepsSmallReceipts)   \
  X(TestAttachmentHistoryRendering)              \
  X(TestHistoryReplaySkipsBareHeader)            \
  X(TestCompactionKeepsDisplayIdentity)          \
  X(TestLateRetainedBlockInsertsInSequenceOrder) \
  X(TestToolResultHealsMissingMetadata)          \
  X(TestObservabilityEvents)                     \
  X(TestWorkspaceScopedSession)                  \
  X(TestProjectTrustTracksSemanticConfig)        \
  X(TestScopedBaseAndMemory)                     \
  X(TestSkillDiscovery)

#define UAGENT_DECLARE_TEST(name) void name();
UAGENT_TESTS(UAGENT_DECLARE_TEST)
#undef UAGENT_DECLARE_TEST

}  // namespace uagent
#endif
