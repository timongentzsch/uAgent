// Copyright 2026 Timon Gentzsch

#include "include/app/setup.h"

#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/api/client.h"
#include "include/app/control.h"
#include "include/app/openrouter_signin.h"
#include "include/core/capture.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/providers/providers.h"

namespace uagent {
namespace {

constexpr char kProviders[] = "UAGENT_PROVIDERS";
constexpr char kModel[] = "UAGENT_MODEL";

// The endpoints setup knows, in the order a default model is chosen from.
// OpenRouter's key is a setting of its own; the others are variables.
struct Known {
  const char* name;
  const char* label;
  const char* variable;
  const char* base_url;
  WireApi wire;
  bool free_only = false;
};
constexpr Known kKnown[] = {
    {"anthropic", "Anthropic", "ANTHROPIC_API_KEY",
     "https://api.anthropic.com/v1", WireApi::kAnthropicMessages},
    {"openai", "OpenAI", "OPENAI_API_KEY", "https://api.openai.com/v1",
     WireApi::kResponses},
    {"openrouter", "OpenRouter", "OPENROUTER_API_KEY",
     "https://openrouter.ai/api/v1", WireApi::kChatCompletions},
    {"openrouter-free", "OpenRouter Free", "OPENROUTER_FREE_API_KEY",
     "https://openrouter.ai/api/v1", WireApi::kChatCompletions, true},
};

template <typename Keys>
auto& KeyOf(Keys& keys, std::string_view name) {
  if (name == "openrouter-free") return keys.openrouter_free;
  if (name == "anthropic") return keys.anthropic;
  if (name == "openai") return keys.openai;
  return keys.openrouter;
}

// Echo stays off only while a key is typed; an interrupt puts it back.
termios g_saved_termios{};
volatile sig_atomic_t g_hidden = 0;

void RestoreEcho(int signal) {
  if (g_hidden) tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
  std::signal(signal, SIG_DFL);
  std::raise(signal);
}

std::string ReadLine() {
  std::string line;
  if (!std::getline(std::cin, line)) return "";
  return Trim(line);
}

}  // namespace

std::string ReadHidden() {
  termios hidden{};
  const bool terminal = tcgetattr(STDIN_FILENO, &g_saved_termios) == 0;
  auto previous = std::signal(SIGINT, RestoreEcho);
  if (terminal) {
    hidden = g_saved_termios;
    hidden.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    hidden.c_lflag |= static_cast<tcflag_t>(ICANON | ISIG);
    hidden.c_iflag |= ICRNL;
    g_hidden = tcsetattr(STDIN_FILENO, TCSANOW, &hidden) == 0;
  }
  std::string line;
  if (!std::getline(std::cin, line)) line.clear();
  if (g_hidden) tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
  g_hidden = 0;
  std::signal(SIGINT, previous);
  fputc('\n', stdout);
  return line;
}

namespace {

bool Confirm(const std::string& question, bool fallback) {
  printf("%s %s ", question.c_str(), fallback ? "[Y/n]" : "[y/N]");
  fflush(stdout);
  const std::string answer = ReadLine();
  if (answer.empty()) return fallback;
  return answer == "y" || answer == "Y" || answer == "yes";
}

struct KeyCheck {
  // Unknown when the endpoint could not be reached to ask.
  std::optional<bool> accepted;
  std::string reason;
  std::vector<std::string> models;
};

KeyCheck CheckKey(const Known& known, const std::string& key) {
  Api api;
  api.base_url = known.base_url;
  api.api_key = key;
  api.wire_api = known.wire;
  DeclareHost(api);
  if (known.free_only) api.options["free_only"] = true;
  KeyCheck check;
  // OpenRouter lists its models to anyone; its key endpoint is what refuses
  // a wrong key.
  if (std::string_view(known.name) == "openrouter" || known.free_only) {
    JsonResponse response = api.Get("/key", false, 10);
    if (response.http_status == 401 || response.http_status == 403) {
      check.accepted = false;
      check.reason = "rejected (" + std::to_string(response.http_status) + ")";
      return check;
    }
    if (!response.error.empty() || response.http_status != 200) {
      check.reason =
          response.error.empty() ? "key check unavailable" : response.error;
      return check;
    }
  }
  if (known.free_only) {
    auto models = CachedModels(api);
    if (!models) models = QueryModels(api, /*abortable=*/false, 15);
    if (!models) {
      check.reason =
          "could not fetch the free-model catalogue; try setup again";
      return check;
    }
    check.accepted = true;
    for (const ModelInfo& info : *models) check.models.push_back(info.id);
    return check;
  }
  JsonResponse response =
      api.Get(std::string_view(known.name) == "anthropic" ? "/models?limit=1000"
                                                          : "/models",
              false, 15);
  if (response.http_status == 401 || response.http_status == 403) {
    check.accepted = false;
    check.reason = "rejected (" + std::to_string(response.http_status) + ")";
    return check;
  }
  auto models =
      response.error.empty() ? ParseModels(response.body) : std::nullopt;
  if (!models) {
    check.reason = response.error.empty() ? "no model list" : response.error;
    return check;
  }
  check.accepted = true;
  for (const ModelInfo& info : *models) check.models.push_back(info.id);
  return check;
}

bool Contains(const std::string& text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

// The newest of `ids` that `keep` takes, by name: model names carry their
// version, so the largest is usually the latest.
std::string Newest(const std::vector<std::string>& ids, auto keep) {
  std::string best;
  for (const std::string& id : ids) {
    if (keep(id) && id > best) best = id;
  }
  return best;
}

// Shows `ids` matching `query` (all of them for an empty one), up to ten,
// numbered after `first`.
std::vector<std::string> Listed(const std::vector<std::string>& ids,
                                const std::string& query,
                                const std::string& first) {
  std::vector<std::string> shown;
  if (!first.empty()) shown.push_back(first);
  for (const std::string& id : ids) {
    if (shown.size() >= 10) break;
    if (id != first && (query.empty() || MatchesModelQuery(id, query))) {
      shown.push_back(id);
    }
  }
  return shown;
}

// Asks for the default model at `known`: Enter takes `suggested`, a number
// one of those shown, anything else searches. Empty when nothing is chosen.
std::string ChooseModel(const std::vector<std::string>& ids,
                        const std::string& suggested,
                        const std::string& current) {
  if (ids.empty()) {
    printf("Could not list the models; type one%s: ",
           current.empty() ? " (Enter skips)"
                           : (", Enter keeps " + current).c_str());
    fflush(stdout);
    return ReadLine();
  }
  std::string query;
  for (;;) {
    std::vector<std::string> shown =
        Listed(ids, query, query.empty() ? suggested : "");
    for (size_t i = 0; i < shown.size(); ++i) {
      printf(
          "  %zu) %s%s\n", i + 1, shown[i].c_str(),
          i == 0 && query.empty() && !suggested.empty() ? "  (suggested)" : "");
    }
    if (shown.empty()) printf("  nothing matches \"%s\"\n", query.c_str());
    const std::string fallback = !current.empty() ? current : suggested;
    printf("Default model [%s%s; a number picks, text searches]: ",
           fallback.empty()  ? "Enter skips"
           : current.empty() ? "Enter takes "
                             : "Enter keeps ",
           fallback.c_str());
    fflush(stdout);
    std::string answer = ReadLine();
    if (answer.empty()) return current.empty() ? suggested : "";
    int64_t picked = 0;
    if (ParseInt64(answer.c_str(), picked) && picked >= 1 &&
        static_cast<size_t>(picked) <= shown.size()) {
      return shown[static_cast<size_t>(picked) - 1];
    }
    // A full name the list does not hold is taken as typed.
    if (std::find(ids.begin(), ids.end(), answer) != ids.end()) {
      return answer;
    }
    query = answer;
  }
}

constexpr char kSignInFile[] = "openrouter-signin.json";
constexpr int kSignInSeconds = 600;

std::string Held(const SavedSettings& saved, const std::string& name) {
  auto found = saved.all.find(name);
  return found == saved.all.end() ? std::string() : found->second;
}

// The free endpoint's address: a saved provider keeps its own.
std::string FreeBaseUrl(const SavedSettings& saved, const Known& free) {
  const json providers = json::parse(Held(saved, kProviders), nullptr, false);
  return JsonValue(JsonValue(providers, free.name, json::object()), "base_url",
                   free.base_url);
}

const Known& FreeKnown() {
  return *std::ranges::find(kKnown, true, &Known::free_only);
}

// Saves `key` as the free provider with the default model `choose` takes
// from what the endpoint lists (suggested, current): the error, or empty
// with `model` set.
std::string SaveFreeKey(const std::string& key, auto choose,
                        std::string& model) {
  const SavedSettings saved = ReadSettings("");
  if (!saved.error.empty()) return saved.error;
  Known free = FreeKnown();
  const std::string base = FreeBaseUrl(saved, free);
  free.base_url = base.c_str();
  const KeyCheck check = CheckKey(free, key);
  if (check.accepted != true || check.models.empty()) {
    return "no usable free coding models: " +
           (check.reason.empty() ? "none listed with tools and 64K context"
                                 : check.reason);
  }
  const std::string prefix = std::string(free.name) + "/";
  std::string current = Held(saved, kModel);
  if (std::ranges::none_of(check.models, [&](const std::string& id) {
        return current == prefix + id;
      })) {
    current.clear();
  }
  model = choose(check.models, check.models.front(), current);
  if (model.empty()) model = current;
  if (!model.empty() && !model.starts_with(prefix)) model = prefix + model;
  return ChangeSettings("", [&](SettingValues& scope) {
    ApplySetupKeys({.openrouter_free = key}, scope);
    if (!model.empty()) scope[kModel] = model;
    return std::string();
  });
}

}  // namespace

void ApplySetupKeys(const SetupKeys& keys, SettingValues& scope) {
  if (keys.openrouter.empty() && keys.openai.empty() &&
      keys.anthropic.empty() && keys.openrouter_free.empty()) {
    return;
  }
  json providers = json::object();
  if (auto held = scope.find(kProviders); held != scope.end()) {
    providers = json::parse(held->second, nullptr, false);
    // Providers set from a variable are that variable's to change.
    if (!providers.is_object()) return;
  }
  for (const Known& known : kKnown) {
    const std::string& key = KeyOf(keys, known.name);
    if (key.empty()) continue;
    scope[known.variable] = key;
    json& provider = providers[known.name];
    if (!provider.is_object()) provider = json::object();
    if (!provider.contains("base_url")) provider["base_url"] = known.base_url;
    if (!provider.contains("wire_api")) {
      provider["wire_api"] = WireApiName(known.wire);
    }
    provider["api_key"] = std::string("$") + known.variable;
    if (known.free_only) EnsureObject(provider, "options")["free_only"] = true;
  }
  scope[kProviders] = JsonDump(providers);
}

std::string SuggestModel(const std::string& provider,
                         const std::vector<std::string>& ids) {
  std::string best;
  if (provider == "anthropic") {
    best = Newest(ids,
                  [](const std::string& id) { return Contains(id, "sonnet"); });
  } else if (provider == "openrouter") {
    best = Newest(ids, [](const std::string& id) {
      return id.starts_with("anthropic/") && Contains(id, "sonnet") &&
             !Contains(id, ":");
    });
  } else if (provider == "openai") {
    best = Newest(ids, [](const std::string& id) {
      if (!id.starts_with("gpt-")) return false;
      for (std::string_view skip :
           {"mini", "nano", "audio", "realtime", "search", "transcribe", "tts",
            "image", "decisions", "codex", "chat"}) {
        if (Contains(id, skip)) return false;
      }
      // A dated snapshot is the same model as its alias.
      return std::count(id.begin(), id.end(), '-') < 3;
    });
  }
  if (best.empty() && !ids.empty()) best = ids.front();
  return best;
}

bool ProviderConfigured() {
  const json reply = ControlProcess({{"kind", "provider"}});
  // A check that fails says nothing; the start itself will say what is wrong.
  return JsonValue(reply, "configured", true);
}

namespace {
int OpenRouterSetup(bool browser);
}  // namespace

int RunSetup() {
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    fprintf(stderr, "uagent setup needs a terminal\n");
    return 2;
  }
  SavedSettings saved = ReadSettings("");
  if (!saved.error.empty()) {
    fprintf(stderr, "uagent setup: %s\n", saved.error.c_str());
    return 1;
  }
  auto held = [&](const std::string& name) {
    auto found = saved.all.find(name);
    return found == saved.all.end() ? std::string() : found->second;
  };
  printf(
      "µAgent setup: keys are saved to %s, readable only by you.\n"
      "Press Enter to skip one. What you type is hidden.\n",
      SettingsPath().c_str());

  const bool free_only = Confirm(
      "Set the default to a free OpenRouter model (account/API key required)?",
      held(kModel).empty());
  if (free_only) {
    printf(
        "Create a key: https://openrouter.ai/keys\n"
        "Free models have usage limits and provider-specific data policies.\n"
        "Review them at https://openrouter.ai/collections/free-models\n"
        "Paid models, hosted search and PDF parsing are excluded from this "
        "provider.\n");
    if (Confirm("Sign in with OpenRouter instead of typing a key?",
                held(FreeKnown().variable).empty())) {
      return OpenRouterSetup(/*browser=*/true);
    }
  }
  SetupKeys keys;
  // The key each endpoint will use, new or kept, and what it lists.
  std::vector<std::pair<const Known*, KeyCheck>> usable;
  for (const Known& known : kKnown) {
    if (known.free_only != free_only) continue;
    const std::string existing = held(known.variable);
    const char* exported = std::getenv(known.variable);
    std::string hint;
    if (!existing.empty()) {
      hint = " [saved, Enter keeps it]";
    } else if (exported && *exported) {
      hint = std::string(" [found in $") + known.variable + ", Enter uses it]";
    }
    printf("%s API key%s: ", known.label, hint.c_str());
    fflush(stdout);
    std::string key = Trim(ReadHidden());
    if (key.empty() && existing.empty() && exported) key = exported;
    const std::string effective = key.empty() ? existing : key;
    if (effective.empty()) continue;
    KeyCheck check = CheckKey(known, effective);
    if (free_only && (check.accepted != true || check.models.empty())) {
      printf(
          "  No usable free coding models: %s. Saved settings are unchanged.\n",
          check.reason.empty() ? "none listed with tools and 64K context"
                               : check.reason.c_str());
      return 1;
    }
    if (check.accepted == false) {
      printf("  ✗ %s\n", check.reason.c_str());
      if (key.empty() || !Confirm("  Keep it anyway?", false)) continue;
    } else if (!check.accepted) {
      printf("  ? could not check it: %s\n", check.reason.c_str());
    } else {
      printf("  ✓ %zu models\n", check.models.size());
    }
    KeyOf(keys, known.name) = key;
    usable.emplace_back(&known, std::move(check));
  }
  if (!keys.openrouter.empty() || !keys.openai.empty() ||
      !keys.anthropic.empty() || !keys.openrouter_free.empty()) {
    const std::string error = ChangeSettings("", [&](SettingValues& scope) {
      ApplySetupKeys(keys, scope);
      return std::string();
    });
    if (!error.empty()) {
      fprintf(stderr, "uagent setup: %s\n", error.c_str());
      return 1;
    }
  }
  if (usable.empty()) {
    printf(
        "No key is set up. Run uagent setup again, or see "
        "https://github.com/timongentzsch/uAgent/blob/master/docs/GUIDE.md"
        "#models-and-providers for other endpoints.\n");
    return 1;
  }

  // The default model comes from the first endpoint with a key.
  const auto& [known, check] = usable.front();
  const std::string prefix = std::string(known->name) + "/";
  std::string current = held(kModel);
  if (free_only &&
      std::ranges::none_of(check.models, [&](const std::string& id) {
        return current == prefix + id;
      })) {
    current.clear();
  }
  printf("\nDefault model, from %s:\n", known->label);
  std::string chosen = ChooseModel(
      check.models, SuggestModel(known->name, check.models), current);
  if (!chosen.empty()) {
    if (!chosen.starts_with(prefix)) chosen = prefix + chosen;
    const std::string error = ChangeSettings("", [&](SettingValues& scope) {
      scope[kModel] = chosen;
      return std::string();
    });
    if (!error.empty()) {
      fprintf(stderr, "uagent setup: %s\n", error.c_str());
      return 1;
    }
    current = chosen;
  }
  printf("Saved%s. Run uagent setup again to change it.\n",
         current.empty() ? "" : ("; new conversations use " + current).c_str());
  return 0;
}

json OpenRouterControl(const json& request) {
  const std::string pending = UagentDir("") + kSignInFile;
  const SavedSettings saved = ReadSettings("");
  if (!saved.error.empty()) return {{"error", saved.error}};
  const std::string base = FreeBaseUrl(saved, FreeKnown());
  std::string error;
  if (JsonValue(request, "action", "") == "start") {
    const SignIn start = BeginSignIn(base, JsonValue(request, "callback", ""));
    if (!WritePrivateFile(
            pending,
            JsonDump({{"verifier", start.verifier},
                      {"state", start.state},
                      {"at", static_cast<int64_t>(time(nullptr))}}),
            error)) {
      return {{"error", error}};
    }
    return {{"url", start.url}};
  }
  const json held =
      json::parse(ReadFile(pending, 4096).value_or(""), nullptr, false);
  unlink(pending.c_str());
  const std::string state = JsonValue(request, "state", "");
  if (!held.is_object() ||
      time(nullptr) - JsonValue(held, "at", int64_t{0}) > kSignInSeconds ||
      (!state.empty() && state != JsonValue(held, "state", ""))) {
    return {{"error", "this sign-in is no longer open; start it again"}};
  }
  const std::string key =
      RedeemSignIn(base, Trim(JsonValue(request, "code", "")),
                   JsonValue(held, "verifier", ""), error);
  std::string model;
  if (error.empty()) {
    error = SaveFreeKey(
        key,
        [](const auto&, const std::string& suggested,
           const std::string& current) {
          return current.empty() ? suggested : current;
        },
        model);
  }
  if (!error.empty()) return {{"error", error}};
  return {{"model", model}};
}

namespace {

// Signs in on this terminal: through this machine's browser where there is
// one, otherwise by a link to open anywhere and the code it shows.
int OpenRouterSetup(bool browser) {
  const SavedSettings saved = ReadSettings("");
  if (!saved.error.empty()) {
    fprintf(stderr, "uagent setup: %s\n", saved.error.c_str());
    return 1;
  }
  const std::string base = FreeBaseUrl(saved, FreeKnown());
  const bool terminal = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#ifdef __APPLE__
  const char* opener = "open";
  const bool display = true;
#else
  const char* opener = "xdg-open";
  const bool display = std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY");
#endif
  std::string callback, code;
  const int listener =
      browser && terminal && display && !std::getenv("SSH_CONNECTION")
          ? ListenForSignIn(callback)
          : -1;
  const SignIn start = BeginSignIn(base, callback);
  if (listener >= 0) {
    printf(
        "Opening OpenRouter to sign in. If no browser opened:\n  %s\n"
        "Waiting for approval…\n",
        start.url.c_str());
    fflush(stdout);
    CaptureProcess({opener, start.url}, 10);
    code = AcceptSignIn(listener, start.state, kSignInSeconds);
  } else {
    printf(
        "Open this on any device and approve:\n  %s\n\n"
        "Code from that page: ",
        start.url.c_str());
    fflush(stdout);
    code = Trim(terminal ? ReadHidden() : ReadLine());
  }
  if (code.empty()) {
    fprintf(stderr, "uagent setup: no code arrived; nothing is changed\n");
    return 1;
  }
  std::string error, model;
  const std::string key = RedeemSignIn(base, code, start.verifier, error);
  if (error.empty()) {
    printf("Signed in.\n");
    error = SaveFreeKey(
        key,
        [&](const std::vector<std::string>& ids, const std::string& suggested,
            const std::string& current) {
          if (!terminal) return current.empty() ? suggested : current;
          printf("\nFree models with tool calling:\n");
          return ChooseModel(ids, suggested, current);
        },
        model);
  }
  if (!error.empty()) {
    fprintf(stderr, "uagent setup: %s\n", error.c_str());
    return 1;
  }
  printf("Saved; new conversations use %s.\n", model.c_str());
  return 0;
}

}  // namespace

int SetupMain(int argc, char** argv) {
  int status = 2;
  const std::string_view what = argc > 2 ? argv[2] : "";
  const std::string_view flag = argc > 3 ? argv[3] : "";
  if (argc == 2) {
    status = RunSetup();
  } else if (what == "openrouter" && argc <= 4 &&
             (flag.empty() || flag == "--no-browser")) {
    status = OpenRouterSetup(flag.empty());
  } else {
    fprintf(stderr, "usage: uagent setup [openrouter [--no-browser]]\n");
  }
  if (status == 0) printf("Start with: uagent\n");
  return status;
}

}  // namespace uagent
