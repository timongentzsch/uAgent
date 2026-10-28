// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "include/app/config_proposal.h"
#include "include/app/panel.h"
#include "include/cli/cli.h"
#include "include/core/config_registry.h"
#include "include/core/effective_config.h"
#include "include/core/fs.h"
#include "include/core/strings.h"
#include "include/core/style.h"
#include "include/core/term.h"
#include "include/core/verbosity.h"
#include "include/ui/interactive.h"
#include "include/ui/sessions.h"
#include "src/app/session_terminal_internal.h"

namespace uagent::session {

Terminal::Command Terminal::HandleCommand(const ParsedSlashCommand& slash,
                                          bool deciding,
                                          std::vector<std::string>& files) {
  if (!slash.spec) return Command::kPass;
  if (const std::optional<Command> done = SessionCommand(slash)) return *done;
  if (deciding) return Command::kPass;
  return ConversationCommand(slash, files);
}

std::optional<Terminal::Command> Terminal::SessionCommand(
    const ParsedSlashCommand& slash) {
  const std::string& argument = slash.argument;
  switch (slash.spec->id) {
    case SlashCommandId::kQuit:
      return Command::kLeave;
    case SlashCommandId::kClear:
      // Screen only: the session keeps running underneath.
      if (raw_) {
        region_.Clear();
        output_.Write(ClearScreen());
      } else {
        fputs(ClearScreen(), stdout);
        fflush(stdout);
      }
      return Command::kDone;
    case SlashCommandId::kBoard:
      WriteTerminalRecord(TerminalSafe(PanelBoard(Folder())));
      return Command::kDone;
    case SlashCommandId::kPanel:
    case SlashCommandId::kOpen: {
      std::string target;
      if (slash.spec->id == SlashCommandId::kPanel) {
        next_folder_ = Folder();
        target = PanelPath(next_folder_);
      } else if (target = MatchFolderSession(argument); target.empty()) {
        WriteTerminalRecord(
            Note(Tone::kNeutral, "no unique session in /board matches \"" +
                                     TerminalSafe(argument) + "\""));
        return Command::kDone;
      }
      return Leave(target, "switching sessions");
    }
    case SlashCommandId::kRestart:
      // A fresh runtime for this conversation, e.g. after a setting that
      // needs a restart; its history is kept.
      return Leave("/restart", "restarting");
    case SlashCommandId::kReset:
      return Leave("/reset", "switching sessions");
    case SlashCommandId::kSessions: {
      // Guarded before the picker: never abandon a running turn.
      if (TurnActive("switching sessions")) return Command::kDone;
      std::string target =
          argument.empty() ? PickSession() : MatchSessionPrefix(argument);
      if (!argument.empty() && target.empty()) {
        WriteTerminalRecord(Note(
            Tone::kNeutral,
            "no unique session matches \"" + TerminalSafe(argument) + "\""));
      }
      if (target.empty()) return Command::kDone;
      return Leave(target, "switching sessions");
    }
    default:
      return std::nullopt;
  }
}

Terminal::Command Terminal::ConversationCommand(
    const ParsedSlashCommand& slash, std::vector<std::string>& files) {
  const std::string& argument = slash.argument;
  switch (slash.spec->id) {
    case SlashCommandId::kAttach: {
      // Bare /attach lists them (host). Terminals quote dropped paths
      // containing spaces; strip one surrounding pair so a drop Just Works.
      if (argument.empty()) return Command::kPass;
      const std::string file = Unquote(argument);
      if (file == "clear") {
        files.clear();
      } else {
        files.push_back(CanonicalAccessPath(file).string());
      }
      return Command::kDone;
    }
    case SlashCommandId::kFork: {
      const ForkArgument fork = ParseForkArgument(argument);
      Send({{"kind", "fork"}, {"title", fork.title}, {"turn", fork.turn}});
      return Command::kDone;
    }
    case SlashCommandId::kRewind: {
      // Forks before message N and opens the fork with that message to
      // edit; the original stays. Bare /rewind lists the numbers (host).
      if (argument.empty()) return Command::kPass;
      const ForkArgument parsed = ParseForkArgument(argument);
      if (parsed.turn <= 0 || !parsed.title.empty()) {
        WriteTerminalRecord(
            Note(Tone::kNeutral, "usage: /rewind N (bare /rewind lists them)"));
        return Command::kDone;
      }
      rewinding_ = true;
      Send({{"kind", "fork"}, {"turn", parsed.turn}});
      return Command::kDone;
    }
    case SlashCommandId::kBtw:
      if (argument.empty()) return Command::kPass;
      Send({{"kind", "side"}, {"operation", "create"}, {"text", argument}});
      return Command::kDone;
    case SlashCommandId::kVerbosity:
      return SetVerbosity(argument);
    case SlashCommandId::kShare:
      Send({{"kind", "share"}});
      return Command::kDone;
    default:
      return Command::kPass;
  }
}

Terminal::Command Terminal::SetVerbosity(const std::string& argument) {
  if (argument.empty()) {
    WriteTerminalRecord(
        Note(Tone::kNeutral,
             "verbosity " + std::string(Detail().level) +
                 (pinned_ ? " · this terminal only (--verbosity)" : "")));
    return Command::kDone;
  }
  if (std::ranges::find(kVerbosityLevels, argument) ==
      std::end(kVerbosityLevels)) {
    WriteTerminalRecord(
        Note(Tone::kNeutral, "usage: /verbosity minimal|default|full"));
    return Command::kDone;
  }
  // The level is one setting for every terminal and browser; a pinned
  // terminal changes only itself.
  if (!pinned_) {
    const json saved = ConfigurationControl(
        {{"operation", "apply"},
         {"changes",
          json::array({{{"key", kVerbositySetting}, {"value", argument}}})}},
        ConfigManager::Capture({}));
    if (const std::string error = JsonValue(saved, "error", "");
        !error.empty()) {
      WriteTerminalRecord(Note(Tone::kError, TerminalSafe(error)));
      return Command::kDone;
    }
  }
  Restyle(DetailFor(argument));
  return Command::kDone;
}

std::string Terminal::Folder() const {
  const json header = SessionHeader(path_);
  std::string folder = JsonValue(ThreadRole(header), "folder", "");
  if (!folder.empty()) return folder;
  return JsonValue(header, kSessionHeaderCwd, CanonicalCwd());
}

std::string Terminal::MatchFolderSession(const std::string& argument) const {
  const std::string wanted = AsciiLower(Trim(argument));
  if (wanted.empty()) return "";
  return UniqueSession(FolderSessions(Folder()), [&](const SessionInfo& info) {
    return SessionIdFor(info.path).starts_with(wanted) ||
           AsciiLower(info.title).find(wanted) != std::string::npos;
  });
}

bool Terminal::TurnActive(const std::string& before) {
  std::lock_guard lock(mutex_);
  if (waiting_.empty() && !running_) return false;
  WriteTerminalRecord(
      Note(Tone::kNeutral, "turn active; interrupt it before " + before));
  return true;
}

Terminal::Command Terminal::Leave(const std::string& target,
                                  const std::string& before) {
  if (TurnActive(before)) return Command::kDone;
  std::lock_guard lock(mutex_);
  next_ = target;
  return Command::kLeave;
}

void Terminal::Restyle(const DetailPolicy& detail) {
  detail_ = &detail;
  // The reader draws, here as everywhere: it holds the tree, and a redraw
  // written from the input thread could fill the pipe only it drains.
  Send({{"kind", "refresh"}});
  wake_.Wake();
}
}  // namespace uagent::session
