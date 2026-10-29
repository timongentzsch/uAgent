// Copyright 2026 Timon Gentzsch

#include "include/app/runtime.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent/agent.h"
#include "include/agent/child_agent.h"
#include "include/agent/dispatch.h"
#include "include/agent/protocol.h"
#include "include/agent/session_store.h"
#include "include/api/retry.h"
#include "include/app/launch.h"
#include "include/app/permissions.h"
#include "include/app/self_description.h"
#include "include/core/child_env.h"
#include "include/core/config.h"
#include "include/core/config_registry.h"
#include "include/core/debug.h"
#include "include/core/effective_config.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/mailbox.h"
#include "include/core/project.h"
#include "include/core/runtime_config.h"
#include "include/core/settings_store.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/providers/providers.h"
#include "include/tools/files.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestEarlyTurnInterruption() {
  TestWorkspace workspace("early-turn-interruption");
  for (const bool before_turn : {true, false}) {
    Api api(RuntimeConfig{});
    std::vector<Tool> tools;
    ActivitySupervisor processes;
    UsageAccumulator usage;
    Agent agent(api, tools, processes, usage,
                [](const Tool&, const json&, int64_t) {
                  return std::string("denied");
                });
    Observability observable;
    auto* previous = ActiveObservability();
    int notices = 0, responses = 0;
    observable.Subscribe([&](const AppEvent& event) {
      if (!before_turn && event.type == "message.changed") {
        SteeringState().Request();
      }
      if (event.type == "notice" &&
          JsonValue(event.data, "text", "") == "interrupted") {
        ++notices;
      }
      if (event.type == "response.started") ++responses;
    });
    SetObservability(&observable);
    if (before_turn) SteeringState().Request();
    agent.Turn("cancel before the model call");
    SteeringState().Take();
    SetObservability(previous);
    CHECK(notices == 1);
    CHECK(responses == 0);
  }
}

// A side question's thread has its own stop: the main turn's abort does not
// reach it, clearing its stop leaves the main turn's intact, and its flag
// alone cancels it.
void TestLocalAbortIsolation() {
  ClearAbort();
  RequestAbort();
  std::atomic<bool> side{false};
  std::thread([&] {
    LocalAbort local(side);
    CHECK(!AbortRequested());
    ClearAbort();
    side = true;
    CHECK(AbortRequested());
  }).join();
  CHECK(AbortRequested());
  ClearAbort();
  CHECK(!AbortRequested());
}

void TestBudgetMsScaling() {
  {
    ScopedEnv scale("UAGENT_TEST_TIMEOUT_SCALE", "6");
    CHECK(BudgetMs(2) == 12);
    CHECK(BudgetMs(0) == 0);
    CHECK(BudgetMs(-2) == -2);
  }
  for (const char* invalid : {"nan", "inf", "1e308", "6junk"}) {
    ScopedEnv scale("UAGENT_TEST_TIMEOUT_SCALE", invalid);
    CHECK(BudgetMs(2) == 2);
  }
  {
    ScopedEnv scale("UAGENT_TEST_TIMEOUT_SCALE", "9223372036854775808");
    CHECK(BudgetMs(1) == 1);
  }
}

void TestRouteKeysAndShutdown() {
  std::ifstream contract(std::string(UAGENT_TEST_SOURCE_DIR) +
                         "/tests/fixtures/route_contract.json");
  json route_contract = json::parse(contract, nullptr, false);
  CHECK(route_contract.is_object());
  for (const json& sample : route_contract["route_keys"]) {
    CHECK(RouteKey(JsonValue(sample, "base_url", ""),
                   JsonValue(sample, "provider", ""),
                   JsonValue(sample, "model", ""),
                   JsonValue(sample, "effort", "")) ==
          JsonValue(sample, "expected", ""));
  }
  AppRuntime runtime(RuntimeConfig{});
  runtime.Shutdown();
  runtime.Shutdown();

  g_signal_abort.test_and_set(std::memory_order_relaxed);
  CHECK(RunCancellable([] {}));
  CHECK(AbortRequested());
  ClearAbort();
}

void TestUsageAccounting() {
  UsageAccumulator accumulator;
  accumulator.Add(json{{"prompt_tokens", 5},
                       {"completion_tokens", 2},
                       {"prompt_tokens_details", {{"cache_write_tokens", 2}}}});
  accumulator.Add(Usage{3, 4, 1, 0, 0.25});
  Usage total = accumulator.Take();
  CHECK(total.input == 6);
  CHECK(total.output == 6);
  CHECK(total.cache_read == 1);
  CHECK(total.cache_write == 2);
  CHECK(total.cost == 0.25);
  CHECK(accumulator.Take().input == 0);

  Usage responses, anthropic;
  responses.Add({{"input_tokens", 100},
                 {"input_tokens_details",
                  {{"cached_tokens", 60}, {"cache_write_tokens", 30}}}});
  anthropic.Add({{"input_tokens", 10},
                 {"cache_read_input_tokens", 60},
                 {"cache_creation_input_tokens", 30}});
  CHECK(UsageJson(responses) == UsageJson(anthropic));
  CHECK(responses.input == 10);
  CHECK(responses.CacheHitPercent() == 60);

  std::filesystem::path ledger = std::filesystem::temp_directory_path() /
                                 ("uagent-ledger-" + std::to_string(getpid()));
  std::filesystem::remove(ledger);
  std::atomic<int> writers_done{0};
  std::atomic<bool> append_ok{true};
  auto append = [&] {
    for (int i = 0; i < 100; ++i) {
      std::string error;
      if (!AppendPrivateLine(ledger.string(), "usage", error)) {
        append_ok = false;
        break;
      }
    }
    ++writers_done;
  };
  std::thread first(append);
  std::thread second(append);
  size_t lines = 0;
  while (writers_done.load() < 2) {
    std::string drained;
    std::string error;
    CHECK(TakePrivateText(ledger.string(), drained, error));
    lines +=
        static_cast<size_t>(std::count(drained.begin(), drained.end(), '\n'));
  }
  first.join();
  second.join();
  CHECK(append_ok.load());
  std::string drained;
  std::string drain_error;
  CHECK(TakePrivateText(ledger.string(), drained, drain_error));
  lines +=
      static_cast<size_t>(std::count(drained.begin(), drained.end(), '\n'));
  CHECK(lines == 200);
  CHECK(std::filesystem::file_size(ledger) == 0);
  std::filesystem::remove(ledger);
}

void TestRuntimeConfigAndRequests() {
  // One table drives the set and the clear, so the two cannot drift apart.
  static constexpr std::pair<const char*, const char*> kRuntimeEnv[] = {
      {"UAGENT_MAX_STEPS", "0"},
      {"UAGENT_MAX_TOOL_CALLS", "0"},
      {"UAGENT_MAX_TURN_TOKENS", "123"},
      {"UAGENT_SESSION_TOKEN_BUDGET", "456"},
      {"UAGENT_TEST_LONG", "999999999999999999999999999999"},
      {"UAGENT_SESSION_BUDGET", "2.5"},
      {"UAGENT_MAX_TURN_COST", "nan"},
      {"UAGENT_WEB_SEARCH_MODEL", "vendor/search"},
      {"UAGENT_WEB_SEARCH_BACKEND", "off"},
  };
  for (const auto& entry : kRuntimeEnv) setenv(entry.first, entry.second, 1);
  RuntimeConfig config = RuntimeConfig::FromEnvironment();
  CHECK(config.max_steps == 0);
  CHECK(config.max_tool_calls == 0);
  CHECK(config.max_turn_tokens == 123);
  CHECK(config.session_token_budget == 456);
  CHECK(EnvLong("UAGENT_TEST_LONG", 7) == 7);
  CHECK(config.session_budget == 2.5);
  CHECK(config.max_turn_cost == 0);
  CHECK(config.web_search_model == "vendor/search");
  CHECK(config.web_search_backend == "off");
  json diagnostics = config.DiagnosticJson();
  CHECK(diagnostics.value("max_steps", int64_t{0}) == config.max_steps);
  CHECK(diagnostics.value("max_turn_tokens", int64_t{0}) == 123);
  CHECK(diagnostics.value("session_token_budget", int64_t{0}) == 456);
  CHECK(diagnostics.value("web_search_model", "") == config.web_search_model);
  CHECK(diagnostics.value("web_search_backend", "") ==
        config.web_search_backend);
  for (const auto& entry : kRuntimeEnv) unsetenv(entry.first);
  RuntimeConfig defaults;
  CHECK(defaults.max_steps == 0);
  CHECK(defaults.max_tool_calls == 0);
  CHECK(defaults.max_turn_tokens == 0);
  CHECK(defaults.session_token_budget == 0);
  CHECK(defaults.max_turn_cost == 0);
  CHECK(AutoCompactTokens() == 0);
  CHECK(defaults.max_turn_seconds == 0);
  CHECK(defaults.stream_timeout_s == 300);
  CHECK(defaults.request_timeout_s == 600);

  RuntimeConfig routed;
  Api api(routed);
  api.base_url = "https://openrouter.ai/api/v1";
  UseOpenRouterDialect(api);
  api.options["provider"] = "streamlake";
  api.model = "test";
  json body =
      api.BuildRequestBody(json::array(), json::array(), "stable-session");
  CHECK(body.value("session_id", "") == "stable-session");
  CHECK(body["provider"]["order"][0] == "streamlake");
  CHECK(!body["provider"].value("allow_fallbacks", true));
  CHECK(!body.contains("stream_options"));
  // A routing suffix is part of the id the endpoint is asked for.
  api.model = "test:free:exacto";
  body = api.BuildRequestBody(json::array(), json::array(), "stable-session");
  CHECK(body.value("model", "") == "test:free:exacto");
  api.reasoning_effort = "low";
  body = api.BuildRequestBody(json::array(), json::array(), "stable-session");
  CHECK(body["reasoning"].value("effort", "") == "low");
  CHECK(!body.contains("reasoning_effort"));
  // An endpoint that declares it asks for prompt caching for every model.
  CHECK(body["cache_control"].value("type", "") == "ephemeral");
  api.model = "test:free";
  UseEndpoint(api, "http://127.0.0.1:8080/v1");
  body = api.BuildRequestBody(json::array(), json::array(), "stable-session");
  CHECK(body.value("model", "") == "test:free");
  CHECK(!body.contains("session_id"));
  CHECK(!body.contains("provider"));
  CHECK(!body.contains("cache_control"));

  api.base_url = "https://api.openai.com/v1";
  UseEndpoint(api, api.base_url);
  api.reasoning_effort = "high";
  body = api.BuildRequestBody(json::array(), json::array());
  CHECK(body.value("reasoning_effort", "") == "high");
  // Uncapped by default: neither spelling is sent, so the provider's own
  // maximum applies and never clamps a derived thinking budget.
  CHECK(!body.contains("max_completion_tokens"));
  CHECK(!body.contains("max_tokens"));
  setenv("UAGENT_MAX_TOKENS", "4096", 1);
  body = api.BuildRequestBody(json::array(), json::array());
  CHECK(body.value("max_completion_tokens", int64_t{0}) == 4096);
  CHECK(!body.contains("max_tokens"));
  unsetenv("UAGENT_MAX_TOKENS");
  CHECK(body["stream_options"].value("include_usage", false));
  api.wire_api = WireApi::kResponses;
  body = api.BuildRequestBody(json::array(), json::array(), "stable-session");
  CHECK(body.value("prompt_cache_key", "") == HashHex("stable-session"));
  // Session ids are 64-bit FNV-1a; the standard vectors pin the constants.
  CHECK(HashHex("") == "cbf29ce484222325");
  CHECK(HashHex("a") == "af63dc4c8601ec8c");
  // A proxy in front of the API gets the key as well, and passes it on.
  api.base_url = "http://127.0.0.1:8080/v1";
  body = api.BuildRequestBody(json::array(), json::array(), "stable-session");
  CHECK(body.value("prompt_cache_key", "") == HashHex("stable-session"));
  api.base_url = "https://api.openai.com/v1";
  UseEndpoint(api, api.base_url);

  // The file-parser plugin rides the conversation request, so it is sent only
  // where it is understood and only when a document is actually attached.
  json documented = json::array(
      {{{"role", "user"},
        {"content", json::array({{{"type", "text"}, {"text", "read it"}},
                                 {{"type", "file"},
                                  {"file", {{"filename", "a.pdf"}}}}})}}});
  CHECK(
      !api.BuildRequestBody(documented, json::array(), "").contains("plugins"));
  Api via_open_router(config);
  UseEndpoint(via_open_router, "https://openrouter.ai/api/v1");
  json plugged =
      via_open_router.BuildRequestBody(documented, json::array(), "");
  CHECK(plugged["plugins"][0]["id"] == "file-parser");
  CHECK(plugged["plugins"][0]["pdf"]["engine"] == "cloudflare-ai");
  // A request without a document does not carry it.
  CHECK(!via_open_router
             .BuildRequestBody(
                 json::array({{{"role", "user"}, {"content", "plain"}}}),
                 json::array(), "")
             .contains("plugins"));

  json assistant = {{"role", "assistant"}, {"content", ""}};
  ChatResult reasoning_result;
  reasoning_result.reasoning = "reasoning state";
  reasoning_result.reasoning_field = true;
  reasoning_result.reasoning_details =
      json::array({{{"type", "reasoning.text"},
                    {"index", 0},
                    {"text", "reasoning state"}}});
  api.base_url = "https://openrouter.ai/api/v1";
  UseOpenRouterDialect(api);
  api.PreserveAssistantReasoning(assistant, reasoning_result);
  CHECK(!assistant.contains("reasoning"));
  CHECK(assistant["reasoning_details"] == reasoning_result.reasoning_details);
  reasoning_result.reasoning_details = json::array();
  reasoning_result.reasoning_details_field = true;
  reasoning_result.reasoning.clear();
  json empty_details_assistant = {{"role", "assistant"}, {"content", ""}};
  api.PreserveAssistantReasoning(empty_details_assistant, reasoning_result);
  CHECK(empty_details_assistant.contains("reasoning_details"));
  CHECK(empty_details_assistant["reasoning_details"].empty());
  CHECK(!empty_details_assistant.contains("reasoning"));
  reasoning_result.reasoning = "reasoning state";
  reasoning_result.reasoning_details_field = false;
  json text_assistant = {{"role", "assistant"}, {"content", ""}};
  api.PreserveAssistantReasoning(text_assistant, reasoning_result);
  CHECK(text_assistant.value("reasoning", "") == "reasoning state");
  api.base_url = "http://127.0.0.1:8787/api/v1";
  json local_assistant = {{"role", "assistant"}, {"content", ""}};
  api.PreserveAssistantReasoning(local_assistant, reasoning_result);
  CHECK(local_assistant.value("reasoning", "") == "reasoning state");
  api.base_url = "https://example.com/v1";
  UseEndpoint(api, api.base_url);
  json generic_assistant = {{"role", "assistant"}, {"content", ""}};
  api.PreserveAssistantReasoning(generic_assistant, reasoning_result);
  CHECK(!generic_assistant.contains("reasoning"));
  CHECK(!generic_assistant.contains("reasoning_details"));
  CHECK(!generic_assistant.contains("reasoning_content"));
  reasoning_result.reasoning_content_field = true;
  json deepseek_assistant = {{"role", "assistant"}, {"content", ""}};
  api.PreserveAssistantReasoning(deepseek_assistant, reasoning_result);
  CHECK(deepseek_assistant.value("reasoning_content", "") == "reasoning state");

  Api generic_api(config), router_api(config);
  UseEndpoint(generic_api, "http://127.0.0.1:8080/v1");
  UseOpenRouterDialect(router_api);
  const ProviderCapabilities& generic = generic_api.capabilities;
  const ProviderCapabilities& router = router_api.capabilities;
  CHECK(generic.stream_usage_option);
  CHECK(!generic_api.features.Has(Feature::kRawVendorIds));
  CHECK(!router.stream_usage_option);
  CHECK(router_api.features.Has(Feature::kRawVendorIds));
  CHECK(router_api.features.Has(Feature::kReasoningObject));
  ChatResult rejected;
  rejected.http_status = 400;
  rejected.remote_error_code = "unsupported_parallel_tool_calls";
  rejected.error = "request rejected";
  CHECK(RejectedRouteCapability(rejected, router) ==
        RejectedCapability::kParallelTools);
  rejected.remote_error_code.clear();
  rejected.error = "unsupported stream_options";
  CHECK(RejectedRouteCapability(rejected, router) == RejectedCapability::kNone);
  CHECK(RejectedRouteCapability(rejected, generic) ==
        RejectedCapability::kStreamUsage);
  rejected.error = "Invalid tool schema: parameters must be an object";
  CHECK(RejectedRouteCapability(rejected, generic) ==
        RejectedCapability::kNone);
  rejected.remote_error_code = "unsupported_value";
  rejected.error = "This model does not support image input";
  CHECK(RejectedRouteCapability(rejected, generic) ==
        RejectedCapability::kImageInput);
  rejected.error = "This model does not support audio input";
  CHECK(RejectedRouteCapability(rejected, generic) ==
        RejectedCapability::kAudioInput);
  rejected.error = "This model does not support video input";
  CHECK(RejectedRouteCapability(rejected, generic) ==
        RejectedCapability::kVideoInput);
  rejected.error = "Invalid image input: size exceeds limit";
  CHECK(RejectedRouteCapability(rejected, generic) ==
        RejectedCapability::kNone);
  rejected.error = "This model does not support image input";
  rejected.http_status = 503;
  CHECK(RejectedRouteCapability(rejected, generic) ==
        RejectedCapability::kNone);
  rejected.http_status = 400;
  rejected.remote_error_code.clear();
  // Native tool calls are not a negotiable capability: a route that rejects
  // them has no fallback to degrade to, so this stays an ordinary error and
  // the turn reports the provider's own message.
  Api summary_api(config);
  UseEndpoint(summary_api, "https://api.openai.com/v1", WireApi::kResponses);
  auto summaries = summary_api.capabilities;
  CHECK(summaries.reasoning_summary);
  UseEndpoint(summary_api, "https://proxy.test/v1", WireApi::kResponses);
  CHECK(!summary_api.capabilities.reasoning_summary);
  rejected.error = "Unsupported parameter: reasoning.summary";
  CHECK(RejectedRouteCapability(rejected, summaries) ==
        RejectedCapability::kReasoningSummary);
  rejected.error =
      "Unsupported parameter: 'reasoning' is not supported with this model";
  CHECK(RejectedRouteCapability(rejected, summaries) ==
        RejectedCapability::kReasoningSummary);
  summaries.SetModelFeatures({{"reasoning_summary", false}});
  summaries.SetModelFeatures(
      {{"reasoning_summary", true}, {"adaptive_thinking", true}});
  CHECK(!summaries.reasoning_summary);
  CHECK(summaries.adaptive_thinking);
  rejected.error = "This model does not support tool calling";
  CHECK(RejectedRouteCapability(rejected, generic) ==
        RejectedCapability::kNone);
}

void TestRetrySafety() {
  ChatResult retryable;
  retryable.retryable = true;
  retryable.semantic_progress = true;
  retryable.reasoning = "partial reasoning";
  CHECK(SafeToRetry(retryable));
  retryable.usage = {{"prompt_tokens", 1}};
  CHECK(!SafeToRetry(retryable));
  retryable.usage = nullptr;
  retryable.annotations.push_back({{"url", "https://example.test"}});
  CHECK(!SafeToRetry(retryable));
  retryable.annotations.clear();
  retryable.content = "visible answer";
  CHECK(!SafeToRetry(retryable));
  json routed_error = {
      {"type", "api_error"},
      {"metadata", {{"error_type", "timeout"}, {"provider_code", "slow"}}}};
  CHECK(RemoteErrorType(routed_error) == "timeout");
  CHECK(RemoteErrorCode(routed_error) == "slow");
}

void TestPermissionPolicy() {
  TestWorkspace workspace("permission-policy");
  ApprovalMode approval;
  CHECK(ParseApprovalMode("ask", approval));
  CHECK(approval == ApprovalMode::kAsk);
  CHECK(ParseApprovalMode("auto", approval));
  CHECK(approval == ApprovalMode::kAuto);
  CHECK(ParseApprovalMode("yolo", approval));
  CHECK(approval == ApprovalMode::kYolo);
  CHECK(ParseApprovalMode("plan", approval));
  CHECK(approval == ApprovalMode::kPlan);
  CHECK(!ParseApprovalMode("guess", approval));

  Tool tool;
  tool.name = "write_file";
  tool.provider = "builtin";
  tool.parameters = {{"type", "object"}};
  tool.mutating = true;
  const auto approval_class = ApprovalClass::kYoloEligibleMutation;
  const std::string first =
      PermissionKey(tool, {{"path", "first"}}, approval_class);
  const std::string second =
      PermissionKey(tool, {{"path", "second"}}, approval_class);
  CHECK(first != second);
  // A tool may say what an approval covers: calls that differ only in what
  // it leaves out share one key.
  tool.approval_subject = [](const json& arguments) {
    return json{{"site", arguments.value("site", "")}};
  };
  CHECK(PermissionKey(tool, {{"site", "a"}, {"view", "1"}}, approval_class) ==
        PermissionKey(tool, {{"site", "a"}, {"view", "2"}}, approval_class));
  CHECK(PermissionKey(tool, {{"site", "a"}}, approval_class) !=
        PermissionKey(tool, {{"site", "b"}}, approval_class));
  tool.approval_subject = nullptr;
  // What a call's approval covered reaches its own run.
  {
    Tool held;
    held.name = "held";
    held.parameters = {{"type", "object"}};
    json reached;
    held.run = [&](const json&, const ToolContext& context) {
      reached = context.approved;
      return ToolSuccess("ran");
    };
    CallTask task;
    task.tool = &held;
    task.args = json::object();
    task.approved = {{"origin", "https://example.com"}};
    ToolCall call;
    call.id = "held-1";
    call.name = "held";
    call.args = "{}";
    ExecuteCall(task, call, 1, 1, ToolContext{}, 30, "");
    CHECK(reached == task.approved);
  }

  // Risks name what approving risks, most serious first: a file write
  // inside the folder only makes changes; one outside says so.
  auto risk_ids = [](const json& risks) {
    std::string ids;
    for (const json& risk : risks) ids += JsonValue(risk, "id", "") + ",";
    return ids;
  };
  tool.capabilities = Capability(ToolCapability::kMutate);
  CHECK(risk_ids(ApprovalRisks(tool, {{"path", "a.txt"}}, CanonicalCwd())) ==
        "writes,");
  CHECK(risk_ids(ApprovalRisks(tool, {{"path", "/etc/hosts"}},
                               CanonicalCwd())) == "writes,outside,");
  Tool command = tool;
  command.capabilities = Capability(ToolCapability::kExecute) |
                         Capability(ToolCapability::kMutate);
  CHECK(risk_ids(ApprovalRisks(command, {{"command", "ls"}}, CanonicalCwd())) ==
        "runs,");
  Tool fetch = tool;
  fetch.mutating = false;
  fetch.capabilities = Capability(ToolCapability::kInspect) |
                       Capability(ToolCapability::kExternal);
  CHECK(risk_ids(ApprovalRisks(fetch, {{"url", "https://x"}},
                               CanonicalCwd())) == "network,");
  std::string error;
  CHECK(RememberRepositoryPermission(CanonicalCwd(), first, tool.name,
                                     "write first", error));
  CHECK(RepositoryPermissionAllows(CanonicalCwd(), first));
  CHECK(!RepositoryPermissionAllows(CanonicalCwd(), second));
  json rules = PermissionRulesControl({{"action", "list"}});
  CHECK(rules["rules"].size() == 1);
  rules = PermissionRulesControl({{"action", "delete"}, {"key", first}});
  CHECK(rules["rules"].empty());
}

void TestAttributedUsageAccumulator() {
  UsageAccumulator accumulator;
  Usage direct;
  direct.input = 3;
  Usage child;
  child.output = 5;
  child.cost = 0.25;
  child.cost_reported = true;
  accumulator.Add(direct);
  accumulator.Add("provider/model", child, 7,
                  {{"model_calls", 2}, {"tool_calls", 4}});

  AccumulatedUsage batch = accumulator.TakeAll();
  CHECK(batch.unassigned.input == 3);
  CHECK(batch.turns[7].output == 5);
  CHECK(batch.turns[7].cost == 0.25);
  CHECK(batch.routes["provider/model"].output == 5);
  CHECK(batch.turn_statistics[7]["model_calls"] == 2);
  CHECK(batch.turn_statistics[7]["tool_calls"] == 4);
  CHECK(accumulator.TakeAll().routes.empty());

  // A title call or memory extraction belongs to no turn; its calls still
  // count instead of being dropped with the turn.
  accumulator.Add("title", direct, 0, {{"model_calls", 1}});
  batch = accumulator.TakeAll();
  CHECK(batch.unassigned.input == 3);
  CHECK(batch.unassigned_statistics["model_calls"] == 1);
  CHECK(accumulator.TakeAll().unassigned_statistics.empty());

  json total = {{"model_calls", 2}, {"duration_ms", 1.5}};
  MergeNumericStatistics(
      total, {{"model_calls", 3}, {"duration_ms", 2.5}, {"complete", true}});
  CHECK(total["model_calls"] == 5);
  CHECK(total["duration_ms"] == 4.0);
  CHECK(!total.contains("complete"));

  json prefixed = PrefixNumericStatistics(total, "side_");
  CHECK(prefixed["side_model_calls"] == 5);
  CHECK(prefixed["side_duration_ms"] == 4.0);
  json flattened = FlattenNumericStatistics(prefixed, "side_");
  CHECK(flattened == total);

  // Grouped counters, such as calls per serving provider, merge the same way.
  json served = {{"served_by", {{"Wafer", {{"model_calls", 1}}}}}};
  MergeNumericStatistics(
      served, {{"served_by",
                {{"Wafer", {{"model_calls", 1}, {"rejected_calls", 1}}},
                 {"DigitalOcean", {{"model_calls", 1}}}}}});
  CHECK(served["served_by"]["Wafer"]["model_calls"] == 2);
  CHECK(served["served_by"]["Wafer"]["rejected_calls"] == 1);
  CHECK(served["served_by"]["DigitalOcean"]["model_calls"] == 1);

  Usage current;
  current.input = 11;
  current.output = 7;
  current.cost = 0.4;
  current.cost_reported = true;
  Usage prior;
  prior.input = 4;
  prior.output = 9;
  prior.cost = 0.15;
  Usage usage_delta = UsageDifference(current, prior);
  CHECK(usage_delta.input == 7);
  CHECK(usage_delta.output == 0);
  CHECK(usage_delta.cost == 0.25);
  CHECK(usage_delta.cost_reported);

  Usage extreme_current;
  extreme_current.input = std::numeric_limits<int64_t>::max();
  Usage extreme_prior;
  extreme_prior.input = -1;
  CHECK(UsageDifference(extreme_current, extreme_prior).input ==
        std::numeric_limits<int64_t>::max());
  json extreme_statistics = {
      {"calls", std::numeric_limits<int64_t>::max() - 1}};
  MergeNumericStatistics(extreme_statistics,
                         {{"calls", std::numeric_limits<uint64_t>::max()}});
  CHECK(extreme_statistics["calls"] == std::numeric_limits<int64_t>::max());
}

// Empty UAGENT_IMAGE_MODEL no longer disables vision: an explicit model
// wins, a vision-capable main route is inherited, otherwise a vision model of
// the same endpoint's catalogue backs the analysis side call.
void TestEffectiveImageModel() {
  Api api(RuntimeConfig{});
  std::vector<Tool> tools;
  ActivitySupervisor processes;
  UsageAccumulator usage;
  Agent agent(
      api, tools, processes, usage,
      [](const Tool&, const json&, int64_t) { return std::string("denied"); });
  api.config.image_model = "custom/vision";
  api.capabilities.image_input = false;
  CHECK(agent.EffectiveImageModel() == "custom/vision");
  api.config.image_model.clear();
  api.capabilities.image_input = true;
  CHECK(agent.EffectiveImageModel().empty());
  api.capabilities.image_input = false;
  // No catalogue to find a vision model in: none, said in words.
  CHECK(agent.EffectiveImageModel().empty());
}

// Mail that wakes a session is its to lose until the turn it starts has the
// text: a runtime killed between the save and that turn receives it again.
// A harness note is labelled as not the user's, and a worktree's .git
// file counts as a repository.
void TestHarnessNoteAndRepository() {
  CHECK(HarnessNote("chat", "Nobody is typing now.") ==
        "[chat, not a user message] Nobody is typing now.");

  TestWorkspace workspace("harness-note-and-repository");
  const std::filesystem::path nested = workspace.workspace / "a" / "b";
  std::filesystem::create_directories(nested);
  CHECK(!InGitRepository(nested));
  // A worktree's .git is a file; a repository's, a directory.
  std::ofstream(workspace.workspace / ".git") << "gitdir: elsewhere\n";
  CHECK(InGitRepository(nested));
  std::filesystem::remove(workspace.workspace / ".git");
  std::filesystem::create_directory(workspace.workspace / ".git");
  CHECK(InGitRepository(workspace.workspace));
}

void TestAWakeSurvivesARuntimeKilledBeforeItsTurn() {
  TestWorkspace workspace("wake-before-turn");
  const std::string folder = workspace.workspace.string();
  const std::string path = HistoryPath(folder, "parent");
  const std::string own = MailboxIdFor(path);
  ScopedEnv session("UAGENT_INTERNAL_SESSION_PATH", path.c_str());
  // From its agent, saved beside it, which names it its parent.
  const std::filesystem::path agent =
      std::filesystem::path(path).parent_path() / "agent-a.json";
  std::filesystem::create_directories(agent.parent_path());
  const json header = {
      {"format", kSessionFormat}, {"cwd", folder}, {"model", ""},
      {"session_id", ""},         {"turns", 0},    {"title", "a"}};
  std::ofstream(path) << header << "\n";
  json role = header;
  role["agent"] = {{"parent", SessionIdFor(path)}};
  std::ofstream(agent) << role << "\n";
  const Mail wake =
      MailFrom(agent.string(), own, {{"text", "the agent finished\n"}});
  CHECK(wake.from == MailboxIdFor(wake.sender_path));
  CHECK(SendMail(wake).empty());

  Api api(RuntimeConfig{});
  api.model = "test";
  std::vector<Tool> tools;
  ActivitySupervisor processes;
  UsageAccumulator usage;
  const auto denied = [](const Tool&, const json&, int64_t) {
    return std::string("denied");
  };
  std::string error;
  {
    Agent killed(api, tools, processes, usage, denied);
    CHECK(killed.DeliverMail());
    CHECK(killed.Save(path, error));
    // Gone with the process: the wake was only queued.
    CHECK(SteeringState().TakeNextAutoStart().has_value());
  }
  {
    Agent restarted(api, tools, processes, usage, denied);
    CHECK(restarted.Load(path, CanonicalCwd(), error));
    RecoverMail(own);
    CHECK(restarted.DeliverMail());
    const auto next = SteeringState().TakeNextAutoStart();
    CHECK(next && next->text == "the agent finished");
    // Its turn has the text now, so this save holds the mail for good.
    restarted.Say(next->text, "", next->origin);
    CHECK(restarted.Save(path, error));
  }
  Agent again(api, tools, processes, usage, denied);
  CHECK(again.Load(path, CanonicalCwd(), error));
  RecoverMail(own);
  CHECK(!again.DeliverMail());
  CHECK(!SteeringState().TakeNextAutoStart().has_value());
}

// Where a message came from travels with it, not with its words: the person
// typing what a queued mail says keeps their own message, and the mail its
// origin.
void TestMailKeepsItsOriginBesideTheSameWords() {
  Api api(RuntimeConfig{});
  api.model = "test";
  std::vector<Tool> tools;
  ActivitySupervisor processes;
  UsageAccumulator usage;
  const auto denied = [](const Tool&, const json&, int64_t) {
    return std::string("denied");
  };
  Agent agent(api, tools, processes, usage, denied);
  agent.Arrive("same words", "mail-1");
  agent.Say("same words", "");
  const auto next = SteeringState().TakeNextAutoStart();
  CHECK(next && next->text == "same words");
  agent.Say(next->text, "", next->origin);
  const json view = agent.DisplaySnapshot();
  std::vector<json> rows;
  for (const json& node : view.at("nodes")) {
    if (JsonValue(node, "kind", "") == "turn" ||
        JsonValue(node, "role", "") == "user") {
      rows.push_back(node);
    }
  }
  CHECK(rows.size() == 2);
  CHECK(!rows[0].contains("origin"));
  CHECK(JsonValue(rows[1], "origin", "") == "mail");
}

void TestAgentConfigAllowlist() {
  namespace fs = std::filesystem;
  CHECK(AgentConfigKey("UAGENT_MODEL"));
  CHECK(AgentConfigKey("OPENROUTER_API_KEY"));
  CHECK(!AgentConfigKey("OPENAI_API_KEY"));
  CHECK(!AgentConfigKey("UAGENT_INTERNAL_DEPTH"));

  fs::path root =
      fs::temp_directory_path() /
      ("uagent-config-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(root / ".uagent");
  ScopedEnv scoped_home("HOME", root.c_str());
  ScopedEnv scoped_key("OPENROUTER_API_KEY");
  // A value may refer to another by name.
  CHECK(
      SaveSettings({{"secret", "test-key"}, {"OPENROUTER_API_KEY", "$secret"}})
          .empty());
  ConfigManager loaded = ConfigManager::Capture({});
  (void)loaded.Initialize();
  CHECK(SettingText("OPENROUTER_API_KEY") == "test-key");
  // So may one in the environment, where a $ that names nothing stays.
  {
    ScopedEnv providers(
        "UAGENT_PROVIDERS",
        R"({"p":{"api_key":"${secret}","note":"$5 a $month"}})");
    ConfigManager from_environment = ConfigManager::Capture({});
    (void)from_environment.Initialize();
    CHECK(SettingText("UAGENT_PROVIDERS") ==
          R"({"p":{"api_key":"test-key","note":"$5 a $month"}})");
  }

  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestChildEnvironmentPolicy() {
  ScopedEnv scoped_api("UAGENT_API_KEY", "secret");
  ScopedEnv scoped_token("GITHUB_TOKEN", "secret");
  ScopedEnv scoped_passwd("DATABASE_PASSWD", "secret");
  ScopedEnv scoped_access("SERVICE_ACCESS_KEY", "secret");
  ScopedEnv scoped_private("SIGNING_PRIVATE_KEY", "secret");
  ScopedEnv scoped_cookie("SESSION_COOKIE", "secret");
  ScopedEnv scoped_usage("UAGENT_INTERNAL_USAGE_FILE", "/tmp/ledger");
  ScopedEnv scoped_providers("UAGENT_PROVIDERS", "private-provider-config");
  ScopedEnv scoped_safe("UAGENT_CHILD_ENV_SAFE", "visible");
  ScopedEnv scoped_router("OPENROUTER_API_KEY", "secret");
  ScopedEnv scoped_allow("UAGENT_SHELL_ENV_ALLOW",
                         " GITHUB_TOKEN, SSH_AUTH_SOCK, OPENROUTER_API_KEY ");

  ChildEnvironment shell;
  CHECK(!shell.Contains("UAGENT_API_KEY"));
  CHECK(!shell.Contains("GITHUB_TOKEN"));
  CHECK(!shell.Contains("DATABASE_PASSWD"));
  CHECK(!shell.Contains("SERVICE_ACCESS_KEY"));
  CHECK(!shell.Contains("SIGNING_PRIVATE_KEY"));
  CHECK(!shell.Contains("SESSION_COOKIE"));
  CHECK(!shell.Contains("UAGENT_INTERNAL_USAGE_FILE"));
  CHECK(!shell.Contains("UAGENT_PROVIDERS"));
  // A program is not told uagent's settings: a build or a test run under
  // them would behave as though the user had set them. What keeps a nested
  // uagent confined still reaches it.
  ScopedEnv scoped_depth("UAGENT_INTERNAL_DEPTH", "1");
  CHECK(!shell.Contains("UAGENT_CHILD_ENV_SAFE"));
  CHECK(ChildEnvironment().Contains("UAGENT_INTERNAL_DEPTH"));
  CHECK(shell.Contains("UAGENT_APPROVAL"));
  CHECK(ChildEnvironment({}, ChildEnvironmentPolicy::kAgent)
            .Contains("UAGENT_CHILD_ENV_SAFE"));

  CHECK(!shell.Contains("OPENROUTER_API_KEY"));
  ChildEnvironment approved({}, ChildEnvironmentPolicy::kApprovedShell);
  CHECK(!approved.Contains("UAGENT_API_KEY"));
  CHECK(approved.Contains("GITHUB_TOKEN"));
  // Named by the user, a credential of uagent's own reaches the command too.
  CHECK(approved.Contains("OPENROUTER_API_KEY"));
  CHECK(!approved.Contains("DATABASE_PASSWD"));
  CHECK(!approved.Contains("SERVICE_ACCESS_KEY"));
  CHECK(!approved.Contains("SIGNING_PRIVATE_KEY"));
  CHECK(!approved.Contains("SESSION_COOKIE"));
  CHECK(!approved.Contains("UAGENT_INTERNAL_USAGE_FILE"));
  CHECK(!approved.Contains("UAGENT_PROVIDERS"));

  // sandbox.hideEnv adds names, matched without regard to case, and wins over
  // shell.envAllow. Overrides a caller sets itself are not filtered.
  ScopedEnv scoped_hidden_env("UAGENT_SANDBOX_HIDE_ENV",
                              " Internal_Url , GITHUB_TOKEN ");
  ScopedEnv scoped_internal("INTERNAL_URL", "https://corp.example");
  ChildEnvironment hiding({}, ChildEnvironmentPolicy::kApprovedShell);
  CHECK(!hiding.Contains("INTERNAL_URL"));
  CHECK(!hiding.Contains("GITHUB_TOKEN"));
  CHECK(!ChildEnvironment({}, ChildEnvironmentPolicy::kAgent)
             .Contains("INTERNAL_URL"));
  CHECK(ChildEnvironment({{"INTERNAL_URL", "explicit"}})
            .Contains("INTERNAL_URL"));

  ChildEnvironment delegated({{"UAGENT_API_KEY", "explicit"},
                              {"UAGENT_INTERNAL_USAGE_FILE", "/tmp/child"}});
  CHECK(delegated.Contains("UAGENT_API_KEY"));
  CHECK(delegated.Contains("UAGENT_INTERNAL_USAGE_FILE"));

  Route route;
  route.base_url = "https://child.example/v1";
  route.api_key = "child-key";
  route.model = "child-model";
  route.ctx_window = 32768;
  route.reasoning_effort = "high";
  route.wire_api = WireApi::kResponses;
  route.features = FeaturesOf({Feature::kHostedSearch, Feature::kPdfParser});
  route.options = {{"pdf_engine", "mistral-ocr"}};
  route.hosted_tools = json::array({{{"type", "code_interpreter"}}});
  EnvironmentOverrides child_overrides =
      ChildAgentEnvironment(route, "parent-id");
  auto child_value = [&](std::string_view key) {
    auto found =
        std::find_if(child_overrides.begin(), child_overrides.end(),
                     [&](const auto& entry) { return entry.first == key; });
    return found == child_overrides.end() ? std::string() : found->second;
  };
  CHECK(child_value("UAGENT_BASE_URL") == "https://child.example/v1");
  CHECK(child_value("UAGENT_API_KEY") == "child-key");
  // A child reports its spend to its parent session's ledger.
  CHECK(child_value("UAGENT_INTERNAL_USAGE_FILE") == UsageLedger("parent-id"));
  CHECK(child_value("UAGENT_MODEL") == "child-model");
  CHECK(child_value("UAGENT_CONTEXT") == "32768");
  CHECK(child_value("UAGENT_REASONING_EFFORT") == "high");
  CHECK(child_value("UAGENT_INTERNAL_WIRE_API") == "responses");
  CHECK(child_value("UAGENT_INTERNAL_FEATURES") == "hosted_search,pdf_parser");
  CHECK(child_value("UAGENT_INTERNAL_ENDPOINT_OPTIONS") ==
        R"({"pdf_engine":"mistral-ocr"})");
  CHECK(child_value("UAGENT_INTERNAL_HOSTED_TOOLS") ==
        R"([{"type":"code_interpreter"}])");
  ChildEnvironment child(child_overrides);
  CHECK(!child.Contains("UAGENT_PROVIDERS"));
  CHECK(child.Contains("UAGENT_API_KEY"));
}

void TestKnownEndpoints() {
  Endpoint openrouter;
  openrouter.base_url = "https://openrouter.ai/api/v1";
  CHECK(DeclareEndpoint(openrouter, {}));
  CHECK(openrouter.features.Has(Feature::kPromptCacheControl));
  CHECK(openrouter.features.Has(Feature::kSearchPlugin));
  CHECK(openrouter.Option("pdf_engine") == "cloudflare-ai");
  Endpoint edge;
  edge.base_url = "https://edge.openrouter.ai/v1";
  CHECK(DeclareEndpoint(edge, {}) && edge.features == openrouter.features);
  Endpoint openai;
  openai.base_url = "https://api.openai.com/v1";
  CHECK(DeclareEndpoint(openai, {}));
  CHECK(openai.features.Has(Feature::kMaxCompletionTokens));
  CHECK(!openai.features.Has(Feature::kSearchPlugin));
  Endpoint unknown;
  unknown.base_url = "https://provider.test/v1";
  CHECK(!DeclareEndpoint(unknown, {}));
  CHECK(unknown.features == FeatureSet{});
  // A provider entry that has the address says what the endpoint is.
  Endpoint configured;
  configured.name = "gateway";
  configured.base_url = "https://provider.test/v1";
  configured.features = FeaturesOf({Feature::kReasoningObject});
  configured.options = {{"provider", "fast"}};
  CHECK(DeclareEndpoint(unknown, {configured}));
  CHECK(unknown.name == "gateway");
  CHECK(unknown.features.Has(Feature::kReasoningObject));
  CHECK(unknown.Option("provider") == "fast");

  // An endpoint is available when its key is set; it names no model.
  ScopedEnv no_providers("UAGENT_PROVIDERS");
  ScopedEnv scoped_key("OPENROUTER_API_KEY");
  CHECK(DefaultEndpoint(SessionProviderCatalog().providers) == nullptr);
  setenv("OPENROUTER_API_KEY", "test-key", 1);
  ProviderCatalog catalog = SessionProviderCatalog();
  const Endpoint* available = DefaultEndpoint(catalog.providers);
  REQUIRE(available != nullptr);
  CHECK(available->name == "openrouter");
  CHECK(available->api_key == "test-key");
  CHECK(available->base_url == openrouter.base_url);
  CHECK(available->features == openrouter.features);
  // A provider entry named like a known endpoint states what differs.
  setenv("UAGENT_PROVIDERS",
         R"json({"openrouter":{"options":{"provider":"streamlake"}}})json", 1);
  catalog = SessionProviderCatalog();
  REQUIRE(catalog.providers.size() == 1);
  CHECK(catalog.providers[0].base_url == openrouter.base_url);
  CHECK(catalog.providers[0].api_key == "test-key");
  CHECK(catalog.providers[0].Option("provider") == "streamlake");
  CHECK(catalog.providers[0].Option("pdf_engine") == "cloudflare-ai");
  unsetenv("UAGENT_PROVIDERS");
}

void TestNamedProviders() {
  // Route variables keep their inherited value until the body changes them,
  // and every one of them is restored when this scope ends.
  ScopedEnv scoped_base_url("UAGENT_BASE_URL", std::getenv("UAGENT_BASE_URL"));
  ScopedEnv scoped_model("UAGENT_MODEL", std::getenv("UAGENT_MODEL"));
  ScopedEnv scoped_effort("UAGENT_REASONING_EFFORT",
                          std::getenv("UAGENT_REASONING_EFFORT"));
  ScopedEnv scoped_context("UAGENT_CONTEXT", std::getenv("UAGENT_CONTEXT"));
  ScopedEnv scoped_features("UAGENT_INTERNAL_FEATURES",
                            std::getenv("UAGENT_INTERNAL_FEATURES"));
  ScopedEnv scoped_wire("UAGENT_INTERNAL_WIRE_API",
                        std::getenv("UAGENT_INTERNAL_WIRE_API"));
  ScopedEnv scoped_providers("UAGENT_PROVIDERS",
                             std::getenv("UAGENT_PROVIDERS"));
  ScopedEnv scoped_openrouter("OPENROUTER_API_KEY",
                              std::getenv("OPENROUTER_API_KEY"));
  json configured = {
      {"", {{"base_url", "https://empty.test/v1"}}},
      {"all", {{"base_url", "https://reserved.test/v1"}}},
      {"bad/name", {{"base_url", "https://invalid.test/v1"}}},
      {"codex-local",
       {{"base_url", "http://127.0.0.1:8787/api/v1/"},
        {"api_key", "local-key"},
        {"supports", json::array({"raw_vendor_ids", "reasoning_object"})},
        {"context", 16384}}},
      {"static",
       {{"base_url", "https://static.test/v1"},
        {"wire_api", "responses"},
        {"hosted_tools", json::array({"web_search",
                                      {{"type", "code_interpreter"},
                                       {"container", {{"type", "auto"}}}},
                                      {{"name", "no type"}}})},
        {"models",
         {{"fast", "actual-model"},
          {"plain",
           {{"id", "plain-model"}, {"hosted_tools", json::array()}}}}}}},
      {"bad-wire",
       {{"base_url", "https://bad-wire.test/v1"},
        {"wire_api", "model-name-is-not-a-wire-api"}}},
      {"bad-feature",
       {{"base_url", "https://bad-feature.test/v1"},
        {"supports", json::array({"model-name-is-not-a-feature"})}}},
  };
  setenv("UAGENT_PROVIDERS", JsonDump(configured).c_str(), 1);
  ProviderCatalog catalog = LoadProviderCatalog();
  CHECK(catalog.providers.size() == 2);
  CHECK(catalog.models.size() == 2);
  const Endpoint* codex = FindEndpoint(catalog.providers, "codex-local");
  CHECK(codex != nullptr);
  CHECK(codex && codex->base_url == "http://127.0.0.1:8787/api/v1");
  CHECK(codex && codex->api_key == "local-key");
  CHECK(codex && codex->context == 16384);
  CHECK(codex && codex->features.Has(Feature::kRawVendorIds));
  CHECK(codex && codex->wire_api == WireApi::kChatCompletions);
  CHECK(codex && !codex->features.Has(Feature::kHostedSearch));
  const Endpoint* native = FindEndpoint(catalog.providers, "static");
  CHECK(native && native->wire_api == WireApi::kResponses);
  CHECK(native && native->features.Has(Feature::kHostedSearch));
  // An object is a tool the endpoint runs, kept as written; one with no
  // type is no tool. A model's own list replaces the provider's.
  CHECK(native && native->hosted_tools ==
                      json::array({{{"type", "code_interpreter"},
                                    {"container", {{"type", "auto"}}}}}));
  for (const Route& model : catalog.models) {
    if (model.route_name == "static/fast") {
      CHECK(model.hosted_tools.size() == 1);
    }
    if (model.route_name == "static/plain") {
      CHECK(model.hosted_tools.empty());
      CHECK(!model.features.Has(Feature::kHostedSearch));
    }
  }

  // Startup consumes the same selection grammar as interactive and side
  // routes. Suffixes configure policy and never leak into the provider model
  // identifier.
  setenv("UAGENT_MODEL", "codex-local/gpt-5.6-luna:low", 1);
  setenv("UAGENT_INTERNAL_WIRE_API", "invalid-direct-wire", 1);
  setenv("UAGENT_INTERNAL_FEATURES", "invalid-direct-feature", 1);
  unsetenv("UAGENT_BASE_URL");
  unsetenv("UAGENT_REASONING_EFFORT");
  RuntimeConfig startup_config;
  Api startup(startup_config);
  ProviderSetup startup_setup = ConfigureProvider(startup);
  CHECK(startup_setup.warning.empty());
  CHECK(startup.base_url == "http://127.0.0.1:8787/api/v1");
  CHECK(startup.api_key == "local-key");
  CHECK(startup.model == "gpt-5.6-luna");
  CHECK(startup.reasoning_effort == "low");
  CHECK(SelectModel(startup, catalog.models, catalog.providers,
                    "codex-local/gpt-5.6-sol:high") ==
        "codex-local/gpt-5.6-sol:high");
  CHECK(startup.model == "gpt-5.6-sol");
  CHECK(startup.reasoning_effort == "high");

  // Effort-only selection retains a discovered window, but an explicit
  // configured window wins over the previous value.
  auto unknown_window = catalog.providers;
  for (auto& provider : unknown_window) provider.context = 0;
  startup.ctx_window = 1300000;
  CHECK(!SelectModel(startup, {}, unknown_window, "codex-local/gpt-5.6-sol:low")
             .empty());
  CHECK(startup.ctx_window == 1300000);
  CHECK(!SelectModel(startup, {}, catalog.providers,
                     "codex-local/gpt-5.6-sol:high")
             .empty());
  CHECK(startup.ctx_window == 16384);

  // [provider/]model[:effort] — an effort peels from the right; any other
  // suffix stays in the model id.
  ModelSelection plain = ParseModelSelection("gpt-5.5");
  CHECK(plain.base == "gpt-5.5");
  CHECK(plain.effort.empty());
  ModelSelection scoped = ParseModelSelection("openrouter/deepseek/v4:xhigh");
  CHECK(scoped.base == "openrouter/deepseek/v4");
  CHECK(scoped.effort == "xhigh");
  // A routing suffix is not an effort: it stays in the id, and it stops the
  // peel.
  ModelSelection suffixed = ParseModelSelection("openrouter/v4:nitro:xhigh");
  CHECK(suffixed.base == "openrouter/v4:nitro" && suffixed.effort == "xhigh");
  ModelSelection trailing_suffix =
      ParseModelSelection("openrouter/v4:xhigh:nitro");
  CHECK(trailing_suffix.base == "openrouter/v4:xhigh:nitro");
  CHECK(trailing_suffix.effort.empty());
  ModelSelection free_id = ParseModelSelection("openrouter/deepseek-chat:free");
  CHECK(free_id.base == "openrouter/deepseek-chat:free");
  CHECK(free_id.effort.empty());
  ModelSelection free_effort =
      ParseModelSelection("openrouter/deepseek-chat:free:high");
  CHECK(free_effort.base == "openrouter/deepseek-chat:free");
  CHECK(free_effort.effort == "high");
  // A second effort is not consumed twice, and a bare colon is left alone.
  ModelSelection repeated = ParseModelSelection("model:low:high");
  CHECK(repeated.base == "model:low" && repeated.effort == "high");
  ModelSelection trailing = ParseModelSelection("model:");
  CHECK(trailing.base == "model:" && trailing.effort.empty());
  CHECK(ParseModelSelection("  spaced/model:high  ").base == "spaced/model");

  std::optional<Route> dynamic = ResolveModelRoute(
      catalog.models, catalog.providers, "codex-local/org/model");
  REQUIRE(dynamic.has_value());
  CHECK(dynamic->model == "org/model");
  CHECK(dynamic->ctx_window == 16384);
  CHECK(dynamic->features.Has(Feature::kRawVendorIds));
  RuntimeConfig config;
  Api routed(config);
  routed.reasoning_effort = "medium";
  ApplyRoute(routed, *dynamic);
  CHECK(routed.reasoning_effort.empty());
  CHECK(routed.features.Has(Feature::kRawVendorIds));
  Route fixed_effort = *dynamic;
  fixed_effort.reasoning_effort = "low";
  fixed_effort.supported_reasoning_efforts = {"low", "high"};
  ApplyRoute(routed, fixed_effort);
  CHECK(routed.reasoning_effort == "low");
  CHECK(SupportsReasoningEffort(routed, "high"));
  CHECK(!SupportsReasoningEffort(routed, "medium"));
  ActivateRoute(routed);
  CHECK(ContainsCaseInsensitive("codex-local/gpt-5.6-sol", "GPT-5.6"));
  CHECK(ContainsCaseInsensitive("openrouter/deepseek/v4", "openrouter"));
  CHECK(!ContainsCaseInsensitive("codex-local/gpt-5.6-sol", "deepseek"));
  CHECK(NormalizeModelQuery("all").empty());
  CHECK(NormalizeModelQuery("*").empty());
  CHECK(NormalizeModelQuery(" codex-local/* ") == "codex-local/");
  CHECK(NormalizeModelQuery("gpt-5.6") == "gpt-5.6");
  Api openrouter_api(config);
  openrouter_api.base_url = "https://openrouter.ai/api/v1";
  UseOpenRouterDialect(openrouter_api);
  openrouter_api.model = "parent-model";
  openrouter_api.reasoning_effort = "high";
  CHECK(RouteSelection(openrouter_api, {}) == "openrouter/parent-model:high");
  openrouter_api.reasoning_effort.clear();
  CHECK(CanUseRawModel(openrouter_api, "stepfun/step-3.7-flash"));
  openrouter_api.base_url = "http://127.0.0.1:8787/api/v1";
  CHECK(CanUseRawModel(openrouter_api, "stepfun/step-3.7-flash"));
  UseEndpoint(openrouter_api, openrouter_api.base_url);
  CHECK(!CanUseRawModel(openrouter_api, "stepfun/step-3.7-flash"));
  std::optional<Route> fixed =
      ResolveModelRoute(catalog.models, catalog.providers, "static/fast");
  REQUIRE(fixed.has_value());
  CHECK(fixed->model == "actual-model");
  CHECK(fixed->wire_api == WireApi::kResponses);
  CHECK(fixed->features.Has(Feature::kHostedSearch));
  CHECK(!ResolveModelRoute(catalog.models, catalog.providers, "missing/model"));
  CHECK(!ResolveModelRoute(catalog.models, catalog.providers, "codex-local/"));
  CHECK(!ResolveModelRoute(catalog.models, catalog.providers, "codex-local"));
  Api no_catalog(RuntimeConfig{});
  const auto alias = [](const char* name, const char* effort) {
    Route route;
    route.base_url = "https://static.test/v1";
    route.api_key = "key";
    route.route_name = name;
    route.model = "same-model";
    route.reasoning_effort = effort;
    return route;
  };
  std::vector<Route> aliases = {alias("static/low", "low"),
                                alias("static/high", "high")};
  ModelSearch alias_search = SearchModels(no_catalog, aliases, {}, "static");
  CHECK(alias_search.matches.size() == 2);

  setenv("OPENROUTER_API_KEY", "openrouter-key", 1);
  catalog = SessionProviderCatalog();
  const Endpoint* openrouter = FindEndpoint(catalog.providers, "openrouter");
  CHECK(openrouter != nullptr);
  CHECK(openrouter && openrouter->base_url == "https://openrouter.ai/api/v1");
  CHECK(openrouter && openrouter->api_key == "openrouter-key");
  CHECK(catalog.providers.size() == 3);

  setenv("UAGENT_BASE_URL", "https://direct.test/v1", 1);
  setenv("UAGENT_MODEL", "direct-model", 1);
  setenv("UAGENT_INTERNAL_WIRE_API", "invalid-direct-wire", 1);
  setenv("UAGENT_INTERNAL_FEATURES", "invalid-direct-feature", 1);
  Api invalid_direct(RuntimeConfig{});
  ProviderSetup invalid_setup = ConfigureProvider(invalid_direct);
  CHECK(invalid_direct.base_url.empty());
  CHECK(invalid_setup.warning.find("invalid wire API") != std::string::npos);
  CHECK(invalid_setup.warning.find("invalid endpoint feature") !=
        std::string::npos);
}

void TestEffectiveConfigReload() {
  TestWorkspace workspace("effective-config");
  ScopedEnv scoped_steps("UAGENT_MAX_STEPS", "9");
  ScopedEnv scoped_model("UAGENT_MODEL");
  ScopedEnv scoped_route_key("OPENROUTER_API_KEY");
  ScopedEnv scoped_base_url("UAGENT_BASE_URL");
  ScopedEnv scoped_toolset("UAGENT_INTERNAL_TOOLSET");
  CHECK(
      SaveSettings({{"UAGENT_MAX_STEPS", "4"},
                    {"UAGENT_INTERNAL_TOOLSET", "lean"},
                    {"UAGENT_MAX_TOOL_CALLS", "2"},
                    {"UAGENT_MAX_TURN_TOKENS", "100"},
                    {"UAGENT_SESSION_TOKEN_BUDGET", "200"},
                    {"UAGENT_MCP_TIMEOUT", "9"},
                    {"UAGENT_MODEL", "initial-model"},
                    {"UAGENT_SESSION_BUDGET", "1"},
                    {"UAGENT_BASE_URL", "https://user:pass@review.example/v1"},
                    {"route_secret", "private-route-key"},
                    {"OPENROUTER_API_KEY", "$route_secret"}})
          .empty());
  // The CLI layer outranks the environment and what is saved.
  ConfigManager manager = ConfigManager::Capture(
      {{"UAGENT_SESSION_BUDGET", "3.5"}, {"UAGENT_MEMORY", "0"}});
  RuntimeConfig active = manager.Initialize();
  CHECK(active.max_steps == 9);
  CHECK(active.max_tool_calls == 2);
  CHECK(active.max_turn_tokens == 100);
  CHECK(active.session_token_budget == 200);
  CHECK(active.mcp_timeout_s == 9);
  CHECK(active.session_budget == 3.5);
  CHECK(!active.memory_enabled);
  json diagnostic = manager.DiagnosticJson(active);
  CHECK(diagnostic["sources"]["UAGENT_MAX_STEPS"] == "environment");
  CHECK(diagnostic["sources"]["UAGENT_SESSION_BUDGET"] == "cli");
  // A parent's handoff to its child is not a setting a file may carry.
  CHECK(!diagnostic["sources"].contains("UAGENT_INTERNAL_TOOLSET"));
  CHECK(diagnostic["sources"]["UAGENT_MAX_TOOL_CALLS"] == "user");
  CHECK(!diagnostic["sources"].contains("UAGENT_REQUEST_TIMEOUT"));
  std::string shown = JsonDump(diagnostic);
  CHECK(shown.find("private-route-key") == std::string::npos);
  CHECK(shown.find("user:pass") == std::string::npos);

  CHECK(ChangeSettings("", [](SettingValues& all) {
          all["UAGENT_MAX_TOOL_CALLS"] = "7";
          all["UAGENT_MAX_TURN_TOKENS"] = "150";
          all["UAGENT_SESSION_TOKEN_BUDGET"] = "250";
          all["UAGENT_MCP_TIMEOUT"] = "10";
          all["UAGENT_TOOL_RESULT_CHARS"] = "1234";
          all["UAGENT_MODEL"] = "next-model";
          all["OPENROUTER_API_KEY"] = "changed-secret";
          return std::string();
        }).empty());
  std::optional<ConfigReload> reload = manager.Reload(active);
  REQUIRE(reload.has_value());
  CHECK(reload->active.max_steps == 9);
  CHECK(reload->active.max_tool_calls == 7);
  CHECK(reload->active.max_turn_tokens == 150);
  CHECK(reload->active.session_token_budget == 250);
  CHECK(reload->active.mcp_timeout_s == 9);
  CHECK(std::find(reload->applied.begin(), reload->applied.end(),
                  "max_tool_calls") != reload->applied.end());
  CHECK(std::find(reload->deferred.begin(), reload->deferred.end(),
                  "mcp_timeout_s") != reload->deferred.end());
  CHECK(std::find(reload->deferred.begin(), reload->deferred.end(),
                  "UAGENT_MODEL") != reload->deferred.end());
  CHECK(std::find(reload->deferred.begin(), reload->deferred.end(),
                  "OPENROUTER_API_KEY") != reload->deferred.end());
  CHECK(
      JsonDump(manager.DiagnosticJson(reload->active)).find("changed-secret") ==
      std::string::npos);
  // A setting read on use applies with the reload: environ is untouched and
  // a child started afterwards is told the new value.
  CHECK(std::find(reload->applied.begin(), reload->applied.end(),
                  "UAGENT_TOOL_RESULT_CHARS") != reload->applied.end());
  CHECK(ToolResultCap() == 1234);
  CHECK(getenv("UAGENT_TOOL_RESULT_CHARS") == nullptr);
  ChildEnvironment child({}, ChildEnvironmentPolicy::kAgent);
  bool told = false;
  for (char** entry = child.Data(); entry && *entry; ++entry) {
    told |= std::string_view(*entry) == "UAGENT_TOOL_RESULT_CHARS=1234";
  }
  CHECK(told);
  // Put back to its default, it is at its default: nothing saved was ever
  // left in the environment to come back from.
  CHECK(ChangeSettings("", [](SettingValues& all) {
          all.erase("UAGENT_TOOL_RESULT_CHARS");
          return std::string();
        }).empty());
  CHECK(manager.Reload(reload->active).has_value());
  CHECK(ToolResultCap() != 1234);
  OverrideSetting("UAGENT_TOOL_RESULT_CHARS", "99");
  CHECK(ToolResultCap() == 99);
}

}  // namespace uagent
