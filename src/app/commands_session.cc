// Copyright 2026 Timon Gentzsch

#include <chrono>
#include <cinttypes>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/child_agent.h"
#include "include/app/commands.h"
#include "include/app/self_description.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/sandbox.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/providers.h"
#include "include/tools/subagent.h"
#include "include/ui/display.h"
#include "src/app/commands_internal.h"

namespace uagent {

void SaveSessionSettings(AppSession& session) {
  session.ActiveAgent().SessionSettings(
      {{"route", RouteSelection(session.ApiClient(),
                                session.context.provider.providers)},
       {"permissions",
        PermissionOverrideName(session.context.permission_override.load())},
       {"tools", session.ActiveAgent().ToolSelectionSettings()}});
}

bool AppSession::Save(std::string& error, bool force) {
  SaveSessionSettings(*this);
  if ((force || ActiveAgent().Revision() != saved_revision ||
       !PathExists(session_file)) &&
      !ActiveAgent().Save(session_file, error)) {
    return false;
  }
  if (!context.observability.Journal().Flush(session_file + ".events.jsonl",
                                             error)) {
    return false;
  }
  saved_revision = ActiveAgent().Revision();
  return true;
}

void HandleAttach(AppSession& session, const std::string& argument,
                  CommandReply& reply) {
  if (argument.empty()) {
    if (session.attachments.empty()) {
      reply.Print("%s· no pending attachments%s\n", DIM(), RST());
    } else {
      for (const Attachment& attachment : session.attachments) {
        reply.Print("%s· %s (%s)%s\n", DIM(),
                    TerminalSafe(attachment.path).c_str(),
                    attachment.mime.c_str(), RST());
      }
    }
    return;
  }
  if (argument == "clear") {
    session.attachments.clear();
    reply.Print("%s· attachments cleared%s\n", DIM(), RST());
    return;
  }
  Attachment attachment;
  std::string error;
  // Dropped or pasted paths often arrive quoted; accept one pair.
  if (!InspectAttachment(Unquote(argument), attachment, error)) {
    reply.Print("%s%s%s\n", RED(), error.c_str(), RST());
    return;
  }
  session.attachments.push_back(std::move(attachment));
  reply.Print("%s· attached %s for the next message%s\n", DIM(),
              session.attachments.back().name.c_str(), RST());
}

void HandleCost(const AppSession& session, CommandReply& reply) {
  reply.result = {
      {"routes", session.ActiveAgent().RouteUsageJson()},
      {"total", UsageJson(session.ActiveAgent().SessionUsage())},
      {"session_budget", session.ApiClient().config.session_budget}};
  const json& routes = reply.result["routes"];
  if (routes.empty()) {
    reply.Print("%s· no session spend yet%s\n", DIM(), RST());
    return;
  }
  for (const auto& [route, usage] : routes.items()) {
    std::string cost = JsonValue(usage, "cost_reported", false)
                           ? FmtCost(JsonValue(usage, "cost", 0.0))
                           : "cost unavailable";
    // Tokens beside the cost say *why* a route is expensive — a large
    // evidence block reads very differently from many small turns.
    Usage spent;
    spent.input = JsonValue(usage, "input", int64_t{0});
    spent.output = JsonValue(usage, "output", int64_t{0});
    spent.cache_read = JsonValue(usage, "cache_read", int64_t{0});
    std::string tokens = TokenSummary(spent);
    std::string cache = CacheSummary(spent);
    if (!cache.empty()) tokens += " · " + cache;
    reply.Print("%s· %s · %s · %s%s\n", DIM(), TerminalSafe(route).c_str(),
                tokens.c_str(), cost.c_str(), RST());
  }
  const Usage& spent = session.ActiveAgent().SessionUsage();
  std::string totals = TokenSummary(spent);
  std::string session_cache = CacheSummary(spent);
  if (!session_cache.empty()) totals += " · " + session_cache;
  reply.Print(
      "%s· total · %s · %s", DIM(), totals.c_str(),
      spent.cost_reported ? FmtCost(spent.cost).c_str() : "cost unavailable");
  if (session.ApiClient().config.session_budget > 0) {
    reply.Print(" / %s",
                FmtCost(session.ApiClient().config.session_budget).c_str());
  }
  reply.Print("%s\n", RST());
}

// What the session actually resolved to: the effective configuration with the
// source of each setting, then the request the model would see.
void HandleContext(AppSession& session, CommandReply& reply) {
  reply.result = {
      {"effective_config",
       session.context.config_manager.DiagnosticJson(session.Runtime().config)},
      {"capabilities", session.ApiClient().capabilities.DiagnosticJson()},
      {"model_request", session.ActiveAgent().ModelRequest()}};
  json effective = reply.result["effective_config"];
  effective["capabilities"] = reply.result["capabilities"];
  // The writable roots are the whole of what the sandbox permits, so the deep
  // view is the one place they are printed in full.
  effective["sandbox"] = SandboxDiagnosticJson();
  const json& sources = effective["sources"];
  auto source = [&](const char* key, const std::string& fallback = "runtime") {
    return sources.is_object() ? JsonValue(sources, key, fallback) : fallback;
  };
  std::string model_source = source("UAGENT_MODEL", source("OPENROUTER_MODEL"));
  std::string credential_source =
      source("UAGENT_API_KEY", source("OPENROUTER_API_KEY"));
  effective["route"] = {
      {"base_url", RedactedUrl(session.ApiClient().base_url)},
      {"base_url_source", source("UAGENT_BASE_URL")},
      {"model", session.ApiClient().RequestModel()},
      {"model_source", std::move(model_source)},
      {"credentials", session.ApiClient().api_key.empty() ||
                              session.ApiClient().api_key == kPlaceholderApiKey
                          ? "<unset>"
                          : "<set>"},
      {"credential_source", std::move(credential_source)},
      {"context_window", session.ApiClient().ctx_window}};
  reply.Print("%seffective configuration%s\n%s\n", BOLD(), RST(),
              TerminalSafe(JsonDump(effective, 2)).c_str());
  reply.Print("%smodel request%s\n", BOLD(), RST());
  reply.Print("%s\n",
              TerminalSafe(JsonDump(reply.result["model_request"], 2)).c_str());
}

// The startup row is a snapshot; MCP refresh and config reloads change the
// set mid-session, so this is the live view.

// /context is the deep live-context view. /status answers the everyday
// questions in one screen and /debug-config explains provenance.
void HandleStatus(const AppSession& session, CommandReply& reply) {
  reply.result =
      DescribeSelf(SelfTopic::kStatus, "", DescriptionInputs(session));
  const json& status = reply.result;
  auto row = [&reply](const char* label, const std::string& value) {
    reply.Print("  %s%-16s%s %s\n", DIM(), label, RST(),
                TerminalSafe(value).c_str());
  };
  reply.Print("%s\u00b5Agent %s%s\n", BOLD(), kVersion, RST());
  row("route", JsonValue(status, "route", std::string()));
  row("wire api", JsonValue(status, "wire_api", std::string()));
  row("endpoint", JsonValue(status, "base_url", std::string()));
  row("effort", JsonValue(status, "effort", std::string()));
  row("approval", JsonValue(status, "approval", std::string()));
  row("web search", JsonValue(status, "web_search", std::string()));
  row("sandbox", JsonValue(status["sandbox"], "summary", std::string()));
  row("memory", JsonValue(status, "memory", false) ? "on" : "off");
  row("tools", std::to_string(JsonValue(status, "tools", int64_t{0})));
  int64_t window = JsonValue(status, "context_window", int64_t{0});
  row("context",
      window > 0 ? FmtCount(window) : std::string("provider default"));
  double budget = JsonValue(status, "session_budget", 0.0);
  if (budget > 0) row("session budget", FmtCost(budget));
  const json& restart = status["restart_required"];
  if (restart.is_array() && !restart.empty()) {
    std::string names;
    for (const json& key : restart) {
      names += (names.empty() ? "" : ", ") + key.get<std::string>();
    }
    row("restart needed", names);
  }
}

void HandleDebugConfig(const AppSession& session, const std::string& argument,
                       CommandReply& reply) {
  reply.result =
      DescribeSelf(SelfTopic::kConfig, argument, DescriptionInputs(session));
  const json& described = reply.result;
  const json& settings = described["settings"];
  if (settings.empty()) {
    reply.Print("%s\u00b7 no setting named %s%s\n", RED(),
                TerminalSafe(argument).c_str(), RST());
    return;
  }
  // Only settings the user actually influenced, unless one was named: the full
  // schema belongs in the generated reference, not in a terminal dump.
  bool named = !argument.empty();
  reply.Print("%sconfiguration%s\n", BOLD(), RST());
  for (const json& setting : settings) {
    std::string source = JsonValue(setting, "source", std::string("default"));
    if (!named && source == "default") continue;
    std::string name = JsonValue(setting, "name", std::string());
    reply.Print("  %s%s%s\n", BOLD(), TerminalSafe(name).c_str(), RST());
    reply.Print("    %ssource%s      %s\n", DIM(), RST(),
                TerminalSafe(source).c_str());
    if (setting.contains("active")) {
      reply.Print("    %sactive%s      %s\n", DIM(), RST(),
                  TerminalSafe(JsonDump(setting["active"])).c_str());
    }
    reply.Print("    %sdefault%s     %s\n", DIM(), RST(),
                TerminalSafe(JsonDump(setting["default"])).c_str());
    reply.Print("    %stakes effect%s %s\n", DIM(), RST(),
                TerminalSafe(JsonValue(setting, "takes_effect", std::string()))
                    .c_str());
  }
  const json& restart = described["restart_required"];
  if (restart.is_array() && !restart.empty()) {
    reply.Print("%s\u00b7 restart required for %zu changed setting%s%s\n",
                YEL(), restart.size(), restart.size() == 1 ? "" : "s", RST());
  }
  reply.Print(
      "%s\u00b7 precedence: command line, process environment, trusted "
      "project config, user config, built-in default%s\n",
      DIM(), RST());
}

void HandleTools(AppSession& session, const std::string& argument,
                 CommandReply& reply) {
  json request = {{"kind", "tools"}, {"operation", "catalog"}};
  if (!argument.empty()) {
    std::istringstream input(argument);
    std::string operation, value, extra;
    input >> operation >> value >> extra;
    if (!extra.empty() ||
        (operation != "reset" && operation != "profile" && operation != "on" &&
         operation != "off") ||
        ((operation == "reset") != value.empty())) {
      reply.Print("%serror: usage: /tools [on|off NAME|profile NAME|reset]%s\n",
                  RED(), RST());
      return;
    }
    request["operation"] =
        operation == "on" || operation == "off" ? "set" : operation;
    if (operation == "profile") request["profile"] = value;
    if (operation == "on" || operation == "off") {
      request["name"] = value;
      request["active"] = operation == "on";
    }
  }
  reply.result = SessionControl(session, request);
  const json& result = reply.result;
  if (result.contains("error")) {
    reply.Print("%serror: %s%s\n", RED(),
                TerminalSafe(JsonValue(result, "error", "")).c_str(), RST());
    return;
  }
  reply.Print("%s· %" PRId64 "/%" PRId64 " tools · %s · %" PRId64
              " serialized schema bytes%s\n",
              DIM(), JsonValue(result, "active", int64_t{0}),
              JsonValue(result, "available", int64_t{0}),
              TerminalSafe(JsonValue(result, "profile", "default")).c_str(),
              JsonValue(result, "schema_bytes", int64_t{0}), RST());
  if (const json* tools = JsonArray(result, "tools")) {
    for (const json& tool : *tools) {
      reply.Print("%s%s %-14s %s · %s%s\n", DIM(),
                  JsonValue(tool, "active", false) ? "●" : "○",
                  TerminalSafe(JsonValue(tool, "name", "")).c_str(),
                  TerminalSafe(JsonValue(tool, "category", "")).c_str(),
                  TerminalSafe(JsonValue(tool, "description", "")).c_str(),
                  RST());
    }
  }
}

// The collaborator records the subagent tool reports, joined with what the
// supervisor knows about the ones still running. The id is the join key: it is
// what the spawn stamped on the job, and it is what the human types back.
json AgentsJson(const AppSession& session) {
  const ProcessSupervisor& processes = session.Runtime().processes;
  std::vector<SubagentView> live = processes.SubagentViews();
  json rows = json::array();
  for (json& record :
       CollaboratorSummaries(processes, &session.Runtime().collaborator)) {
    const std::string id = JsonValue(record, "id", std::string());
    for (const SubagentView& view : live) {
      if (view.source_id != id) continue;
      record["elapsed_ms"] =
          std::chrono::duration_cast<std::chrono::milliseconds>(view.elapsed)
              .count();
      if (!view.tail.empty()) record["progress"] = view.tail;
      break;
    }
    rows.push_back(std::move(record));
  }
  return rows;
}

SelfDescriptionInputs DescriptionInputs(const AppSession& session) {
  return {session.context.config_manager, session.Runtime().config,
          session.ApiClient(), session.context.tools, &session.ActiveAgent()};
}

}  // namespace uagent
