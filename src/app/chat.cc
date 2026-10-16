// Copyright 2026 Timon Gentzsch

#include "include/app/chat.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "include/agent/protocol.h"
#include "include/agent/session_role.h"
#include "include/app/launch.h"
#include "include/app/panel.h"
#include "include/app/session.h"
#include "include/cli/options.h"
#include "include/core/config_registry.h"
#include "include/core/debug.h"
#include "include/core/strings.h"

namespace uagent {
namespace {
// `text` without its author's own name in front: a model writes it there as
// it reads the others' messages, and it would address the author.
std::string WithoutOwnName(std::string text, const std::string& name) {
  text = Trim(text);
  if (AsciiLower(text).starts_with(AsciiLower(name) + ":")) {
    text = Trim(text.substr(name.size() + 1));
  }
  return text;
}

// The moderator among the participants, under the id a saved round and
// queued mail know it by; what a message calls it is ModeratorName.
constexpr const char* kSelf = kChatPanel;
// The names participants had before a rename, in lower case, with whose
// each is: a mailbox, or kSelf. Kept beside the panel's session file.
constexpr const char* kFormer = ".names";

// The moderator goes by the title its panel was given, if it was given one.
std::string ModeratorName(const std::string& folder) {
  const json header = SessionHeader(PanelPath(folder));
  return JsonValue(header, "custom_title", false)
             ? JsonValue(header, kSessionHeaderTitle, "")
             : "Moderator";
}
// Messages one participant may post before the user writes again.
constexpr int kPostsEach = 2;

// Where the round is kept beside the panel's session file.
constexpr const char* kState = ".chat";

// Starts the runtimes of `members`, which mail alone does not: it starts a
// turn only in a running one. Off this thread: each start waits for its
// runtime to answer.
void Start(std::vector<SessionInfo> members) {
  if (members.empty()) return;
  std::thread([members = std::move(members)] {
    for (const SessionInfo& member : members) {
      std::string error;
      if (!session::Open(ExecutablePath(), member.cwd, member.path, "",
                         Options{}, error)
               .socket) {
        DebugLog("chat_member_start_failed", {{"error", error}});
      }
    }
  }).detach();
}

bool WordChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}
}  // namespace

// A member goes by its session's title: renaming the one renames the other.
// Its mailbox costs a file-system lookup and the moderator's name a read of
// the panel's header, so each is taken once per event.
struct ChatRoster {
  std::string moderator;
  std::vector<SessionInfo> members;
  std::vector<std::string> boxes;  // each member's mailbox, by its index

  const SessionInfo* Find(std::string_view box) const {
    const auto at = std::ranges::find(boxes, box);
    return at == boxes.end()
               ? nullptr
               : &members[static_cast<size_t>(at - boxes.begin())];
  }
  // The participants in `ids` by name, as a sentence lists them.
  std::string Names(const std::set<std::string>& ids) const {
    std::string names;
    for (size_t index = 0; index < members.size(); ++index) {
      if (ids.contains(boxes[index])) {
        names += (names.empty() ? "" : ", ") + members[index].title;
      }
    }
    if (ids.contains(kSelf)) names += (names.empty() ? "" : ", ") + moderator;
    return names;
  }
};

namespace {
ChatRoster LoadRoster(const std::string& folder) {
  ChatRoster roster{ModeratorName(folder), ChatMembers(folder), {}};
  for (const SessionInfo& member : roster.members) {
    roster.boxes.push_back(MailboxIdFor(member.path));
  }
  return roster;
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

Chat::Chat(std::string folder,
           std::function<void(const std::string&, const json&)> wake)
    : folder_(std::move(folder)), wake_(std::move(wake)) {
  const json saved = ReadPanelFile(folder_, kState);
  each_ = JsonValue(saved, "each", int64_t{0});
  round_ = JsonValue(saved, "round", int64_t{0});
  posted_ = JsonValue(saved, "posted", 0);
  closed_ = JsonValue(saved, "closed", false);
  for (std::string& id : JsonStrings(saved, "typing")) {
    typing_.insert(std::move(id));
  }
  waiting_ = JsonStrings(saved, "waiting");
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
  const std::string error = WritePanelFile(folder_, kState,
                                           {{"each", each_},
                                            {"round", round_},
                                            {"posted", posted_},
                                            {"closed", closed_},
                                            {"typing", typing_},
                                            {"waiting", waiting_},
                                            {"heard", heard_},
                                            {"seats", std::move(seats)}});
  if (!error.empty()) DebugLog("chat_state_unsaved", {{"error", error}});
}

namespace {
// The participants `text` opens with: a member as its mailbox, the
// moderator as kSelf. A name one of them had before still reaches it.
std::set<std::string> Named(const std::string& folder, const ChatRoster& roster,
                            const std::string& text) {
  std::map<std::string, std::string> whose;
  const json former = ReadPanelFile(folder, kFormer);
  for (const auto& [name, id] : former.items()) {
    if (id.is_string()) whose[name] = id.get<std::string>();
  }
  whose[roster.moderator] = kSelf;
  for (size_t index = 0; index < roster.members.size(); ++index) {
    whose[roster.members[index].title] = roster.boxes[index];
  }
  std::vector<std::string> names;
  names.reserve(whose.size());
  for (const auto& [name, id] : whose) names.push_back(name);
  std::set<std::string> ids;
  for (const std::string& name : Addressed(text, names)) {
    // Not one a member has left behind.
    if (const std::string& id = whose[name]; id == kSelf || roster.Find(id)) {
      ids.insert(id);
    }
  }
  return ids;
}
}  // namespace

bool ForMembersOnly(const std::string& folder, const std::string& text) {
  const std::set<std::string> opens = Named(folder, LoadRoster(folder), text);
  return !opens.empty() && !opens.contains(kSelf);
}

std::string ChatNameProblem(const std::string& folder, const std::string& name,
                            const std::string& path) {
  const std::string lower = AsciiLower(name);
  const bool moderator = path == PanelPath(folder);
  const std::string self = moderator ? kSelf : MailboxIdFor(path);
  const ChatRoster roster = LoadRoster(folder);
  bool taken =
      lower == "user" || (!moderator && lower == AsciiLower(roster.moderator));
  for (size_t index = 0; index < roster.members.size(); ++index) {
    taken |= roster.boxes[index] != self &&
             AsciiLower(roster.members[index].title) == lower;
  }
  const json before = ReadPanelFile(folder, kFormer);
  for (const auto& [former, id] : before.items()) {
    taken |= former == lower && id != self;
  }
  const bool shaped =
      !name.empty() && name.size() <= 40 && name == Trim(name) &&
      name.find("  ") == std::string::npos &&
      std::ranges::none_of(name, [](unsigned char c) {
        return c < 32 || c == 127 || c == ',' || c == ':' || c == '&';
      });
  return shaped && !taken ? ""
                          : "a name in the chat is 1 to 40 characters "
                            "without , : or &, and nobody else's";
}

std::string ChatName(const std::string& folder, const std::string& path) {
  return path == PanelPath(folder)
             ? ModeratorName(folder)
             : JsonValue(SessionHeader(path), kSessionHeaderTitle, "");
}

void ChatRenamed(const std::string& folder, const std::string& path,
                 const std::string& old, const std::string& now) {
  const bool moderator = path == PanelPath(folder);
  if (old.empty() || AsciiLower(old) == AsciiLower(now)) return;
  json former = ReadPanelFile(folder, kFormer);
  former.erase(AsciiLower(now));
  former[AsciiLower(old)] = moderator ? kSelf : MailboxIdFor(path);
  if (const std::string error = WritePanelFile(folder, kFormer, former);
      !error.empty()) {
    DebugLog("chat_names_unsaved", {{"error", error}});
  }
  // The others keep the lines they read under the old name, and are told.
  const json body = {
      {"text", HarnessNote("chat", old + " is now called " + now + ".")},
      {"quiet", true}};
  for (const SessionInfo& member : ChatMembers(folder)) {
    if (member.path == path) continue;
    if (const std::string refused = SendMail(MailFrom(
            PanelPath(folder), MailboxIdFor(member.path), kMailChat, body));
        !refused.empty()) {
      DebugLog("chat_mail_refused", {{"error", refused}});
    }
  }
}

std::vector<SessionInfo> ChatMembers(const std::string& folder) {
  std::vector<SessionInfo> members = OwnThreads(folder);
  std::erase_if(members, [](const auto& info) {
    return ChatMember(info.thread).empty();
  });
  std::ranges::reverse(members);
  return members;
}

std::set<std::string> ChatTyping(const std::string& folder) {
  std::vector<std::string> ids =
      JsonStrings(ReadPanelFile(folder, kState), "typing");
  return {std::make_move_iterator(ids.begin()),
          std::make_move_iterator(ids.end())};
}

std::string Chat::Context() const {
  const ChatRoster roster = LoadRoster(folder_);
  if (roster.members.empty()) return "";
  std::set<std::string> others;
  for (size_t index = 0; index < roster.members.size(); ++index) {
    if (typing_.contains(roster.boxes[index]) &&
        AnswerAhead(roster.members[index])) {
      others.insert(roster.boxes[index]);
    }
  }
  const std::string typing = roster.Names(others);
  return "\n## chat\nYour name: " + roster.moderator + "\nMembers: " +
         roster.Names({roster.boxes.begin(), roster.boxes.end()}) +
         (typing.empty() ? "" : "\nTyping: " + typing) + "\n";
}

Chat::Answer Chat::Finished(const std::string& who, const std::string& text) {
  typing_.erase(who);
  Seat& seat = seats_[who];
  Answer answer = !text.empty() && !SilentAnswer(text) ? Answer::kMessage
                  : text.starts_with("WAIT")           ? Answer::kWait
                                                       : Answer::kPass;
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

json Chat::Deliver(const ChatRoster& roster, const std::set<std::string>& opens,
                   const std::string& from, const std::string& author,
                   const std::string& text, const json& to,
                   const std::string& source) {
  Settle(roster);
  // Whom it is for: those it opens with, else whoever asked its author.
  const std::string asker = JsonValue(to, "asker", "");
  const bool answers = opens.empty() && to.is_object() && to.contains("asker");
  std::set<std::string> some = opens;
  if (answers) some.insert(asker);
  const bool narrowed = !opens.empty() || answers;
  // An answer to the user is the user's. The panel's greets only members
  // who joined while it was written, who have no seat yet.
  const bool for_user = answers && asker.empty();
  // What a reader it wakes answers to: only one it names answers its author.
  const auto asks = [&](const std::string& reader) {
    json re = {{"round", round_}};
    if (opens.contains(reader)) re["asker"] = from;
    return re;
  };
  // Settled for everyone first, so each is told who else is answering.
  const bool late = Late(to);
  std::vector<bool> woken(roster.members.size());
  for (size_t index = 0; index < woken.size(); ++index) {
    const std::string& id = roster.boxes[index];
    const bool joined = for_user && from == kSelf && !seats_.contains(id);
    woken[index] =
        !late && id != from && Wakes(id, narrowed, some.contains(id) || joined);
  }
  const bool self = !late && from != kSelf && !from.empty() &&
                    Wakes(kSelf, narrowed, some.contains(kSelf));
  // The author's session, as clients name it: who wrote this, whatever it
  // is called later.
  const SessionInfo* writer = roster.Find(from);
  const std::string sender = from == kSelf ? PanelId(folder_)
                             : writer      ? HashHex(writer->path)
                                           : "";
  const Mail chat = MailFrom(PanelPath(folder_), "", kMailChat, json());
  std::vector<SessionInfo> started;
  for (size_t index = 0; index < roster.members.size(); ++index) {
    const std::string& reader = roster.boxes[index];
    if (reader == from) continue;
    std::set<std::string> others = typing_;
    others.erase(reader);
    const std::string typing = roster.Names(others);
    Mail mail = chat;
    mail.to = reader;
    // Forwarded again after a crash, it is the mail its reader already has.
    if (!source.empty()) mail.id = HashHex(source + reader);
    mail.body = {{"text", author + ": " + text +
                              (typing.empty() || !woken[index]
                                   ? ""
                                   : "\n\n(typing: " + typing + ")")},
                 {"author", author},
                 {"sender", sender},
                 {"re", asks(reader)},
                 {"quiet", !woken[index]}};
    if (const std::string refused = SendMail(std::move(mail));
        !refused.empty()) {
      DebugLog("chat_mail_refused", {{"error", refused}});
      // Owed nothing it was not sent.
      if (woken[index]) {
        --seats_[reader].turns;
        typing_.erase(reader);
      }
      continue;
    }
    if (woken[index]) started.push_back(roster.members[index]);
  }
  // A member's message in the round, not its answer to the user who asked
  // it, is discussion the panel closes.
  if (from != kSelf && !from.empty() && !late && !for_user) ++posted_;
  Start(std::move(started));
  return self ? asks(kSelf) : json();
}

void Chat::Settle(const ChatRoster& roster) {
  // Nobody waits for a member that has left, nor for one whose answer
  // nothing can bring any more: its runtime never started, or is gone.
  std::erase_if(typing_, [&](const std::string& id) {
    if (id == kSelf) return false;
    const SessionInfo* member = roster.Find(id);
    return !member || !AnswerAhead(*member);
  });
  std::erase_if(waiting_, [&](const std::string& id) {
    return id != kSelf && !roster.Find(id);
  });
}

void Chat::Release(const ChatRoster& roster) {
  Settle(roster);
  const std::string note = HarnessNote("chat", "Nobody is typing now.");
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
      wake_(note, {{"round", round_}});
      return;
    }
    const SessionInfo* member = roster.Find(who);
    if (!member) continue;
    if (!SendMail(MailFrom(PanelPath(folder_), who, kMailChat,
                           {{"text", note},
                            {"re", {{"round", round_}}},
                            {"quiet", false}}))
             .empty()) {
      continue;
    }
    ++seat.turns;
    typing_.insert(who);
    Start({*member});
  }
  // Silent after members discussed (one answer needs no summing up), with
  // nothing the panel handed out still under way: the moderator closes the
  // round for the user, once.
  if (typing_.empty() && waiting_.empty() && posted_ >= 2 && !closed_ &&
      !ThreadsWorking(folder_)) {
    posted_ = 0;
    closed_ = true;
    typing_.insert(kSelf);
    wake_(HarnessNote("chat", "The discussion has settled.") +
              "\nClose it for the user: what was decided, the answer or "
              "scores, and what is still open. This message goes to the user; "
              "the members only read it.",
          {{"round", round_}, {"asker", ""}});
  }
}

json Chat::Begin(bool answers) {
  each_ = LongSetting(Cfg("UAGENT_PANEL_CHAT_TURNS"));
  round_ = NowMillis();
  typing_.clear();
  waiting_.clear();
  seats_.clear();
  heard_.clear();
  posted_ = 0;
  closed_ = false;
  if (!answers) return json();
  seats_[kSelf].turns = 1;
  typing_.insert(kSelf);
  return {{"round", round_}};
}

json Chat::Said(const std::string& text) {
  const ChatRoster roster = LoadRoster(folder_);
  // The panel answers its user's message like any other, unless it is
  // for members alone. Members it adds while answering take part.
  const std::set<std::string> opens = Named(folder_, roster, text);
  json re = Begin(opens.empty() || opens.contains(kSelf));
  if (!re.is_null()) re["asker"] = "";
  Deliver(roster, opens, "", "user", text);
  Save();
  return re;
}

void Chat::Answered(const std::string& text, const json& to) {
  // Kept with members or without: the panel's turn ends either way.
  const ChatRoster roster = LoadRoster(folder_);
  const std::string said = WithoutOwnName(text, roster.moderator);
  if (Finished(kSelf, said) == Answer::kMessage) {
    posted_ = 0;
    Deliver(roster, Named(folder_, roster, said), kSelf, roster.moderator, said,
            to);
  }
  Release(roster);
  Save();
}

void Chat::Heard(Mail& mail) {
  // A thread's report is news to discuss: what the panel makes of it
  // begins a round, as the user's message does.
  if (mail.type == kMailTaskCompleted) {
    // Once the round is closed, the panel's answer to a report is the
    // user's: the chat waits for the user.
    mail.body["re"] =
        closed_ ? json{{"round", round_}, {"asker", ""}} : Begin(true);
    Save();
    return;
  }
  if (mail.type != kMailChat) return;
  const ChatRoster roster = LoadRoster(folder_);
  const SessionInfo* sender = roster.Find(mail.from);
  std::string text = Trim(JsonValue(mail.body, "text", ""));
  // What it answers. One to an earlier round is read, and is nobody's turn
  // in this one; so is one that answers nothing in the chat, as an
  // introduction does.
  json to = JsonValue(mail.body, "re", json());
  if (!to.is_object()) to = json::object();
  mail.body.erase("re");
  mail.body["text"] = "";
  // Counted once: handed over again, it leaves as it did then.
  const auto [read, first] = heard_.try_emplace(mail.id, kNothing);
  // A member that has left is not heard; its last words go with it.
  if (sender) {
    // Named from its header, never from what it sent.
    const std::string& name = sender->title;
    text = WithoutOwnName(std::move(text), name);
    if (first && (Late(to) ? !text.empty() && !SilentAnswer(text)
                           : Finished(mail.from, text) == Answer::kMessage)) {
      // With what the panel's answer to it is an answer to.
      const json woke = Deliver(roster, Named(folder_, roster, text), mail.from,
                                name, text, to, mail.id);
      read->second = woke.is_null() ? kQuiet : kWaking;
      if (!woke.is_null()) mail.body["re"] = woke;
    }
    if (read->second != kNothing) {
      mail.body["quiet"] = read->second == kQuiet;
      mail.body["author"] = name;
      mail.body["sender"] = HashHex(sender->path);
      mail.body["text"] = name + ": " + text;
    }
  }
  Release(roster);
  Save();
}
}  // namespace uagent
