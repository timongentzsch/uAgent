// Copyright 2026 Timon Gentzsch

#include "include/app/chat.h"

#include <algorithm>
#include <cctype>
#include <set>
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

// The coordinator among the participants.
constexpr const char* kSelf = "coordinator";
// Messages one participant may post before the user writes again.
constexpr int kPostsEach = 2;

// The round, beside the coordinator's session file. Not named *.json, so the
// session catalogue never mistakes it for one.
std::string StatePath(const std::string& folder) {
  return CoordinatorPath(folder) + ".chat";
}

json ReadState(const std::string& folder) {
  std::string bytes, error;
  if (!ReadRegularFile(StatePath(folder), size_t{64} * 1024, bytes, error)) {
    return json::object();
  }
  json saved = json::parse(bytes, nullptr, false);
  return saved.is_object() ? saved : json::object();
}

// The participants in `ids` by name, as a sentence lists them.
std::string Names(const std::vector<SessionInfo>& members,
                  const std::set<std::string>& ids) {
  std::string names;
  for (const SessionInfo& member : members) {
    if (ids.contains(MailboxIdFor(member.path))) {
      names += (names.empty() ? "" : ", ") + Name(member);
    }
  }
  if (ids.contains(kSelf)) {
    names += (names.empty() ? "" : ", ") + std::string(kSelf);
  }
  return names;
}

bool WordChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}
}  // namespace

std::set<std::string> Addressed(const std::string& text,
                                const std::vector<std::string>& names) {
  const std::string lower = AsciiLower(text);
  size_t at = 0;
  const auto space = [&] {
    while (at < lower.size() &&
           std::isspace(static_cast<unsigned char>(lower[at]))) {
      ++at;
    }
  };
  // The longest name that stands at `at` as a whole word, stepped over.
  const auto name = [&]() -> const std::string* {
    const std::string* found = nullptr;
    for (const std::string& candidate : names) {
      const size_t end = at + candidate.size();
      if (!candidate.empty() &&
          lower.compare(at, candidate.size(), AsciiLower(candidate)) == 0 &&
          (end >= lower.size() || !WordChar(lower[end])) &&
          (!found || candidate.size() > found->size())) {
        found = &candidate;
      }
    }
    if (found) at += found->size();
    return found;
  };
  std::set<std::string> addressed;
  for (;;) {
    space();
    const std::string* next = name();
    if (!next) return {};
    addressed.insert(*next);
    space();
    // Another name follows a comma, "and" or "&"; anything else ends them.
    const size_t end = at;
    if (lower[at] == ',' || lower[at] == '&') {
      ++at;
    } else if (lower.compare(at, 3, "and") == 0 && !WordChar(lower[at + 3])) {
      at += 3;
    } else {
      break;
    }
    space();
    const size_t resume = at;
    at = name() ? resume : end;
    if (at == end) break;
  }
  return lower[at] == ',' || lower[at] == ':' ? addressed
                                              : std::set<std::string>();
}

Chat::Chat(std::string folder, std::function<void(const std::string&)> wake)
    : folder_(std::move(folder)), wake_(std::move(wake)) {
  const json saved = ReadState(folder_);
  each_ = JsonValue(saved, "each", int64_t{0});
  round_ = JsonValue(saved, "round", int64_t{0});
  for (const char* list : {"typing", "waiting"}) {
    const json* ids = JsonArray(saved, list);
    if (!ids) continue;
    for (const json& id : *ids) {
      if (!id.is_string()) continue;
      if (list == std::string_view("typing")) {
        typing_.insert(id.get<std::string>());
      } else {
        waiting_.push_back(id.get<std::string>());
      }
    }
  }
  if (const json* seats = JsonObject(saved, "seats")) {
    for (const auto& [who, seat] : seats->items()) {
      seats_[who] = {JsonValue(seat, "turns", int64_t{0}),
                     JsonValue(seat, "posts", 0),
                     JsonValue(seat, "waited", false)};
    }
  }
  if (const json* heard = JsonObject(saved, "heard")) {
    for (const auto& [mail, read] : heard->items()) {
      if (read.is_number_integer()) heard_[mail] = read.get<int>();
    }
  }
  // A runtime that starts again has no turn of its own under way.
  typing_.erase(kSelf);
}

void Chat::Save() const {
  json seats = json::object();
  for (const auto& [who, seat] : seats_) {
    seats[who] = {
        {"turns", seat.turns}, {"posts", seat.posts}, {"waited", seat.waited}};
  }
  std::string error;
  if (!AtomicWriteFile(StatePath(folder_),
                       JsonDump({{"each", each_},
                                 {"round", round_},
                                 {"typing", typing_},
                                 {"waiting", waiting_},
                                 {"heard", heard_},
                                 {"seats", std::move(seats)}}),
                       kPrivateFileMode, false, error)) {
    DebugLog("chat_state_unsaved", {{"error", error}});
  }
}

namespace {
// The participants `text` opens with: a member as its mailbox, the
// coordinator as kSelf.
std::set<std::string> Named(const std::vector<SessionInfo>& members,
                            const std::string& text) {
  std::vector<std::string> names{kSelf};
  for (const SessionInfo& member : members) names.push_back(Name(member));
  const std::set<std::string> opens = Addressed(text, names);
  std::set<std::string> ids;
  if (opens.contains(kSelf)) ids.insert(kSelf);
  for (const SessionInfo& member : members) {
    if (opens.contains(Name(member))) ids.insert(MailboxIdFor(member.path));
  }
  return ids;
}
}  // namespace

bool ForMembersOnly(const std::string& folder, const std::string& text) {
  const std::set<std::string> opens = Named(ChatMembers(folder), text);
  return !opens.empty() && !opens.contains(kSelf);
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

std::set<std::string> ChatTyping(const std::string& folder) {
  std::set<std::string> typing;
  const json saved = ReadState(folder);
  if (const json* ids = JsonArray(saved, "typing")) {
    for (const json& id : *ids) {
      if (id.is_string() && id != kSelf) typing.insert(id.get<std::string>());
    }
  }
  return typing;
}

std::string Chat::Context() const {
  const std::vector<SessionInfo> members = ChatMembers(folder_);
  if (members.empty()) return "";
  std::string names;
  for (const SessionInfo& member : members) {
    names += (names.empty() ? "" : ", ") + Name(member);
  }
  std::set<std::string> others;
  for (const SessionInfo& member : members) {
    if (typing_.contains(MailboxIdFor(member.path)) && AnswerAhead(member)) {
      others.insert(MailboxIdFor(member.path));
    }
  }
  const std::string typing = Names(members, others);
  return "\n## chat\nMembers: " + names +
         (typing.empty() ? "" : "\nTyping: " + typing) + "\n";
}

Chat::Answer Chat::Finished(const std::string& who, const std::string& text) {
  typing_.erase(who);
  Seat& seat = seats_[who];
  Answer answer = text.empty() || text == "PASS" || text == "PASS."
                      ? Answer::kPass
                  : text == "WAIT" || text == "WAIT." ? Answer::kWait
                                                      : Answer::kMessage;
  // Waiting twice running, or for nobody, is having nothing to add.
  if (answer == Answer::kWait && (seat.waited || typing_.empty())) {
    answer = Answer::kPass;
  }
  // A message past the limit, from a turn started before the limit was
  // reached, is not posted.
  if (answer == Answer::kMessage && seat.posts >= kPostsEach) {
    answer = Answer::kPass;
  }
  seat.waited = answer == Answer::kWait;
  if (answer == Answer::kWait) waiting_.push_back(who);
  if (answer == Answer::kMessage) ++seat.posts;
  return answer;
}

bool Chat::Wakes(const std::string& who, bool some, bool one) {
  // Whoever waited, waited for this.
  const bool waited = std::erase(waiting_, who) > 0;
  // Whoever is writing what may be its last message reads this one after.
  Seat& seat = seats_[who];
  const int ahead = seat.posts + (typing_.contains(who) ? 1 : 0);
  if (seat.turns >= each_ || ahead >= kPostsEach) return false;
  if (!waited && some && !one) return false;
  ++seat.turns;
  typing_.insert(who);
  return true;
}

json Chat::Deliver(const std::string& from, const std::string& author,
                   const std::string& text, const json& to,
                   const std::string& source) {
  const std::vector<SessionInfo> members = ChatMembers(folder_);
  Settle(members);

  // Whom it is for: those it opens with, else whoever asked its author.
  const std::set<std::string> opens = Named(members, text);
  const bool answers = opens.empty() && to.is_object() && to.contains("asker");
  std::set<std::string> some = opens;
  if (answers) some.insert(JsonValue(to, "asker", ""));
  const bool narrowed = !opens.empty() || answers;
  // What a reader it wakes answers to: only one it names answers its author.
  const auto asks = [&](const std::string& reader) {
    json re = {{"round", round_}};
    if (opens.contains(reader)) re["asker"] = from;
    return re;
  };
  // Settled for everyone first, so each is told who else is answering.
  const bool late = Late(to);
  std::vector<const SessionInfo*> woken;
  for (const SessionInfo& member : members) {
    const std::string id = MailboxIdFor(member.path);
    if (!late && id != from && Wakes(id, narrowed, some.contains(id))) {
      woken.push_back(&member);
    }
  }
  const bool self = !late && from != kSelf && !from.empty() &&
                    Wakes(kSelf, narrowed, some.contains(kSelf));
  std::vector<SessionInfo> started;
  for (const SessionInfo& member : members) {
    Mail mail;
    mail.to = MailboxIdFor(member.path);
    if (mail.to == from) continue;
    const bool wake = std::ranges::find(woken, &member) != woken.end();
    std::set<std::string> others = typing_;
    others.erase(mail.to);
    const std::string typing = Names(members, others);
    mail.sender_path = CoordinatorPath(folder_);
    mail.from = MailboxIdFor(mail.sender_path);
    mail.type = kMailChat;
    // Forwarded again after a crash, it is the mail its reader already has.
    if (!source.empty()) mail.id = HashHex(source + mail.to);
    mail.body = {
        {"text",
         author + ": " + text +
             (typing.empty() || !wake ? "" : "\n\n(typing: " + typing + ")")},
        {"author", author},
        {"re", asks(mail.to)},
        {"quiet", !wake}};
    const std::string reader = mail.to;
    if (const std::string refused = SendMail(std::move(mail));
        !refused.empty()) {
      DebugLog("chat_mail_refused", {{"error", refused}});
      // Owed nothing it was not sent.
      if (wake) {
        --seats_[reader].turns;
        typing_.erase(reader);
      }
      continue;
    }
    if (wake) started.push_back(member);
  }
  if (!started.empty()) {
    // Mail starts a turn only in a running runtime. Off this thread: each
    // start waits for its runtime to answer.
    std::thread([started = std::move(started)] {
      for (const SessionInfo& member : started) {
        std::string error;
        if (!session::Open(ExecutablePath(), member.cwd, member.path, "",
                           Options{}, error)
                 .socket) {
          DebugLog("chat_member_start_failed", {{"error", error}});
        }
      }
    }).detach();
  }
  return self ? asks(kSelf) : json();
}

void Chat::Settle(const std::vector<SessionInfo>& members) {
  const auto member = [&](const std::string& id) {
    return std::ranges::find_if(members, [&](const auto& item) {
      return MailboxIdFor(item.path) == id;
    });
  };
  // Nobody waits for a member that has left, nor for one whose answer
  // nothing can bring any more: its runtime never started, or is gone.
  std::erase_if(typing_, [&](const std::string& id) {
    return id != kSelf &&
           (member(id) == members.end() || !AnswerAhead(*member(id)));
  });
  std::erase_if(waiting_, [&](const std::string& id) {
    return id != kSelf && member(id) == members.end();
  });
}

void Chat::Release(const std::vector<SessionInfo>& members) {
  Settle(members);
  const std::string note = "[chat, not a user message] Nobody is typing now.";
  // The first waiter that can still be woken; one that cannot is passed
  // over, so nobody waits behind it.
  while (typing_.empty() && !waiting_.empty()) {
    const std::string who = waiting_.front();
    waiting_.erase(waiting_.begin());
    Seat& seat = seats_[who];
    if (seat.turns >= each_) continue;
    if (who == kSelf) {
      ++seat.turns;
      typing_.insert(who);
      wake_(note);
      return;
    }
    const auto member = std::ranges::find_if(members, [&](const auto& item) {
      return MailboxIdFor(item.path) == who;
    });
    if (member == members.end()) continue;
    Mail mail;
    mail.to = who;
    mail.sender_path = CoordinatorPath(folder_);
    mail.from = MailboxIdFor(mail.sender_path);
    mail.type = kMailChat;
    mail.body = {{"text", note}, {"re", {{"round", round_}}}, {"quiet", false}};
    if (!SendMail(std::move(mail)).empty()) continue;
    ++seat.turns;
    typing_.insert(who);
    std::thread([member = *member] {
      std::string error;
      session::Open(ExecutablePath(), member.cwd, member.path, "", Options{},
                    error);
    }).detach();
  }
}

json Chat::Said(const std::string& text) {
  const std::vector<SessionInfo> members = ChatMembers(folder_);
  if (members.empty()) return json();
  // A new round: whoever still owed an answer to the last one owes none.
  each_ = LongSetting(Cfg("UAGENT_COORDINATOR_CHAT_TURNS"));
  round_ = NowMillis();
  typing_.clear();
  waiting_.clear();
  seats_.clear();
  heard_.clear();
  // The coordinator answers its user's message like any other, unless it is
  // for members alone.
  const std::set<std::string> opens = Named(members, text);
  json re;
  if (opens.empty() || opens.contains(kSelf)) {
    seats_[kSelf].turns = 1;
    typing_.insert(kSelf);
    re = {{"round", round_}};
    if (!opens.empty()) re["asker"] = "";
  }
  Deliver("", "user", text);
  Save();
  return re;
}

void Chat::Answered(const std::string& text, const json& to) {
  const std::vector<SessionInfo> members = ChatMembers(folder_);
  if (members.empty()) return;
  if (Finished(kSelf, Trim(text)) == Answer::kMessage) {
    Deliver(kSelf, kSelf, Trim(text), to);
  }
  Release(members);
  Save();
}

void Chat::Heard(Mail& mail) {
  if (mail.type != kMailChat) return;
  const std::vector<SessionInfo> members = ChatMembers(folder_);
  const auto sender = std::ranges::find_if(members, [&](const auto& member) {
    return MailboxIdFor(member.path) == mail.from;
  });
  std::string text = Trim(JsonValue(mail.body, "text", ""));
  // What it answers. One to an earlier round is read, and is nobody's turn
  // in this one.
  const json to = JsonValue(mail.body, "re", json::object());
  mail.body.erase("re");
  mail.body["text"] = "";
  // Counted once: handed over again, it leaves as it did then.
  const auto [read, first] = heard_.try_emplace(mail.id, kNothing);
  // A member that has left is not heard; its last words go with it.
  if (sender != members.end()) {
    // Named from its header, never from what it sent: a model that writes
    // its name before its message, as it reads the others', is named once.
    const std::string name = Name(*sender);
    if (AsciiLower(text).starts_with(AsciiLower(name) + ":")) {
      text = Trim(text.substr(name.size() + 1));
    }
    if (first && (Late(to) ? !text.empty() && !SilentAnswer(text)
                           : Finished(mail.from, text) == Answer::kMessage)) {
      // With what the coordinator's answer to it is an answer to.
      const json woke = Deliver(mail.from, name, text, to, mail.id);
      read->second = woke.is_null() ? kQuiet : kWaking;
      if (!woke.is_null()) mail.body["re"] = woke;
    }
    if (read->second != kNothing) {
      mail.body["quiet"] = read->second == kQuiet;
      mail.body["author"] = name;
      mail.body["text"] = name + ": " + text;
    }
  }
  Release(members);
  Save();
}
}  // namespace uagent
