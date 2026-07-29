// Copyright 2026 Timon Gentzsch

#include <string>

#include "tests/unit/test_support.h"

namespace uagent {

void TestRuntimeOwnershipHelpers() {
  AppRuntime runtime(RuntimeConfig{});
  runtime.Shutdown();
  runtime.Shutdown();

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

  setenv("UAGENT_MAX_STEPS", "0", 1);
  setenv("UAGENT_SESSION_ARCHIVE_BYTES", "-1", 1);
  setenv("UAGENT_WEB_SEARCH_MODEL", "vendor/search", 1);
  setenv("UAGENT_WEB_SEARCH_EFFORT", "low", 1);
  setenv("UAGENT_WEB_SEARCH_ENGINE", "invalid", 1);
  setenv("UAGENT_WEB_SEARCH_CONTEXT_SIZE", "huge", 1);
  setenv("UAGENT_WEB_SEARCH_MAX_RESULTS", "99", 1);
  setenv("UAGENT_WEB_SEARCH_MAX_USES", "0", 1);
  setenv("UAGENT_WEB_SEARCH_SERVER", "0", 1);
  const char* checkpoint_mode = getenv("UAGENT_CHECKPOINT_MODE");
  std::string prior_checkpoint_mode = checkpoint_mode ? checkpoint_mode : "";
  unsetenv("UAGENT_CHECKPOINT_MODE");
  RuntimeConfig config = RuntimeConfig::FromEnvironment();
  CHECK(config.max_steps == 1);
  CHECK(config.session_archive_bytes == 0);
  CHECK(config.checkpoint_mode == "apply");
  CHECK(config.web_search_model == "vendor/search");
  CHECK(config.web_search_effort == "low");
  CHECK(config.web_search_engine == "auto");
  CHECK(config.web_search_context_size.empty());
  CHECK(config.web_search_max_results == 25);
  CHECK(config.web_search_max_uses == 1);
  CHECK(!config.web_search_server);
  CHECK(config.DiagnosticJson().value("max_steps", int64_t{0}) ==
        config.max_steps);
  CHECK(config.DiagnosticJson().value("web_search_model", "") ==
        config.web_search_model);
  if (checkpoint_mode) {
    setenv("UAGENT_CHECKPOINT_MODE", prior_checkpoint_mode.c_str(), 1);
  }
  unsetenv("UAGENT_MAX_STEPS");
  unsetenv("UAGENT_SESSION_ARCHIVE_BYTES");
  unsetenv("UAGENT_WEB_SEARCH_MODEL");
  unsetenv("UAGENT_WEB_SEARCH_EFFORT");
  unsetenv("UAGENT_WEB_SEARCH_ENGINE");
  unsetenv("UAGENT_WEB_SEARCH_CONTEXT_SIZE");
  unsetenv("UAGENT_WEB_SEARCH_MAX_RESULTS");
  unsetenv("UAGENT_WEB_SEARCH_MAX_USES");
  unsetenv("UAGENT_WEB_SEARCH_SERVER");

  RuntimeConfig routed;
  routed.openrouter_provider = "streamlake";
  routed.openrouter_fallbacks = true;
  Api api(routed);
  api.base_url = "https://openrouter.ai/api/v1";
  api.model = "test";
  json body = api.BuildChatBody(json::array(), json::array(), "stable-session");
  CHECK(body.value("session_id", "") == "stable-session");
  CHECK(body["provider"]["order"][0] == "streamlake");
  CHECK(body["provider"].value("allow_fallbacks", false));
  api.reasoning_effort = "low";
  body = api.BuildChatBody(json::array(), json::array(), "stable-session");
  CHECK(body["reasoning"].value("effort", "") == "low");
  CHECK(!body.contains("reasoning_effort"));
  api.base_url = "http://127.0.0.1:8080/v1";
  body = api.BuildChatBody(json::array(), json::array(), "stable-session");
  CHECK(!body.contains("session_id"));
  CHECK(!body.contains("provider"));

  api.base_url = "https://api.openai.com/v1";
  api.reasoning_effort = "high";
  body = api.BuildChatBody(json::array(), json::array());
  CHECK(body.value("reasoning_effort", "") == "high");
  CHECK(body.contains("max_completion_tokens"));
  CHECK(!body.contains("max_tokens"));
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

  std::optional<ModelRoute> dynamic = ResolveModelRoute(
      catalog.models, catalog.providers, "codex-local/org/model");
  CHECK(dynamic.has_value());
  CHECK(dynamic && dynamic->model == "org/model");
  CHECK(dynamic && dynamic->context == 16384);
  std::optional<ModelQuery> all =
      ResolveProviderQuery(catalog.providers, "codex-local/*");
  CHECK(all && all->provider == codex && all->filter.empty());
  std::optional<ModelQuery> filtered =
      ResolveProviderQuery(catalog.providers, "codex-local/gpt-5.6");
  CHECK(filtered && filtered->provider == codex);
  CHECK(filtered && filtered->filter == "gpt-5.6");
  CHECK(!ResolveProviderQuery(catalog.providers, "unknown/filter"));
  std::optional<ModelRoute> fixed =
      ResolveModelRoute(catalog.models, catalog.providers, "static/fast");
  CHECK(fixed.has_value());
  CHECK(fixed && fixed->model == "actual-model");
  CHECK(!ResolveModelRoute(catalog.models, catalog.providers, "missing/model"));
  CHECK(!ResolveModelRoute(catalog.models, catalog.providers, "codex-local/"));
  CHECK(!ResolveModelRoute(catalog.models, catalog.providers, "codex-local"));

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
}

}  // namespace uagent
