// Copyright 2026 Timon Gentzsch

#include "include/app/commands.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>

#include "include/agent/session_store.h"
#include "include/agent/session_view.h"
#include "include/app/control.h"
#include "include/app/prompt_control.h"
#include "include/app/self_description.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/steering.h"
#include "include/core/strings.h"
#include "include/core/term.h"
#include "include/providers.h"
#include "include/tools/session.h"
#include "src/app/commands_internal.h"

namespace uagent {

void CommandReply::Print(const char* format, ...) {
  constexpr size_t kOutputBytes = KiB(64);
  if (output.size() >= kOutputBytes) return;
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  const int needed = vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  if (needed > 0) {
    const size_t start = output.size();
    const size_t count =
        std::min(static_cast<size_t>(needed), kOutputBytes - start);
    output.resize(start + count + 1);
    vsnprintf(output.data() + start, count + 1, format, args);
    output.resize(start + count);
  }
  va_end(args);
}

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
    PermissionOverride saved = PermissionOverride::kDefault;
    const std::string mode = JsonValue(settings, "permissions", "");
    if (!mode.empty()) ParsePermissionOverride(mode, saved);
    session.context.permission_override.store(saved);
    PermissionControl(session.context, json::object());
    session.ActiveAgent().ApprovalChanged();
  }
  const json saved_tools = JsonValue(settings, "tools", json::object());
  if (!saved_tools.empty()) {
    session.ActiveAgent().RestoreToolSelection(saved_tools);
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

CommandReply RunSlashCommand(AppSession& session,
                             const ParsedSlashCommand& command) {
  CommandReply reply;
  json& result = reply.result;
  switch (command.spec->id) {
    case SlashCommandId::kQuit:
    case SlashCommandId::kReset:
    case SlashCommandId::kRestart:
    case SlashCommandId::kSessions:
    case SlashCommandId::kFork:
    case SlashCommandId::kVerbose:
    case SlashCommandId::kBtw:
      result = {{"error", "this command belongs to the client"}};
      return reply;
    case SlashCommandId::kClear:
      reply.Print("\033[H\033[2J");
      return reply;
    case SlashCommandId::kRewind: {
      // The clients fork before message N; bare, this lists the numbers.
      if (!Trim(command.argument).empty()) {
        result = {{"error", "this command belongs to the client"}};
        return reply;
      }
      const Conversation& history = session.ActiveAgent().History();
      const int64_t count = history.UserTurns();
      for (int64_t turn = 1; turn <= count; ++turn) {
        reply.Print(
            "%s%3" PRId64 "%s  %s\n", DIM(), turn, RST(),
            TerminalSafe(FirstLine(history.UserMessageText(turn))).c_str());
      }
      reply.Print(
          "%s· /rewind N forks before message N and opens it with "
          "that message to edit; the original stays as it is%s\n",
          DIM(), RST());
      return reply;
    }
    case SlashCommandId::kShare: {
      if (!command.argument.empty()) {
        result = {{"error", "usage: /share"}};
        return reply;
      }
      result = SessionControl(session, {{"kind", "share"}});
      return reply;
    }
    case SlashCommandId::kVariant:
      HandleVariant(session, command.argument, reply);
      break;
    case SlashCommandId::kHelp: {
      result =
          DescribeSelf(SelfTopic::kCommands, "", DescriptionInputs(session));
      size_t width = 0;
      for (const auto& row : result["commands"]) {
        width = std::max(width, JsonValue(row, "usage", "").size());
      }
      reply.Print("%scommands%s\n", BOLD(), RST());
      for (const auto& row : result["commands"]) {
        reply.Print("  %s%-*s%s  %s%s%s\n", BOLD(), static_cast<int>(width),
                    JsonValue(row, "usage", "").c_str(), RST(), DIM(),
                    JsonValue(row, "description", "").c_str(), RST());
      }
      break;
    }
    case SlashCommandId::kModels:
      HandleModels(session, command.argument, reply);
      break;
    case SlashCommandId::kModel:
      HandleModel(session, command.argument, reply);
      break;
    case SlashCommandId::kEffort:
      HandleEffort(session, command.argument, reply);
      break;
    case SlashCommandId::kPermissions:
      if (command.argument == "rules" ||
          command.argument.starts_with("forget")) {
        HandlePermissionRules(command.argument, reply);
        return reply;
      }
      result = PermissionControl(session.context, {{"mode", command.argument}});
      session.ActiveAgent().ApprovalChanged();
      return reply;
    case SlashCommandId::kRename:
      if (Trim(command.argument).empty()) {
        result = {{"error", "usage: /rename TITLE"}};
      } else {
        session.ActiveAgent().Rename(Trim(command.argument));
        result = {{"title", Trim(command.argument)}};
      }
      return reply;
    case SlashCommandId::kConfig:
      HandleConfig(session, command.argument, reply);
      return reply;
    case SlashCommandId::kMcp:
      HandleMcp(session, command.argument, reply);
      return reply;
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
          reply.Print("%s\n", TerminalSafe(JsonDump(result, 2)).c_str());
          size_t offset = 0;
          for (;;) {
            auto page = ReadPrivateArtifact(
                JsonValue(result, (part + "_path").c_str(), ""), offset);
            reply.Print(
                "%s",
                TerminalSafe(JsonValue(page, "text", JsonDump(page))).c_str());
            if (!JsonValue(page, "more", false)) break;
            offset = JsonValue(page, "next", offset);
          }
          reply.Print("\n");
          return reply;
        }
      }
      reply.Print("%s\n", JsonDump(result).c_str());
      return reply;
    }
    case SlashCommandId::kYolo:
      result = PermissionControl(session.context,
                                 {{"mode", ApprovalIsYolo() ? "ask" : "yolo"}});
      session.ActiveAgent().ApprovalChanged();
      reply.Print(
          "%s· yolo %s%s\n", DIM(),
          ApprovalIsYolo() ? "ON — automatic ordinary approvals" : "off",
          RST());
      break;
    case SlashCommandId::kCompact:
      session.ActiveAgent().Compact();
      SteeringState().Take();
      break;
    case SlashCommandId::kContext:
      HandleContext(session, reply);
      break;
    case SlashCommandId::kCost:
      HandleCost(session, reply);
      break;
    case SlashCommandId::kPrompt:
      result = PromptCommand(command.argument, [&session](const json& request) {
        return session.ActiveAgent().PromptConfiguration(request);
      });
      return reply;
    case SlashCommandId::kMemory:
    case SlashCommandId::kSkills:
    case SlashCommandId::kSchedule:
      result = ManagementCommand(
          command.spec->id == SlashCommandId::kMemory   ? "memory"
          : command.spec->id == SlashCommandId::kSkills ? "skills"
                                                        : "schedule",
          command.argument);
      return reply;
    case SlashCommandId::kTools:
      HandleTools(session, command.argument, reply);
      return reply;
    case SlashCommandId::kStatus:
      HandleStatus(session, reply);
      break;
    case SlashCommandId::kDebugConfig:
      HandleDebugConfig(session, command.argument, reply);
      break;
    case SlashCommandId::kAttach:
      HandleAttach(session, command.argument, reply);
      result = {{"attachments", json::array()}};
      for (const Attachment& attachment : session.attachments) {
        result["attachments"].push_back({{"name", attachment.name},
                                         {"path", attachment.path},
                                         {"mime", attachment.mime}});
      }
      break;
    case SlashCommandId::kProcesses:
    case SlashCommandId::kAgents:
      result = ActivityCommand(session, command);
      reply.Print("%s", TerminalSafe(ActivityText(result)).c_str());
      return reply;
    case SlashCommandId::kPeers:
      result = SessionSlashPeers();
      reply.Print("%s", TerminalSafe(SessionText(result)).c_str());
      return reply;
    case SlashCommandId::kTell: {
      result = SessionSlashTell(command.argument);
      reply.Print("%s\n",
                  TerminalSafe(JsonValue(result, "output",
                                         JsonValue(result, "error", "")))
                      .c_str());
      return reply;
    }
    case SlashCommandId::kLink: {
      result = SessionSlashLink(command.argument);
      reply.Print("%s\n",
                  TerminalSafe(JsonValue(result, "output",
                                         JsonValue(result, "error", "")))
                      .c_str());
      return reply;
    }
    case SlashCommandId::kDiff:
    case SlashCommandId::kInit:
    case SlashCommandId::kReview:
      // SlashCommandPrompt turns these into an ordinary turn before the
      // dispatcher ever sees them.
      break;
  }
  if (result.is_null()) result = json::object();
  return reply;
}

}  // namespace uagent
