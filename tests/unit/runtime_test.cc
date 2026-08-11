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

  g_signal_abort = 1;
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
  setenv("UAGENT_WEB_SEARCH_SERVER", "0", 1);
  setenv("UAGENT_MEMORY_GENERATE", "0", 1);
  setenv("UAGENT_MCP_ROOTS", "/tmp/one:/tmp/two", 1);
  setenv("UAGENT_OPENROUTER_VARIANT", "floor", 1);
  const char* checkpoint_mode = getenv("UAGENT_CHECKPOINT_MODE");
  std::string prior_checkpoint_mode = checkpoint_mode ? checkpoint_mode : "";
  unsetenv("UAGENT_CHECKPOINT_MODE");
  RuntimeConfig config = RuntimeConfig::FromEnvironment();
  CHECK(config.max_steps == 1);
  CHECK(EnvLong("UAGENT_TEST_LONG", 7) == 7);
  CHECK(config.session_budget == 2.5);
  CHECK(config.max_turn_cost == 1.0);
  CHECK(TaskModel() == "fast/model");
  CHECK(ToolTraceProtectChars() == 1234);
  CHECK(ToolTracePruneMinChars() == 5678);
  CHECK(config.session_archive_bytes == 0);
  CHECK(config.checkpoint_mode == "apply");
  CHECK(config.web_search_model == "vendor/search");
  CHECK(config.web_search_backend == "responses");
  CHECK(config.web_search_url == "https://search.example/v1");
  CHECK(config.web_search_api_key == "secret-search-key");
  CHECK(config.web_search_effort == "low");
  CHECK(config.web_search_engine == "auto");
  CHECK(config.web_search_context_size.empty());
  CHECK(config.web_search_max_results == 25);
  CHECK(config.web_search_max_uses == 1);
  CHECK(!config.web_search_server);
  CHECK(!config.memory_generate);
  CHECK(config.mcp_roots == "/tmp/one:/tmp/two");
  CHECK(config.openrouter_variant == "floor");
  json diagnostics = config.DiagnosticJson();
  CHECK(diagnostics.value("max_steps", int64_t{0}) == config.max_steps);
  CHECK(diagnostics.value("web_search_model", "") == config.web_search_model);
  CHECK(diagnostics.value("web_search_backend", "") ==
        config.web_search_backend);
  CHECK(diagnostics.value("openrouter_variant", "") == "floor");
  CHECK(!diagnostics.value("memory_generate", true));
  CHECK(diagnostics.value("mcp_roots", "") == config.mcp_roots);
  CHECK(diagnostics.value("tool_trace_protect_chars", int64_t{0}) == 1234);
  CHECK(diagnostics.value("tool_trace_prune_min_chars", int64_t{0}) == 5678);
  CHECK(diagnostics.find("web_search_api_key") == diagnostics.end());
  if (checkpoint_mode) {
    setenv("UAGENT_CHECKPOINT_MODE", prior_checkpoint_mode.c_str(), 1);
  }
  unsetenv("UAGENT_MAX_STEPS");
  unsetenv("UAGENT_TEST_LONG");
  unsetenv("UAGENT_SESSION_BUDGET");
  unsetenv("UAGENT_MAX_TURN_COST");
  unsetenv("UAGENT_TASK_MODEL");
  unsetenv("UAGENT_TOOL_TRACE_PROTECT_CHARS");
  unsetenv("UAGENT_TOOL_TRACE_PRUNE_MIN_CHARS");
  unsetenv("UAGENT_MEMORY_GENERATE");
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
  unsetenv("UAGENT_WEB_SEARCH_SERVER");
  setenv("UAGENT_OPENROUTER_VARIANT", "invalid", 1);
  CHECK(RuntimeConfig::FromEnvironment().openrouter_variant.empty());
  unsetenv("UAGENT_OPENROUTER_VARIANT");
  RuntimeConfig defaults;
  CHECK(defaults.max_steps == 100);
  CHECK(defaults.max_turn_seconds == 3600);
  CHECK(defaults.first_event_timeout_s == 300);
  CHECK(defaults.stream_idle_timeout_s == 300);
  CHECK(defaults.request_timeout_s == 600);

  RuntimeConfig routed;
  routed.openrouter_provider = "streamlake";
  routed.openrouter_fallbacks = true;
  Api api(routed);
  api.base_url = "https://openrouter.ai/api/v1";
  api.openrouter_compatible = true;
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
  api.openrouter_compatible = false;
  body = api.BuildChatBody(json::array(), json::array(), "stable-session");
  CHECK(body.value("model", "") == "test:free");
  CHECK(!body.contains("session_id"));
  CHECK(!body.contains("provider"));

  api.base_url = "https://api.openai.com/v1";
  api.reasoning_effort = "high";
  body = api.BuildChatBody(json::array(), json::array());
  CHECK(body.value("reasoning_effort", "") == "high");
  CHECK(body.contains("max_completion_tokens"));
  CHECK(!body.contains("max_tokens"));
  CHECK(body["stream_options"].value("include_usage", false));

  std::filesystem::path profile_home =
      std::filesystem::temp_directory_path() /
      ("uagent-route-profile-" + std::to_string(getpid()));
  std::filesystem::create_directories(profile_home / ".uagent/config");
  const char* old_home = getenv("HOME");
  std::string saved_home = old_home ? old_home : "";
  setenv("HOME", profile_home.c_str(), 1);
  std::string profile_error;
  api.model = "profile-model";
  api.config.openrouter_provider.clear();
  api.config.checkpoint_mode = "apply";
  api.parallel_tools = true;
  api.image_input = true;
  json saved_profile = {
      {"schema", 3},
      {"routes",
       {{RouteProfileKey(api),
         {{"samples", 4},
          {"passing_samples", 4},
          {"pass_rate", 1.0},
          {"certified", true},
          {"scenario_classes", {"analysis", "checkpoint"}},
          {"certified_at_unix", static_cast<int64_t>(std::time(nullptr))},
          {"checkpoint_mode", "shadow"},
          {"parallel_hint_support", false},
          {"image_support", false}}}}},
      {"recommendations", json::object()}};
  CHECK(AtomicWriteFile(RouteProfilesPath(), JsonDump(saved_profile), 0600,
                        false, profile_error));
  api.openrouter_compatible = true;
  CHECK(ApplyRouteProfile(api).empty());
  api.openrouter_compatible = false;
  json profile = ApplyRouteProfile(api);
  CHECK(!profile.empty());
  CHECK(api.config.checkpoint_mode == "shadow");
  CHECK(!api.parallel_tools);
  CHECK(!api.image_input);
  CHECK(api.route_certified);
  saved_profile["routes"][RouteProfileKey(api)]["certified_at_unix"] = 1;
  CHECK(AtomicWriteFile(RouteProfilesPath(), JsonDump(saved_profile), 0600,
                        false, profile_error));
  api.config.checkpoint_mode = "apply";
  api.parallel_tools = true;
  api.image_input = true;
  api.route_certified = false;
  CHECK(ApplyRouteProfile(api).empty());
  CHECK(api.config.checkpoint_mode == "apply");
  CHECK(api.parallel_tools);
  CHECK(api.image_input);

  saved_profile["routes"][RouteProfileKey(api)]["certified_at_unix"] =
      static_cast<int64_t>(std::time(nullptr));
  saved_profile["routes"][RouteProfileKey(api)]["memory_enabled"] = false;
  CHECK(AtomicWriteFile(RouteProfilesPath(), JsonDump(saved_profile), 0600,
                        false, profile_error));
  CHECK(ApplyRouteProfile(api).empty());
  api.config.memory_enabled = false;
  CHECK(!ApplyRouteProfile(api).empty());
  api.config.memory_enabled = true;
  saved_profile["routes"][RouteProfileKey(api)]["memory_enabled"] = true;
  saved_profile["routes"][RouteProfileKey(api)]["memory_generate"] = false;
  CHECK(AtomicWriteFile(RouteProfilesPath(), JsonDump(saved_profile), 0600,
                        false, profile_error));
  CHECK(ApplyRouteProfile(api).empty());
  api.config.memory_generate = false;
  CHECK(!ApplyRouteProfile(api).empty());
  api.config.memory_generate = true;

  api.reasoning_effort.clear();
  std::string low_route = RouteKey(api.base_url, "", api.model, "low");
  saved_profile = {
      {"schema", 3},
      {"routes",
       {{low_route,
         {{"samples", 4},
          {"passing_samples", 4},
          {"pass_rate", 1.0},
          {"certified", true},
          {"scenario_classes", {"analysis", "checkpoint"}},
          {"certified_at_unix", static_cast<int64_t>(std::time(nullptr))},
          {"parallel_hint_support", true}}}}},
      {"recommendations",
       {{RouteKey(api.base_url, "", api.model, ""),
         {{"effort", "low"}, {"mean_cost", 0.01}, {"route", low_route}}}}}};
  CHECK(AtomicWriteFile(RouteProfilesPath(), JsonDump(saved_profile), 0600,
                        false, profile_error));
  CHECK(!ApplyRouteProfile(api).empty());
  CHECK(api.reasoning_effort == "low");
  api.reasoning_effort.clear();
  saved_profile["routes"][low_route]["certified_at_unix"] = 1;
  CHECK(AtomicWriteFile(RouteProfilesPath(), JsonDump(saved_profile), 0600,
                        false, profile_error));
  CHECK(ApplyRouteProfile(api).empty());
  CHECK(api.reasoning_effort.empty());
  if (old_home) {
    setenv("HOME", saved_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  std::filesystem::remove_all(profile_home);

  json assistant = {{"role", "assistant"}, {"content", ""}};
  ChatResult reasoning_result;
  reasoning_result.reasoning = "reasoning state";
  reasoning_result.reasoning_details =
      json::array({{{"type", "reasoning.text"},
                    {"index", 0},
                    {"text", "reasoning state"}}});
  api.base_url = "https://openrouter.ai/api/v1";
  api.openrouter_compatible = true;
  api.PreserveAssistantReasoning(assistant, reasoning_result);
  CHECK(!assistant.contains("reasoning"));
  CHECK(assistant["reasoning_details"] == reasoning_result.reasoning_details);
  reasoning_result.reasoning_details = json::array();
  json text_assistant = {{"role", "assistant"}, {"content", ""}};
  api.PreserveAssistantReasoning(text_assistant, reasoning_result);
  CHECK(text_assistant.value("reasoning", "") == "reasoning state");
  api.base_url = "http://127.0.0.1:8787/api/v1";
  json local_assistant = {{"role", "assistant"}, {"content", ""}};
  api.PreserveAssistantReasoning(local_assistant, reasoning_result);
  CHECK(local_assistant.value("reasoning", "") == "reasoning state");
  api.base_url = "https://example.com/v1";
  api.openrouter_compatible = false;
  json generic_assistant = {{"role", "assistant"}, {"content", ""}};
  api.PreserveAssistantReasoning(generic_assistant, reasoning_result);
  CHECK(!generic_assistant.contains("reasoning"));
  CHECK(!generic_assistant.contains("reasoning_details"));

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
  LoadConfigFile(/*trust_project=*/false);
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
      false,
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
  CHECK(codex && codex->openrouter_compatible);

  std::optional<ModelRoute> dynamic = ResolveModelRoute(
      catalog.models, catalog.providers, "codex-local/org/model");
  CHECK(dynamic.has_value());
  CHECK(dynamic && dynamic->model == "org/model");
  CHECK(dynamic && dynamic->context == 16384);
  CHECK(dynamic && dynamic->openrouter_compatible);
  RuntimeConfig config;
  Api routed(config);
  routed.reasoning_effort = "medium";
  ApplyRoute(routed, *dynamic);
  CHECK(routed.reasoning_effort == "medium");
  CHECK(routed.openrouter_compatible);
  ModelRoute fixed_effort = *dynamic;
  fixed_effort.effort = "low";
  ApplyRoute(routed, fixed_effort);
  CHECK(routed.reasoning_effort == "low");
  json activated = ActivateRoute(routed, "apply");
  CHECK(activated.empty());
  CHECK(routed.openrouter_web_search);
  CHECK(ContainsCaseInsensitive("codex-local/gpt-5.6-sol", "GPT-5.6"));
  CHECK(ContainsCaseInsensitive("openrouter/deepseek/v4", "openrouter"));
  CHECK(!ContainsCaseInsensitive("codex-local/gpt-5.6-sol", "deepseek"));
  CHECK(NormalizeModelQuery("all").empty());
  CHECK(NormalizeModelQuery("*").empty());
  CHECK(NormalizeModelQuery(" codex-local/* ") == "codex-local/");
  CHECK(NormalizeModelQuery("gpt-5.6") == "gpt-5.6");
  Api openrouter_api(config);
  openrouter_api.base_url = "https://openrouter.ai/api/v1";
  openrouter_api.openrouter_compatible = true;
  openrouter_api.model = "parent-model";
  CHECK(DefaultTaskModel(openrouter_api) == "deepseek/deepseek-v4-flash");
  CHECK(DelegationRuntimeContext(openrouter_api) ==
        "[delegation: parent=parent-model (default); "
        "default=deepseek/deepseek-v4-flash (default)]");
  CHECK(CanUseRawModel(openrouter_api, "stepfun/step-3.7-flash"));
  openrouter_api.base_url = "http://127.0.0.1:8787/api/v1";
  CHECK(CanUseRawModel(openrouter_api, "stepfun/step-3.7-flash"));
  openrouter_api.openrouter_compatible = false;
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

}  // namespace uagent
