// Copyright 2026 Timon Gentzsch

#include "include/app/commands.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/config_proposal.h"
#include "include/app/control.h"
#include "include/app/prompt_control.h"
#include "include/app/self_description.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/sandbox.h"
#include "include/core/signals.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/media/attachments.h"
#include "include/providers.h"
#include "include/agent/child_agent.h"
#include "include/agent/jobs.h"
#include "include/tools/memory.h"
#include "include/agent/process.h"
#include "include/tools/session.h"
#include "include/tools/subagent.h"
#include "include/ui/conversation.h"
#include "include/ui/sessions.h"

#include "src/app/commands_internal.h"
namespace uagent {

void LoadSessionJournal(AppSession& session, const std::string& previous_path) {
  const json settings = session.ActiveAgent().SessionSettings();
  const std::string route = JsonValue(settings, "route", "");
  if (!route.empty() &&
      !session.context.options.overrides.contains("UAGENT_MODEL")) {
    if (SelectModel(session.ApiClient(), session.context.provider.routes,
                    session.context.provider.providers, route)
            .empty()) {
      Emit(NoticeEvent(PresentationStatus::kFailed,
                       "Saved model is unavailable: " + route));
    } else {
      ActivateRoute(session.ApiClient());
      session.ActiveAgent().RouteChanged();
    }
  }
  if (!session.context.options.yolo) {
    int mode = JsonValue(settings, "permissions", -1);
    session.context.permission_override.store(mode >= -1 && mode <= 1 ? mode
                                                                      : -1);
    PermissionControl(session.context, json::object());
    session.ActiveAgent().ApprovalChanged();
  }
  if (session.session_file.empty() || session.session_file == previous_path) {
    return;
  }
  session.context.session_approvals.clear();
  std::string error;
  if (!session.context.observability.Journal().Load(
          session.session_file + ".events.jsonl", error)) {
    Emit(NoticeEvent(PresentationStatus::kFailed,
                     "cannot load session journal: " + error));
  }
  Emit(Event{EventId::kSessionResumed,
             {{"model", session.ApiClient().RequestModel()},
              {"messages", session.ActiveAgent().MessageCount()}}});
}

bool RunSlashCommand(AppSession& session, const ParsedSlashCommand& command,
                     json& result) {
  switch (command.spec->id) {
    case SlashCommandId::kQuit:
    case SlashCommandId::kReset:
    case SlashCommandId::kSessions:
    case SlashCommandId::kFork:
      result = {{"error", "conversation navigation belongs to the client"}};
      return false;
    case SlashCommandId::kTrace:
      if (!command.argument.empty()) {
        size_t offset = 0;
        do {
          result = session.ActiveAgent().RawExchange(command.argument, offset);
          printf("%s", TerminalSafe(JsonValue(result, "text",
                                              JsonValue(result, "error", "")))
                           .c_str());
          offset = JsonValue(result, "next", size_t{0});
        } while (JsonValue(result, "more", false));
        printf("\n");
        fflush(stdout);
        return false;
      }
      PrintLatestTrace(session.ActiveAgent().TraceArchive(),
                       session.context.tools);
      break;
    case SlashCommandId::kVariant:
      HandleVariant(session, command.argument);
      break;
    case SlashCommandId::kVerbose:
      session.ActiveAgent().SetVerbose(!session.ActiveAgent().Verbose());
      printf("%s· verbose %s%s\n", DIM(),
             session.ActiveAgent().Verbose()
                 ? "ON — full reasoning and expanded bounded tool output"
                 : "off — compact reasoning and compact tool output",
             RST());
      break;
    case SlashCommandId::kHelp:
      PrintCommandHelp();
      break;
    case SlashCommandId::kModels:
      HandleModels(session, command.argument);
      break;
    case SlashCommandId::kModel:
      HandleModel(session, command.argument);
      break;
    case SlashCommandId::kEffort:
      HandleEffort(session, command.argument);
      break;
    case SlashCommandId::kPermissions:
      result = PermissionControl(session.context, {{"mode", command.argument}});
      session.ActiveAgent().ApprovalChanged();
      if (session.context.channel == nullptr) {
        printf("%s\n", TerminalSafe(JsonDump(result, 2)).c_str());
        fflush(stdout);
      }
      return false;
    case SlashCommandId::kConfig: {
      json request = {{"kind", "config"}};
      if (!command.argument.empty()) {
        std::istringstream input(command.argument);
        std::string scope, change;
        input >> scope;
        std::getline(input, change);
        change = Trim(change);
        bool unset = change.starts_with("unset ");
        size_t equal = change.find('=');
        if (unset) change = Trim(change.substr(6));
        request.update(
            {{"operation", "apply"},
             {"scope", scope},
             {"changes",
              json::array({{{"key", unset ? change : change.substr(0, equal)},
                            {"value", unset || equal == std::string::npos
                                          ? ""
                                          : change.substr(equal + 1)},
                            {"unset", unset}}})}});
      }
      result = SessionControl(session, request);
      if (session.context.channel == nullptr) {
        printf("%s\n", TerminalSafe(JsonDump(result, 2)).c_str());
        fflush(stdout);
      }
      return false;
    }
    case SlashCommandId::kHttp: {
      auto exchanges = session.ActiveAgent().HttpExchanges();
      if (exchanges.empty()) {
        result = {{"error", "No HTTP exchange captured"}};
      } else {
        size_t index = exchanges.size();
        std::string part = "request";
        std::istringstream input(command.argument);
        if (!command.argument.empty()) input >> index >> part;
        if (index == 0 || index > exchanges.size() ||
            (part != "request" && part != "response")) {
          result = {{"error", "Use /http INDEX request|response"}};
        } else {
          result = exchanges[index - 1];
          {
            printf("%s\n", TerminalSafe(JsonDump(result, 2)).c_str());
            size_t offset = 0;
            for (;;) {
              auto page = ReadPrivateArtifact(
                  JsonValue(result, (part + "_path").c_str(), ""), offset);
              printf("%s", TerminalSafe(JsonValue(page, "text", JsonDump(page)))
                               .c_str());
              if (!JsonValue(page, "more", false)) break;
              offset = JsonValue(page, "next", offset);
            }
            printf("\n");
            fflush(stdout);
          }
          return false;
        }
      }
      {
        printf("%s\n", JsonDump(result).c_str());
      }
      return false;
    }
    case SlashCommandId::kYolo:
      result = PermissionControl(
          session.context, {{"mode", ApprovalIsAutomatic() ? "ask" : "yolo"}});
      session.ActiveAgent().ApprovalChanged();
      printf(
          "%s· yolo %s%s\n", DIM(),
          ApprovalIsAutomatic() ? "ON — automatic ordinary approvals" : "off",
          RST());
      break;
    case SlashCommandId::kCompact:
      HandleCompact(session);
      break;
    case SlashCommandId::kContext:
      HandleContext(session);
      break;
    case SlashCommandId::kCost:
      HandleCost(session);
      break;
    case SlashCommandId::kPrompt:
      result = PromptCommand(command.argument, [&session](const json& request) {
        return session.ActiveAgent().PromptConfiguration(request);
      });
      if (!session.context.channel) {
        printf("%s\n", TerminalSafe(result.contains("error")
                                        ? JsonValue(result, "error", "")
                                        : JsonValue(result, "effective", ""))
                           .c_str());
      }
      return false;
    case SlashCommandId::kMemory:
    case SlashCommandId::kSkills:
    case SlashCommandId::kSchedule:
      result = ManagementCommand(
          command.spec->id == SlashCommandId::kMemory   ? "memory"
          : command.spec->id == SlashCommandId::kSkills ? "skills"
                                                        : "schedule",
          command.argument);
      if (!session.context.channel) {
        printf("%s\n", TerminalSafe(JsonDump(result, 2)).c_str());
      }
      return false;
    case SlashCommandId::kTools:
      HandleTools(session);
      break;
    case SlashCommandId::kStatus:
      HandleStatus(session);
      break;
    case SlashCommandId::kDebugConfig:
      HandleDebugConfig(session, command.argument);
      break;
    case SlashCommandId::kAttach:
      HandleAttach(session, command.argument);
      break;
    case SlashCommandId::kProcesses:
    case SlashCommandId::kAgents:
      result = ActivityCommand(session, command);
      printf("%s", TerminalSafe(ActivityText(result)).c_str());
      fflush(stdout);
      return false;
    case SlashCommandId::kPeers:
      result = SessionSlashPeers();
      printf("%s", TerminalSafe(SessionText(result)).c_str());
      fflush(stdout);
      return false;
    case SlashCommandId::kTell: {
      result = SessionSlashTell(command.argument);
      printf("%s\n",
             TerminalSafe(JsonValue(result, "output",
                                    JsonValue(result, "error", "")))
                 .c_str());
      fflush(stdout);
      return false;
    }
    case SlashCommandId::kLink: {
      result = SessionSlashLink(command.argument);
      printf("%s\n",
             TerminalSafe(JsonValue(result, "output",
                                    JsonValue(result, "error", "")))
                 .c_str());
      fflush(stdout);
      return false;
    }
    case SlashCommandId::kDiff:
    case SlashCommandId::kInit:
    case SlashCommandId::kReview:
      // SlashCommandPrompt turns these into an ordinary turn before the
      // dispatcher ever sees them.
      break;
  }
  // Notices above are written with bare printf; the interactive composer owns
  // stdout and only sees what has left the buffer.
  fflush(stdout);
  result = CommandResult(session, command);
  return false;
}

}  // namespace uagent
