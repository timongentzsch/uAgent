// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "include/api/retry.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestRuntimeOwnershipHelpers() {
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

  UsageAccumulator accumulator;
  accumulator.Add(json{{"prompt_tokens", 5},
                       {"completion_tokens", 2},
                       {"prompt_tokens_details", {{"cache_write_tokens", 2}}}});
  accumulator.Add(Usage{3, 4, 1, 0, 0.25});
  Usage total = accumulator.Take();
  CHECK(total.input == 8);
  CHECK(total.output == 6);
  CHECK(total.cache_read == 1);
  CHECK(total.cache_write == 2);
  CHECK(total.cost == 0.25);
  CHECK(accumulator.Take().input == 0);

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

  setenv("UAGENT_MAX_STEPS", "0", 1);
  setenv("UAGENT_MAX_TOOL_CALLS", "0", 1);
  setenv("UAGENT_TEST_LONG", "999999999999999999999999999999", 1);
  setenv("UAGENT_SESSION_BUDGET", "2.5", 1);
  setenv("UAGENT_MAX_TURN_COST", "nan", 1);
  setenv("UAGENT_TASK_MODEL", "fast/model", 1);
  setenv("UAGENT_TOOL_TRACE_PROTECT_CHARS", "1234", 1);
  setenv("UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS", "5678", 1);
  setenv("UAGENT_SESSION_ARCHIVE_BYTES", "-1", 1);
  setenv("UAGENT_WEB_SEARCH_MODEL", "vendor/search", 1);
  setenv("UAGENT_WEB_SEARCH_BACKEND", "responses", 1);
  setenv("UAGENT_WEB_SEARCH_URL", "https://search.example/v1", 1);
  setenv("UAGENT_WEB_SEARCH_API_KEY", "secret-search-key", 1);
  setenv("UAGENT_WEB_SEARCH_EFFORT", "low", 1);
  setenv("UAGENT_WEB_SEARCH_ENGINE", "invalid", 1);
  setenv("UAGENT_WEB_SEARCH_CONTEXT_SIZE", "huge", 1);
  setenv("UAGENT_WEB_SEARCH_MAX_RESULTS", "99", 1);
  setenv("UAGENT_WEB_SEARCH_MAX_USES", "0", 1);
  setenv("UAGENT_MCP_ROOTS", "/tmp/one:/tmp/two", 1);
  setenv("UAGENT_OPENROUTER_VARIANT", "floor", 1);
  RuntimeConfig config = RuntimeConfig::FromEnvironment();
  CHECK(config.max_steps == 0);
  CHECK(config.max_tool_calls == 0);
  CHECK(EnvLong("UAGENT_TEST_LONG", 7) == 7);
  CHECK(config.session_budget == 2.5);
  CHECK(config.max_turn_cost == 0);
  CHECK(TaskModel() == "fast/model");
  CHECK(ToolTraceProtectChars() == 1234);
  CHECK(ToolTracePruneMinChars() == 5678);
  CHECK(config.session_archive_bytes == 0);
  CHECK(config.web_search_model == "vendor/search");
  CHECK(config.web_search_backend == "responses");
  CHECK(config.web_search_url == "https://search.example/v1");
  CHECK(config.web_search_api_key == "secret-search-key");
  CHECK(config.web_search_effort == "low");
  CHECK(config.web_search_engine == "auto");
  CHECK(config.web_search_context_size.empty());
  CHECK(config.web_search_max_results == 25);
  CHECK(config.web_search_max_uses == 1);
  CHECK(config.mcp_roots == "/tmp/one:/tmp/two");
  CHECK(config.openrouter_variant == "floor");
  json diagnostics = config.DiagnosticJson();
  CHECK(diagnostics.value("max_steps", int64_t{0}) == config.max_steps);
  CHECK(diagnostics.value("web_search_model", "") == config.web_search_model);
  CHECK(diagnostics.value("web_search_backend", "") ==
        config.web_search_backend);
  CHECK(diagnostics.value("openrouter_variant", "") == "floor");
  CHECK(diagnostics.value("mcp_roots", "") == config.mcp_roots);
  CHECK(diagnostics.value("tool_trace_protect_chars", int64_t{0}) == 1234);
  CHECK(diagnostics.value("tool_trace_prune_min_chars", int64_t{0}) == 5678);
  CHECK(diagnostics.value("web_search_api_key", "") == "<set>");
  CHECK(JsonDump(diagnostics).find("secret-search-key") == std::string::npos);
  unsetenv("UAGENT_MAX_STEPS");
  unsetenv("UAGENT_MAX_TOOL_CALLS");
  unsetenv("UAGENT_TEST_LONG");
  unsetenv("UAGENT_SESSION_BUDGET");
  unsetenv("UAGENT_MAX_TURN_COST");
  unsetenv("UAGENT_TASK_MODEL");
  unsetenv("UAGENT_TOOL_TRACE_PROTECT_CHARS");
  unsetenv("UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS");
  unsetenv("UAGENT_MCP_ROOTS");
  unsetenv("UAGENT_SESSION_ARCHIVE_BYTES");
  unsetenv("UAGENT_WEB_SEARCH_MODEL");
  unsetenv("UAGENT_WEB_SEARCH_BACKEND");
  unsetenv("UAGENT_WEB_SEARCH_URL");
  unsetenv("UAGENT_WEB_SEARCH_API_KEY");
  unsetenv("UAGENT_WEB_SEARCH_EFFORT");
  unsetenv("UAGENT_WEB_SEARCH_ENGINE");
  unsetenv("UAGENT_WEB_SEARCH_CONTEXT_SIZE");
  unsetenv("UAGENT_WEB_SEARCH_MAX_RESULTS");
  unsetenv("UAGENT_WEB_SEARCH_MAX_USES");
  setenv("UAGENT_OPENROUTER_VARIANT", "invalid", 1);
  CHECK(RuntimeConfig::FromEnvironment().openrouter_variant.empty());
  unsetenv("UAGENT_OPENROUTER_VARIANT");
  RuntimeConfig defaults;
  CHECK(defaults.max_steps == 0);
  CHECK(defaults.max_tool_calls == 0);
  CHECK(defaults.max_turn_cost == 0);
  CHECK(AutoCompactTokens() == 0);
  CHECK(defaults.max_turn_seconds == 0);
  CHECK(defaults.first_event_timeout_s == 300);
  CHECK(defaults.stream_idle_timeout_s == 300);
  CHECK(defaults.request_timeout_s == 600);

  RuntimeConfig routed;
  routed.openrouter_provider = "streamlake";
  routed.openrouter_fallbacks = true;
  Api api(routed);
  api.base_url = "https://openrouter.ai/api/v1";
  api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenRouter, api.base_url);
  api.model = "test";
  json body = api.BuildChatBody(json::array(), json::array(), "stable-session");
  CHECK(body.value("session_id", "") == "stable-session");
  CHECK(body["provider"]["order"][0] == "streamlake");
  CHECK(body["provider"].value("allow_fallbacks", false));
  CHECK(!body.contains("stream_options"));
  api.config.openrouter_variant = "nitro";
  body = api.BuildChatBody(json::array(), json::array(), "stable-session");
  CHECK(body.value("model", "") == "test:nitro");
  api.model = "test:exacto";
  api.config.openrouter_variant = "floor";
  body = api.BuildChatBody(json::array(), json::array(), "stable-session");
  CHECK(body.value("model", "") == "test:floor");
  api.model = "test:free";
  api.config.openrouter_variant = "exacto";
  body = api.BuildChatBody(json::array(), json::array(), "stable-session");
  CHECK(body.value("model", "") == "test:free:exacto");
  api.reasoning_effort = "low";
  body = api.BuildChatBody(json::array(), json::array(), "stable-session");
  CHECK(body["reasoning"].value("effort", "") == "low");
  CHECK(!body.contains("reasoning_effort"));
  api.base_url = "http://127.0.0.1:8080/v1";
  api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenAi, api.base_url);
  body = api.BuildChatBody(json::array(), json::array(), "stable-session");
  CHECK(body.value("model", "") == "test:free");
  CHECK(!body.contains("session_id"));
  CHECK(!body.contains("provider"));

  api.base_url = "https://api.openai.com/v1";
  api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenAi, api.base_url);
  api.reasoning_effort = "high";
  body = api.BuildChatBody(json::array(), json::array());
  CHECK(body.value("reasoning_effort", "") == "high");
  CHECK(body.contains("max_completion_tokens"));
  CHECK(!body.contains("max_tokens"));
  CHECK(body["stream_options"].value("include_usage", false));

  json assistant = {{"role", "assistant"}, {"content", ""}};
  ChatResult reasoning_result;
  reasoning_result.reasoning = "reasoning state";
  reasoning_result.reasoning_field = true;
  reasoning_result.reasoning_details =
      json::array({{{"type", "reasoning.text"},
                    {"index", 0},
                    {"text", "reasoning state"}}});
  api.base_url = "https://openrouter.ai/api/v1";
  api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenRouter, api.base_url);
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
  api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenAi, api.base_url);
  json generic_assistant = {{"role", "assistant"}, {"content", ""}};
  api.PreserveAssistantReasoning(generic_assistant, reasoning_result);
  CHECK(!generic_assistant.contains("reasoning"));
  CHECK(!generic_assistant.contains("reasoning_details"));
  CHECK(!generic_assistant.contains("reasoning_content"));
  reasoning_result.reasoning_content_field = true;
  json deepseek_assistant = {{"role", "assistant"}, {"content", ""}};
  api.PreserveAssistantReasoning(deepseek_assistant, reasoning_result);
  CHECK(deepseek_assistant.value("reasoning_content", "") == "reasoning state");

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

  ProviderCapabilities generic = CapabilitiesForRoute(
      ProviderProtocol::kOpenAi, "http://127.0.0.1:8080/v1");
  ProviderCapabilities router = CapabilitiesForRoute(
      ProviderProtocol::kOpenRouter, "http://127.0.0.1:8080/v1");
  CHECK(generic.stream_usage_option);
  CHECK(!generic.raw_slash_models);
  CHECK(!router.stream_usage_option);
  CHECK(router.raw_slash_models);
  CHECK(router.reasoning_object);
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
}

void TestAgentConfigAllowlist() {
  namespace fs = std::filesystem;
  CHECK(AgentConfigKey("UAGENT_MODEL"));
  CHECK(AgentConfigKey("OPENROUTER_API_KEY"));
  CHECK(AgentConfigKey("OPENROUTER_MODEL"));
  CHECK(AgentConfigKey("OPENROUTER_EFFORT"));
  CHECK(!AgentConfigKey("OPENAI_API_KEY"));

  fs::path root =
      fs::temp_directory_path() /
      ("uagent-config-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(root / ".uagent");
  CHECK(ToolWriteFile((root / ".uagent/.config").string(),
                      "secret=test-key\n"
                      "OPENROUTER_API_KEY=$secret\n"
                      "OPENROUTER_MODEL=vendor/model\n"
                      "OPENROUTER_EFFORT=high\n")
            .output.starts_with("wrote "));

  const char* inherited_home = getenv("HOME");
  const char* inherited_config = getenv("UAGENT_CONFIG_FILE");
  std::string prior_home = inherited_home ? inherited_home : "";
  std::string prior_config = inherited_config ? inherited_config : "";
  setenv("HOME", root.c_str(), 1);
  unsetenv("UAGENT_CONFIG_FILE");
  unsetenv("OPENROUTER_API_KEY");
  unsetenv("OPENROUTER_MODEL");
  unsetenv("OPENROUTER_EFFORT");
  ConfigManager loaded = ConfigManager::Capture(
      /*trust_project=*/false, /*cli_budget=*/-1,
      /*cli_no_memory=*/false);
  (void)loaded.Initialize();
  CHECK(EnvStr("OPENROUTER_API_KEY") == "test-key");
  CHECK(EnvStr("OPENROUTER_MODEL") == "vendor/model");
  CHECK(EnvStr("OPENROUTER_EFFORT") == "high");

  unsetenv("OPENROUTER_API_KEY");
  unsetenv("OPENROUTER_MODEL");
  unsetenv("OPENROUTER_EFFORT");
  if (inherited_home) {
    setenv("HOME", prior_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  if (inherited_config) {
    setenv("UAGENT_CONFIG_FILE", prior_config.c_str(), 1);
  } else {
    unsetenv("UAGENT_CONFIG_FILE");
  }
  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestChildEnvironmentPolicy() {
  auto prior = [](const char* key) -> std::optional<std::string> {
    const char* value = getenv(key);
    return value ? std::optional<std::string>(value) : std::nullopt;
  };
  const auto prior_api = prior("UAGENT_API_KEY");
  const auto prior_token = prior("GITHUB_TOKEN");
  const auto prior_usage = prior("UAGENT_USAGE_FILE");
  const auto prior_safe = prior("UAGENT_CHILD_ENV_SAFE");
  const auto prior_allow = prior("UAGENT_SHELL_ENV_ALLOW");
  setenv("UAGENT_API_KEY", "secret", 1);
  setenv("GITHUB_TOKEN", "secret", 1);
  setenv("UAGENT_USAGE_FILE", "/tmp/ledger", 1);
  setenv("UAGENT_CHILD_ENV_SAFE", "visible", 1);
  setenv("UAGENT_SHELL_ENV_ALLOW", " GITHUB_TOKEN, SSH_AUTH_SOCK ", 1);

  ChildEnvironment shell;
  CHECK(!shell.Contains("UAGENT_API_KEY"));
  CHECK(!shell.Contains("GITHUB_TOKEN"));
  CHECK(!shell.Contains("UAGENT_USAGE_FILE"));
  CHECK(shell.Contains("UAGENT_CHILD_ENV_SAFE"));

  ChildEnvironment approved({}, ChildEnvironmentPolicy::kApprovedShell);
  CHECK(!approved.Contains("UAGENT_API_KEY"));
  CHECK(approved.Contains("GITHUB_TOKEN"));
  CHECK(!approved.Contains("UAGENT_USAGE_FILE"));

  ChildEnvironment delegated(
      {{"UAGENT_API_KEY", "explicit"}, {"UAGENT_USAGE_FILE", "/tmp/child"}});
  CHECK(delegated.Contains("UAGENT_API_KEY"));
  CHECK(delegated.Contains("UAGENT_USAGE_FILE"));

  auto restore = [](const char* key, const std::optional<std::string>& value) {
    if (value) {
      setenv(key, value->c_str(), 1);
    } else {
      unsetenv(key);
    }
  };
  restore("UAGENT_API_KEY", prior_api);
  restore("GITHUB_TOKEN", prior_token);
  restore("UAGENT_USAGE_FILE", prior_usage);
  restore("UAGENT_CHILD_ENV_SAFE", prior_safe);
  restore("UAGENT_SHELL_ENV_ALLOW", prior_allow);
}

void TestModelPreference() {
  namespace fs = std::filesystem;
  fs::path root = fs::temp_directory_path() /
                  ("uagent-model-preference-" +
                   std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(root);
  const char* inherited_home = getenv("HOME");
  std::string prior_home = inherited_home ? inherited_home : "";
  setenv("HOME", root.c_str(), 1);

  std::string error;
  CHECK(SaveModelPreference({"provider/fast", "https://example.test/v1", true},
                            error));
  ModelPreference saved = LoadModelPreference();
  CHECK(saved.selection == "provider/fast");
  CHECK(saved.base_url == "https://example.test/v1");
  CHECK(saved.route);
  struct stat st{};
  CHECK(stat(ModelPreferencePath().c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  CHECK(!SaveModelPreference({"bad\nmodel", "https://example.test/v1", false},
                             error));

  if (inherited_home) {
    setenv("HOME", prior_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestProviderTemplates() {
  const ProviderTemplate* openrouter = FindProviderTemplate("openrouter");
  CHECK(openrouter != nullptr);
  CHECK(openrouter && openrouter->matches_url("https://openrouter.ai/api/v1"));
  CHECK(FindProviderTemplateForUrl("https://edge.openrouter.ai/v1") ==
        openrouter);

  constexpr ProviderTemplate kTestProvider = {
      "test",
      "https://provider.test/v1",
      "UAGENT_TEST_PROVIDER_KEY",
      "UAGENT_TEST_PROVIDER_MODEL",
      "UAGENT_TEST_PROVIDER_EFFORT",
      "default-model",
      +[](std::string url) { return url == "https://provider.test/v1"; },
      ProviderProtocol::kOpenAi,
  };
  const char* inherited_effort = getenv("UAGENT_REASONING_EFFORT");
  std::string prior_effort = inherited_effort ? inherited_effort : "";
  setenv(kTestProvider.api_key_env, "test-key", 1);
  setenv(kTestProvider.model_env, "selected-model", 1);
  setenv(kTestProvider.effort_env, "low", 1);
  unsetenv("UAGENT_REASONING_EFFORT");
  RuntimeConfig config;
  Api api(config);
  CHECK(ApplyProviderTemplate(api, kTestProvider));
  CHECK(api.base_url == kTestProvider.base_url);
  CHECK(api.api_key == "test-key");
  CHECK(api.model == "selected-model");
  CHECK(api.reasoning_effort == "low");
  unsetenv(kTestProvider.api_key_env);
  unsetenv(kTestProvider.model_env);
  unsetenv(kTestProvider.effort_env);
  if (inherited_effort) {
    setenv("UAGENT_REASONING_EFFORT", prior_effort.c_str(), 1);
  } else {
    unsetenv("UAGENT_REASONING_EFFORT");
  }
}

void TestNamedProviders() {
  std::vector<std::pair<const char*, std::optional<std::string>>> prior_route;
  for (const char* key : {"UAGENT_BASE_URL", "UAGENT_MODEL",
                          "UAGENT_REASONING_EFFORT", "UAGENT_CONTEXT"}) {
    const char* value = getenv(key);
    prior_route.emplace_back(
        key, value ? std::optional<std::string>(value) : std::nullopt);
  }
  const char* inherited = getenv("UAGENT_PROVIDERS");
  std::string prior = inherited ? inherited : "";
  const char* inherited_openrouter = getenv("OPENROUTER_API_KEY");
  std::string prior_openrouter =
      inherited_openrouter ? inherited_openrouter : "";
  json configured = {
      {"", {{"base_url", "https://empty.test/v1"}}},
      {"all", {{"base_url", "https://reserved.test/v1"}}},
      {"bad/name", {{"base_url", "https://invalid.test/v1"}}},
      {"codex-local",
       {{"base_url", "http://127.0.0.1:8787/api/v1/"},
        {"api_key", "local-key"},
        {"protocol", "openrouter"},
        {"context", 16384}}},
      {"static",
       {{"base_url", "https://static.test/v1"},
        {"models", {{"fast", "actual-model"}}}}},
  };
  setenv("UAGENT_PROVIDERS", JsonDump(configured).c_str(), 1);
  ProviderCatalog catalog = LoadProviderCatalog();
  CHECK(catalog.providers.size() == 2);
  CHECK(catalog.models.size() == 1);
  const NamedProvider* codex =
      FindNamedProvider(catalog.providers, "codex-local");
  CHECK(codex != nullptr);
  CHECK(codex && codex->base_url == "http://127.0.0.1:8787/api/v1");
  CHECK(codex && codex->api_key == "local-key");
  CHECK(codex && codex->context == 16384);
  CHECK(codex && codex->protocol == ProviderProtocol::kOpenRouter);

  std::optional<ModelRoute> dynamic = ResolveModelRoute(
      catalog.models, catalog.providers, "codex-local/org/model");
  CHECK(dynamic.has_value());
  CHECK(dynamic && dynamic->model == "org/model");
  CHECK(dynamic && dynamic->context == 16384);
  CHECK(dynamic && dynamic->protocol == ProviderProtocol::kOpenRouter);
  RuntimeConfig config;
  Api routed(config);
  routed.reasoning_effort = "medium";
  ApplyRoute(routed, *dynamic);
  CHECK(routed.reasoning_effort == "medium");
  CHECK(routed.capabilities.OpenRouter());
  ModelRoute fixed_effort = *dynamic;
  fixed_effort.effort = "low";
  ApplyRoute(routed, fixed_effort);
  CHECK(routed.reasoning_effort == "low");
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
  openrouter_api.capabilities = CapabilitiesForRoute(
      ProviderProtocol::kOpenRouter, openrouter_api.base_url);
  openrouter_api.model = "parent-model";
  CHECK(DefaultTaskModel(openrouter_api) == "parent-model");
  CHECK(DelegationRuntimeContext(openrouter_api) ==
        "[delegation: parent=parent-model (default); default=parent]");
  CHECK(CanUseRawModel(openrouter_api, "stepfun/step-3.7-flash"));
  openrouter_api.base_url = "http://127.0.0.1:8787/api/v1";
  CHECK(CanUseRawModel(openrouter_api, "stepfun/step-3.7-flash"));
  openrouter_api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenAi, openrouter_api.base_url);
  CHECK(!CanUseRawModel(openrouter_api, "stepfun/step-3.7-flash"));
  CHECK(DefaultTaskModel(openrouter_api) == "parent-model");
  std::optional<ModelRoute> fixed =
      ResolveModelRoute(catalog.models, catalog.providers, "static/fast");
  CHECK(fixed.has_value());
  CHECK(fixed && fixed->model == "actual-model");
  CHECK(!ResolveModelRoute(catalog.models, catalog.providers, "missing/model"));
  CHECK(!ResolveModelRoute(catalog.models, catalog.providers, "codex-local/"));
  CHECK(!ResolveModelRoute(catalog.models, catalog.providers, "codex-local"));
  Api no_catalog(RuntimeConfig{});
  std::vector<ModelRoute> aliases = {
      {"static/low", "https://static.test/v1", "key", "same-model", "low", 0},
      {"static/high", "https://static.test/v1", "key", "same-model", "high",
       0}};
  ModelSearch alias_search = SearchModels(no_catalog, aliases, {}, "static");
  CHECK(alias_search.matches.size() == 2);

  setenv("OPENROUTER_API_KEY", "openrouter-key", 1);
  AddAvailableProviderTemplates(catalog);
  const NamedProvider* openrouter =
      FindNamedProvider(catalog.providers, "openrouter");
  CHECK(openrouter != nullptr);
  CHECK(openrouter && openrouter->base_url == "https://openrouter.ai/api/v1");
  CHECK(openrouter && openrouter->api_key == "openrouter-key");
  AddAvailableProviderTemplates(catalog);
  CHECK(catalog.providers.size() == 3);

  if (inherited) {
    setenv("UAGENT_PROVIDERS", prior.c_str(), 1);
  } else {
    unsetenv("UAGENT_PROVIDERS");
  }
  if (inherited_openrouter) {
    setenv("OPENROUTER_API_KEY", prior_openrouter.c_str(), 1);
  } else {
    unsetenv("OPENROUTER_API_KEY");
  }
  for (const auto& [key, value] : prior_route) {
    if (value) {
      setenv(key, value->c_str(), 1);
    } else {
      unsetenv(key);
    }
  }
}

void TestEffectiveConfigReload() {
  TestWorkspace workspace("effective-config");
  const char* inherited = getenv("UAGENT_MAX_STEPS");
  std::string prior = inherited ? inherited : "";
  const char* inherited_model = getenv("UAGENT_MODEL");
  std::string prior_model = inherited_model ? inherited_model : "";
  setenv("UAGENT_MAX_STEPS", "9", 1);
  unsetenv("UAGENT_MODEL");
  std::string path = UagentConfigPath();
  CHECK(ToolWriteFile(
            path,
            "UAGENT_MAX_STEPS=4\n"
            "UAGENT_MAX_TOOL_CALLS=2\n"
            "UAGENT_MCP_SERVERS=9\n"
            "UAGENT_MODEL=initial-model\n"
            "UAGENT_SESSION_BUDGET=1\n"
            "UAGENT_WEB_SEARCH_URL=https://user:pass@search.example/v1\n"
            "search_secret=private-search-key\n"
            "UAGENT_WEB_SEARCH_API_KEY=$search_secret\n")
            .output.starts_with("wrote "));
  ConfigManager manager = ConfigManager::Capture(
      /*trust_project=*/false, /*cli_budget=*/3.5,
      /*cli_no_memory=*/true);
  RuntimeConfig active = manager.Initialize();
  CHECK(active.max_steps == 9);
  CHECK(active.max_tool_calls == 2);
  CHECK(active.mcp_servers == 9);
  CHECK(active.session_budget == 3.5);
  CHECK(!active.memory_enabled);
  json diagnostic = manager.DiagnosticJson(active);
  CHECK(diagnostic["sources"]["UAGENT_MAX_STEPS"] == "environment");
  CHECK(diagnostic["sources"]["UAGENT_SESSION_BUDGET"] == "cli");
  CHECK(diagnostic["provenance"]["max_steps"] == "environment");
  CHECK(diagnostic["provenance"]["max_tool_calls"] == "global-config");
  CHECK(diagnostic["provenance"]["request_bytes"] == "default");
  CHECK(diagnostic["configured"]["web_search_api_key"] == "<set>");
  std::string shown = JsonDump(diagnostic);
  CHECK(shown.find("private-search-key") == std::string::npos);
  CHECK(shown.find("user:pass") == std::string::npos);

  CHECK(ToolWriteFile(path,
                      "UAGENT_MAX_TOOL_CALLS=7\n"
                      "UAGENT_MCP_SERVERS=10\n"
                      "UAGENT_MODEL=next-model\n"
                      "UAGENT_WEB_SEARCH_API_KEY=changed-secret\n")
            .output.starts_with("wrote "));
  std::optional<ConfigReload> reload = manager.Reload(active);
  CHECK(reload.has_value());
  if (reload) {
    CHECK(reload->active.max_steps == 9);
    CHECK(reload->active.max_tool_calls == 7);
    CHECK(reload->active.mcp_servers == 9);
    CHECK(std::find(reload->applied.begin(), reload->applied.end(),
                    "max_tool_calls") != reload->applied.end());
    CHECK(std::find(reload->deferred.begin(), reload->deferred.end(),
                    "mcp_servers") != reload->deferred.end());
    CHECK(std::find(reload->deferred.begin(), reload->deferred.end(),
                    "UAGENT_MODEL") != reload->deferred.end());
    CHECK(std::find(reload->deferred.begin(), reload->deferred.end(),
                    "web_search_api_key") != reload->deferred.end());
  }
  CHECK(JsonDump(manager.DiagnosticJson(reload ? reload->active : active))
            .find("changed-secret") == std::string::npos);

  if (inherited) {
    setenv("UAGENT_MAX_STEPS", prior.c_str(), 1);
  } else {
    unsetenv("UAGENT_MAX_STEPS");
  }
  if (inherited_model) {
    setenv("UAGENT_MODEL", prior_model.c_str(), 1);
  } else {
    unsetenv("UAGENT_MODEL");
  }
}

}  // namespace uagent
