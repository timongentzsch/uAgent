// Copyright 2026 Timon Gentzsch

#include "include/app/setup.h"

#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "include/api/client.h"
#include "include/app/control.h"
#include "include/core/json.h"
#include "include/core/strings.h"
#include "include/providers/providers.h"

namespace uagent {
namespace {

constexpr char kProviders[] = "UAGENT_PROVIDERS";
constexpr char kModel[] = "UAGENT_MODEL";

// The three endpoints setup knows, in the order a default model is chosen
// from. OpenRouter's key is a setting of its own; the others are variables a
// provider of that name refers to.
struct Known {
  const char* name;
  const char* label;
  const char* variable;
  const char* base_url;
  const char* wire_api;
  WireApi wire;
};
constexpr Known kKnown[] = {
    {"anthropic", "Anthropic", "ANTHROPIC_API_KEY",
     "https://api.anthropic.com/v1", "anthropic_messages",
     WireApi::kAnthropicMessages},
    {"openai", "OpenAI", "OPENAI_API_KEY", "https://api.openai.com/v1",
     "responses", WireApi::kResponses},
    {"openrouter", "OpenRouter", "OPENROUTER_API_KEY",
     "https://openrouter.ai/api/v1", nullptr, WireApi::kChatCompletions},
};

std::string& KeyOf(SetupKeys& keys, const std::string& name) {
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
  std::string line = ReadLine();
  if (g_hidden) tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
  g_hidden = 0;
  std::signal(SIGINT, previous);
  fputc('\n', stdout);
  return line;
}

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
  KeyCheck check;
  // OpenRouter lists its models to anyone; its key endpoint is what refuses
  // a wrong key.
  if (std::string_view(known.name) == "openrouter") {
    JsonResponse response = api.Get("/key", false, 10);
    if (response.http_status == 401 || response.http_status == 403) {
      check.accepted = false;
      check.reason = "rejected (" + std::to_string(response.http_status) + ")";
      return check;
    }
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
    const std::string answer = ReadLine();
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

}  // namespace

void ApplySetupKeys(const SetupKeys& keys, SettingValues& scope) {
  if (!keys.openrouter.empty()) scope["OPENROUTER_API_KEY"] = keys.openrouter;
  if (keys.openai.empty() && keys.anthropic.empty()) return;
  json providers = json::object();
  if (auto held = scope.find(kProviders); held != scope.end()) {
    providers = json::parse(held->second, nullptr, false);
    // Providers set from a variable are that variable's to change.
    if (!providers.is_object()) return;
  }
  for (const Known& known : kKnown) {
    if (!known.wire_api) continue;
    const std::string& key =
        std::string_view(known.name) == "openai" ? keys.openai : keys.anthropic;
    if (key.empty()) continue;
    scope[known.variable] = key;
    json& provider = providers[known.name];
    if (!provider.is_object()) provider = json::object();
    if (!provider.contains("base_url")) provider["base_url"] = known.base_url;
    if (!provider.contains("wire_api")) provider["wire_api"] = known.wire_api;
    provider["api_key"] = std::string("$") + known.variable;
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

  SetupKeys keys;
  // The key each endpoint will use, new or kept, and what it lists.
  std::vector<std::pair<const Known*, KeyCheck>> usable;
  for (const Known& known : kKnown) {
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
    std::string key = ReadHidden();
    if (key.empty() && existing.empty() && exported) key = exported;
    const std::string effective = key.empty() ? existing : key;
    if (effective.empty()) continue;
    KeyCheck check = CheckKey(known, effective);
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
      !keys.anthropic.empty()) {
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

int SetupMain() {
  const int status = RunSetup();
  if (status == 0) printf("Start with: uagent\n");
  return status;
}

}  // namespace uagent
