// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
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

  // One table drives the set and the clear, so the two cannot drift apart.
  static constexpr std::pair<const char*, const char*> kRuntimeEnv[] = {
      {"UAGENT_MAX_STEPS", "0"},
      {"UAGENT_MAX_TOOL_CALLS", "0"},
      {"UAGENT_TEST_LONG", "999999999999999999999999999999"},
      {"UAGENT_SESSION_BUDGET", "2.5"},
      {"UAGENT_MAX_TURN_COST", "nan"},
      {"UAGENT_SUBAGENT_MODEL", "fast/model"},
      {"UAGENT_TOOL_TRACE_PROTECT_CHARS", "1234"},
      {"UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS", "5678"},
      {"UAGENT_SESSION_ARCHIVE_BYTES", "-1"},
      {"UAGENT_WEB_SEARCH_MODEL", "vendor/search"},
      {"UAGENT_WEB_SEARCH_BACKEND", "off"},
      {"UAGENT_WEB_SEARCH_URL", "https://search.example/v1"},
      {"UAGENT_WEB_SEARCH_API_KEY", "secret-search-key"},
      {"UAGENT_WEB_SEARCH_EFFORT", "low"},
      {"UAGENT_WEB_SEARCH_ENGINE", "invalid"},
      {"UAGENT_WEB_SEARCH_CONTEXT_SIZE", "huge"},
      {"UAGENT_WEB_SEARCH_MAX_RESULTS", "99"},
      {"UAGENT_WEB_SEARCH_MAX_USES", "0"},
      {"UAGENT_MCP_ROOTS", "/tmp/one:/tmp/two"},
      {"UAGENT_OPENROUTER_VARIANT", "floor"},
  };
  for (const auto& entry : kRuntimeEnv) setenv(entry.first, entry.second, 1);
  RuntimeConfig config = RuntimeConfig::FromEnvironment();
  CHECK(config.max_steps == 0);
  CHECK(config.max_tool_calls == 0);
  CHECK(EnvLong("UAGENT_TEST_LONG", 7) == 7);
  CHECK(config.session_budget == 2.5);
  CHECK(config.max_turn_cost == 0);
  CHECK(SubagentModel() == "fast/model");
  CHECK(ToolTraceProtectChars() == 1234);
  CHECK(ToolTracePruneMinChars() == 5678);
  CHECK(config.session_archive_bytes == 0);
  CHECK(config.web_search_model == "vendor/search");
  CHECK(config.web_search_backend == "off");
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
  for (const auto& entry : kRuntimeEnv) unsetenv(entry.first);
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
  // Uncapped by default: neither spelling is sent, so the provider's own
  // maximum applies and never clamps a derived thinking budget.
  CHECK(!body.contains("max_completion_tokens"));
  CHECK(!body.contains("max_tokens"));
  setenv("UAGENT_MAX_TOKENS", "4096", 1);
  body = api.BuildChatBody(json::array(), json::array());
  CHECK(body.value("max_completion_tokens", int64_t{0}) == 4096);
  CHECK(!body.contains("max_tokens"));
  unsetenv("UAGENT_MAX_TOKENS");
  CHECK(body["stream_options"].value("include_usage", false));

  // The incremental payload cache reuses the previous request's serialized
  // messages, so it has to be byte-identical to dumping the whole body -
  // provider prefix caching pays for exact bytes and a stale prefix would be
  // silent. Every shape of history mutation is checked, not just appending.
  json history = json::array();
  auto message = [](const char* role, std::string text) {
    return json{{"role", role}, {"content", std::move(text)}};
  };
  auto same_bytes = [&](const json& messages, const std::string& session) {
    bool available = false;
    return api.ChatPayload(messages, json::array(), session, &available) ==
           JsonDump(
               api.BuildChatBody(messages, json::array(), session, &available));
  };
  CHECK(same_bytes(history, ""));  // empty history
  history.push_back(message("system", "base"));
  CHECK(same_bytes(history, ""));  // first element
  history.push_back(message("user", "h\u00e9llo \"quoted\"\n\t"));
  CHECK(same_bytes(history, ""));  // append, with bytes that escape
  CHECK(same_bytes(history, ""));  // unchanged history, full cache hit
  history[1]["content"] = "edited";
  CHECK(same_bytes(history, ""));  // last element rewritten
  history[0]["content"] = "rewritten";
  CHECK(same_bytes(history, ""));  // prefix rewritten
  history.erase(1);
  CHECK(same_bytes(history, ""));  // truncation
  history.insert(history.begin(), message("user", "prepended"));
  CHECK(same_bytes(history, ""));        // insertion shifts every element
  CHECK(same_bytes(json::array(), ""));  // cleared
  CHECK(same_bytes(history, ""));        // refilled after a clear
  CHECK(same_bytes(history, "stable-session"));  // body around it changes

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

  ScopedEnv scoped_home("HOME", root.c_str());
  ScopedEnv scoped_config("UAGENT_CONFIG_FILE");
  ScopedEnv scoped_key("OPENROUTER_API_KEY");
  ScopedEnv scoped_model("OPENROUTER_MODEL");
  ScopedEnv scoped_effort("OPENROUTER_EFFORT");
  ConfigManager loaded = ConfigManager::Capture(/*trust_project=*/false, {});
  (void)loaded.Initialize();
  CHECK(EnvStr("OPENROUTER_API_KEY") == "test-key");
  CHECK(EnvStr("OPENROUTER_MODEL") == "vendor/model");
  CHECK(EnvStr("OPENROUTER_EFFORT") == "high");

  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestChildEnvironmentPolicy() {
  ScopedEnv scoped_api("UAGENT_API_KEY", "secret");
  ScopedEnv scoped_token("GITHUB_TOKEN", "secret");
  ScopedEnv scoped_usage("UAGENT_USAGE_FILE", "/tmp/ledger");
  ScopedEnv scoped_safe("UAGENT_CHILD_ENV_SAFE", "visible");
  ScopedEnv scoped_allow("UAGENT_SHELL_ENV_ALLOW",
                         " GITHUB_TOKEN, SSH_AUTH_SOCK ");

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
}

void TestModelPreference() {
  namespace fs = std::filesystem;
  fs::path root = fs::temp_directory_path() /
                  ("uagent-model-preference-" +
                   std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(root);
  ScopedEnv scoped_home("HOME", root.c_str());

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

  std::error_code ec;
  fs::remove_all(root, ec);
}

void TestProviderTemplates() {
  const ProviderTemplate* openrouter =
      FindProviderTemplateForUrl("https://openrouter.ai/api/v1");
  CHECK(openrouter != nullptr);
  CHECK(openrouter && openrouter->name == std::string("openrouter"));
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
  ScopedEnv scoped_key(kTestProvider.api_key_env, "test-key");
  ScopedEnv scoped_model(kTestProvider.model_env, "selected-model");
  ScopedEnv scoped_provider_effort(kTestProvider.effort_env, "low");
  ScopedEnv scoped_effort("UAGENT_REASONING_EFFORT");
  RuntimeConfig config;
  Api api(config);
  CHECK(ApplyProviderTemplate(api, kTestProvider));
  CHECK(api.base_url == kTestProvider.base_url);
  CHECK(api.api_key == "test-key");
  CHECK(api.model == "selected-model");
  CHECK(api.reasoning_effort == "low");
}

void TestNamedProviders() {
  // Route variables keep their inherited value until the body changes them,
  // and every one of them is restored when this scope ends.
  ScopedEnv scoped_base_url("UAGENT_BASE_URL", std::getenv("UAGENT_BASE_URL"));
  ScopedEnv scoped_model("UAGENT_MODEL", std::getenv("UAGENT_MODEL"));
  ScopedEnv scoped_effort("UAGENT_REASONING_EFFORT",
                          std::getenv("UAGENT_REASONING_EFFORT"));
  ScopedEnv scoped_context("UAGENT_CONTEXT", std::getenv("UAGENT_CONTEXT"));
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

  // Startup consumes the same selection grammar as interactive and side
  // routes. Suffixes configure policy and never leak into the provider model
  // identifier.
  setenv("UAGENT_MODEL", "codex-local/gpt-5.6-luna:nitro:low", 1);
  unsetenv("UAGENT_BASE_URL");
  unsetenv("UAGENT_REASONING_EFFORT");
  RuntimeConfig startup_config;
  Api startup(startup_config);
  ConfigureProvider(startup);
  CHECK(startup.base_url == "http://127.0.0.1:8787/api/v1");
  CHECK(startup.api_key == "local-key");
  CHECK(startup.model == "gpt-5.6-luna");
  CHECK(startup.reasoning_effort == "low");
  CHECK(startup.config.openrouter_variant == "nitro");
  CHECK(SelectModel(startup, catalog.models, catalog.providers,
                    "codex-local/gpt-5.6-sol:high") ==
        "codex-local/gpt-5.6-sol:high");
  CHECK(startup.model == "gpt-5.6-sol");
  CHECK(startup.reasoning_effort == "high");

  // [provider/]model[:variant][:effort] — suffixes peel from the right
  // against two closed sets and stop at the first unrecognized one.
  ModelSelection plain = ParseModelSelection("gpt-5.5");
  CHECK(plain.base == "gpt-5.5");
  CHECK(plain.variant.empty() && plain.effort.empty());
  ModelSelection scoped = ParseModelSelection("openrouter/deepseek/v4:xhigh");
  CHECK(scoped.base == "openrouter/deepseek/v4");
  CHECK(scoped.effort == "xhigh");
  CHECK(scoped.variant.empty());
  ModelSelection both = ParseModelSelection("openrouter/v4:nitro:xhigh");
  ModelSelection swapped = ParseModelSelection("openrouter/v4:xhigh:nitro");
  CHECK(both.base == "openrouter/v4" && swapped.base == "openrouter/v4");
  CHECK(both.variant == "nitro" && swapped.variant == "nitro");
  CHECK(both.effort == "xhigh" && swapped.effort == "xhigh");
  // An OpenRouter id suffix is not an effort, and it stops the peel.
  ModelSelection free_id = ParseModelSelection("openrouter/deepseek-chat:free");
  CHECK(free_id.base == "openrouter/deepseek-chat:free");
  CHECK(free_id.effort.empty() && free_id.variant.empty());
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
  CHECK(DefaultSubagentModel(openrouter_api) == "parent-model");
  // The runtime context names the parent in schema form, so the child can be
  // asked for a route in the same spelling the user would type.
  CHECK(DelegationRuntimeContext(openrouter_api) ==
        "[delegation: parent=openrouter/parent-model; default=parent]");
  openrouter_api.reasoning_effort = "high";
  CHECK(RouteSelection(openrouter_api, {}) == "openrouter/parent-model:high");
  openrouter_api.reasoning_effort.clear();
  CHECK(CanUseRawModel(openrouter_api, "stepfun/step-3.7-flash"));
  openrouter_api.base_url = "http://127.0.0.1:8787/api/v1";
  CHECK(CanUseRawModel(openrouter_api, "stepfun/step-3.7-flash"));
  openrouter_api.capabilities =
      CapabilitiesForRoute(ProviderProtocol::kOpenAi, openrouter_api.base_url);
  CHECK(!CanUseRawModel(openrouter_api, "stepfun/step-3.7-flash"));
  CHECK(DefaultSubagentModel(openrouter_api) == "parent-model");
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
}

void TestEffectiveConfigReload() {
  TestWorkspace workspace("effective-config");
  ScopedEnv scoped_steps("UAGENT_MAX_STEPS", "9");
  ScopedEnv scoped_model("UAGENT_MODEL");
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
  // The CLI layer outranks the environment and both config files.
  ConfigManager manager = ConfigManager::Capture(
      /*trust_project=*/false,
      {{"UAGENT_SESSION_BUDGET", "3.5"}, {"UAGENT_MEMORY", "0"}});
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
}

}  // namespace uagent
