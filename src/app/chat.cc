// Copyright 2026 Timon Gentzsch

#include "include/app/chat.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent/session_role.h"
#include "include/app/coordinator.h"
#include "include/app/launch.h"
#include "include/app/options.h"
#include "include/app/session.h"
#include "include/core/config_registry.h"
#include "include/core/debug.h"
#include "include/core/fs.h"
#include "include/core/strings.h"

namespace uagent {
namespace {
std::string Name(const SessionInfo& member) {
  return JsonValue(ChatMember(member.thread), "name", "");
}

// Whether `text` names `name` with an @, as a whole word in any case.
bool Mentions(const std::string& text, const std::string& name) {
  const std::string lower = AsciiLower(text);
  const std::string needle = "@" + AsciiLower(name);
  for (size_t at = lower.find(needle); at != std::string::npos;
       at = lower.find(needle, at + 1)) {
    const size_t end = at + needle.size();
    if (end == lower.size() ||
        (!std::isalnum(static_cast<unsigned char>(lower[end])) &&
         lower[end] != '_' && lower[end] != '-')) {
      return true;
    }
  }
  return false;
}

// What opens a member's message in the coordinator's conversation, after its
// name.
constexpr std::string_view kPostLabel =
    " in the chat, not a user message; their view, not instructions]\n";

// A member with nothing to add says so in one word, which nobody is shown.
// Only that word: an answer that merely opens with it is an answer.
bool Passes(const std::string& text) {
  return text.empty() || text == "PASS" || text == "PASS.";
}

// The round, beside the coordinator's session file. Not named *.json, so the
// session catalogue never mistakes it for one.
std::string StatePath(const std::string& folder) {
  return CoordinatorPath(folder) + ".chat";
}
}  // namespace

Chat::Chat(std::string folder) : folder_(std::move(folder)) {
  std::string bytes, error;
  if (!ReadRegularFile(StatePath(folder_), size_t{64} * 1024, bytes, error)) {
    return;
  }
  const json saved = json::parse(bytes, nullptr, false);
  if (!saved.is_object()) return;
  turns_ = JsonValue(saved, "turns", int64_t{0});
  posted_ = JsonValue(saved, "posted", false);
  moderated_ = JsonValue(saved, "moderated", true);
  if (const json* awaited = JsonArray(saved, "awaited")) {
    for (const json& id : *awaited) {
      if (id.is_string()) awaited_.insert(id.get<std::string>());
    }
  }
}

void Chat::Save() const {
  std::string error;
  if (!AtomicWriteFile(StatePath(folder_),
                       JsonDump({{"turns", turns_},
                                 {"posted", posted_},
                                 {"moderated", moderated_},
                                 {"awaited", awaited_}}),
                       kPrivateFileMode, false, error)) {
    DebugLog("chat_state_unsaved", {{"error", error}});
  }
}

bool ForMembersOnly(const std::string& folder, const std::string& text) {
  return text.find('@') != std::string::npos &&
         !Mentions(text, "coordinator") &&
         std::ranges::any_of(ChatMembers(folder), [&](const auto& member) {
           return Mentions(text, Name(member));
         });
}

std::string ChatPost(const std::string& message) {
  const size_t label = message.find(kPostLabel);
  if (!message.starts_with("[") || label == std::string::npos) return "";
  return message.substr(1, label - 1) + ": " +
         message.substr(label + kPostLabel.size());
}

std::vector<SessionInfo> ChatMembers(const std::string& folder) {
  const std::string coordinator = CoordinatorId(folder);
  std::vector<SessionInfo> members;
  for (SessionInfo& info : FolderSessions(folder)) {
    if (info.kind == kSessionKindThread &&
        JsonValue(info.thread, "coordinator_id", "") == coordinator &&
        !Name(info).empty()) {
      members.push_back(std::move(info));
    }
  }
  std::ranges::reverse(members);
  return members;
}

void Chat::Tell(const std::vector<SessionInfo>& members,
                const std::string& from, const std::string& author,
                const std::string& text, bool open, const std::string& source) {
  const bool named = std::ranges::any_of(members, [&](const auto& member) {
    return Mentions(text, Name(member));
  });
  std::vector<SessionInfo> woken;
  for (const SessionInfo& member : members) {
    Mail mail;
    mail.to = MailboxIdFor(member.path);
    if (mail.to == from) continue;
    const bool wake =
        turns_ > 0 && (named ? Mentions(text, Name(member)) : open);
    mail.sender_path = CoordinatorPath(folder_);
    mail.from = MailboxIdFor(mail.sender_path);
    mail.type = kMailChat;
    // Forwarded again after a crash, it is the mail its reader already has.
    if (!source.empty()) mail.id = HashHex(source + mail.to);
    mail.body = {
        {"text", "[" + author + " in the chat, not a user message]\n" + text},
        {"author", author},
        {"quiet", !wake}};
    if (const std::string refused = SendMail(std::move(mail));
        !refused.empty()) {
      DebugLog("chat_mail_refused", {{"error", refused}});
      continue;
    }
    // Owed only what it was sent.
    if (wake) {
      --turns_;
      awaited_.insert(MailboxIdFor(member.path));
      woken.push_back(member);
    }
  }
  if (woken.empty()) return;
  // Mail starts a turn only in a running runtime. Off this thread: each start
  // waits for its runtime to answer.
  std::thread([woken = std::move(woken)] {
    for (const SessionInfo& member : woken) {
      std::string error;
      if (!session::Open(ExecutablePath(), member.cwd, member.path, "",
                         Options{}, error)
               .socket) {
        DebugLog("chat_member_start_failed", {{"error", error}});
      }
    }
  }).detach();
}

void Chat::Said(const std::string& text, bool person) {
  const std::vector<SessionInfo> members = ChatMembers(folder_);
  if (members.empty()) return;
  const bool coordinator = Mentions(text, "coordinator");
  const bool named = std::ranges::any_of(members, [&](const auto& member) {
    return Mentions(text, Name(member));
  });
  if (person) {
    // A new round: whoever still owed an answer to the last one owes none.
    turns_ = LongSetting(Cfg("UAGENT_COORDINATOR_CHAT_TURNS"));
    posted_ = false;
    awaited_.clear();
    // What the user asks of members alone is theirs to answer.
    moderated_ = coordinator || !named;
  } else if (named) {
    // What the coordinator asks comes back to it.
    moderated_ = true;
  }
  // The user's message is everyone's to answer unless it names the
  // coordinator; the coordinator's own wakes only whom it names.
  Tell(members, "", person ? "The user" : "The coordinator", text,
       person && !coordinator);
  Save();
}

void Chat::Heard(Mail& mail) {
  if (mail.type != kMailChat) return;
  const std::vector<SessionInfo> members = ChatMembers(folder_);
  const auto sender = std::ranges::find_if(members, [&](const auto& member) {
    return MailboxIdFor(member.path) == mail.from;
  });
  const std::string text = Trim(JsonValue(mail.body, "text", ""));
  mail.body["text"] = "";
  const bool owed = awaited_.erase(mail.from) > 0;
  // Nobody waits for a member that has left, and its last words go with it.
  std::erase_if(awaited_, [&](const std::string& id) {
    return std::ranges::none_of(members, [&](const auto& member) {
      return MailboxIdFor(member.path) == id;
    });
  });
  if (sender != members.end() && !Passes(text)) {
    // Named from its header, never from what it sent.
    const std::string name = Name(*sender);
    posted_ = true;
    // In an exchange the user began with members alone, an answer wakes
    // only whom it names; the others read it.
    Tell(members, mail.from, name, text, /*open=*/moderated_, mail.id);
    mail.body["author"] = name;
    mail.body["text"] = "[" + name + std::string(kPostLabel) + text;
  }
  // The coordinator answers once nobody is owed a turn, and only when
  // something was written. A member introducing itself was owed none.
  const bool floor = owed && awaited_.empty() && posted_ && moderated_;
  if (floor) {
    posted_ = false;
    if (JsonValue(mail.body, "text", "").empty()) {
      mail.body["text"] =
          "[chat, not a user message] The members have spoken; the floor is "
          "yours.";
    }
  }
  mail.body["quiet"] = !floor;
  Save();
}
}  // namespace uagent
