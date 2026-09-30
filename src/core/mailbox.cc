// Copyright 2026 Timon Gentzsch

#include "include/core/mailbox.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <sys/event.h>
#elif defined(__linux__)
#include <sys/inotify.h>
#endif

#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/core/time.h"
#include "include/transport/session.h"

namespace uagent {
namespace {
namespace fs = std::filesystem;

constexpr const char* kDeliveryNames[] = {"wake", "step", "passive",
                                          "interrupt"};

bool ValidId(const std::string& id) {
  return !id.empty() && SafeFileComponent(id) == id;
}

// Sorted by name, which starts with the send time: the order they were sent.
std::vector<fs::path> Messages(const fs::path& dir) {
  std::vector<fs::path> files;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(dir, error)) {
    const std::string name = entry.path().filename().string();
    if (!name.starts_with(".") && name.ends_with(".json")) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

bool ReadMail(const fs::path& path, Mail& mail) {
  auto text = ReadFile(path.string(), 2 * kMailBytes);
  return text && MailFromJson(json::parse(*text, nullptr, false), mail);
}

// Pending messages with their files, unreadable ones set aside.
std::vector<std::pair<fs::path, Mail>> ReadPending(const std::string& id) {
  std::vector<std::pair<fs::path, Mail>> pending;
  const fs::path dir = fs::path(MailboxDir(id)) / "new";
  for (const fs::path& path : Messages(dir)) {
    Mail mail;
    if (ReadMail(path, mail)) {
      pending.emplace_back(path, std::move(mail));
      continue;
    }
    std::error_code error;
    fs::rename(
        path,
        fs::path(MailboxDir(id)) / "cur" / (path.filename().string() + ".bad"),
        error);
  }
  return pending;
}

// A sender's recent sends, per process: the rate is one sender's to keep.
bool OverRate(const std::string& from) {
  static std::mutex mutex;
  static std::map<std::string, std::deque<int64_t>> sent;
  std::lock_guard lock(mutex);
  const int64_t now = NowMillis();
  auto& times = sent[from];
  while (!times.empty() && now - times.front() > int64_t{60} * 1000) {
    times.pop_front();
  }
  if (times.size() >= kMailSenderPerMinute) return true;
  times.push_back(now);
  return false;
}
}  // namespace

const char* MailDeliveryName(MailDelivery delivery) {
  return kDeliveryNames[static_cast<int>(delivery)];
}

json MailToJson(const Mail& mail) {
  return {{"format", 1},
          {"id", mail.id},
          {"from", mail.from},
          {"to", mail.to},
          {"type", mail.type},
          {"sender_path", mail.sender_path},
          {"correlation_id", mail.correlation_id},
          {"causation_id", mail.causation_id},
          {"reply_to", mail.reply_to},
          {"delivery", MailDeliveryName(mail.delivery)},
          {"hops", mail.hops},
          {"created_ms", mail.created_ms},
          {"expires_ms", mail.expires_ms},
          {"body", mail.body}};
}

bool MailFromJson(const json& value, Mail& mail) {
  if (!value.is_object() || JsonValue(value, "format", 0) != 1) return false;
  mail.id = JsonValue(value, "id", "");
  mail.from = JsonValue(value, "from", "");
  mail.to = JsonValue(value, "to", "");
  mail.type = JsonValue(value, "type", "");
  mail.sender_path = JsonValue(value, "sender_path", "");
  mail.correlation_id = JsonValue(value, "correlation_id", "");
  mail.causation_id = JsonValue(value, "causation_id", "");
  mail.reply_to = JsonValue(value, "reply_to", "");
  mail.hops = JsonValue(value, "hops", 0);
  mail.created_ms = JsonValue(value, "created_ms", int64_t{0});
  mail.expires_ms = JsonValue(value, "expires_ms", int64_t{0});
  mail.body = JsonValue(value, "body", json::object());
  const std::string delivery = JsonValue(value, "delivery", "");
  auto found =
      std::find(std::begin(kDeliveryNames), std::end(kDeliveryNames), delivery);
  if (found == std::end(kDeliveryNames)) return false;
  mail.delivery = static_cast<MailDelivery>(found - std::begin(kDeliveryNames));
  return ValidId(mail.id) && !mail.type.empty() && mail.body.is_object();
}

std::string MailboxIdFor(const std::string& session_path) {
  if (session_path.empty()) return "";
  std::error_code error;
  const fs::path canonical = fs::weakly_canonical(session_path, error);
  return HashHex(error ? session_path : canonical.string());
}

std::string MailboxDir(const std::string& id) {
  if (!ValidId(id)) return "";
  const std::string dir = UagentDir("mail") + "/" + id;
  CreatePrivateDirectories(dir + "/new");
  CreatePrivateDirectories(dir + "/cur");
  return dir;
}

std::string SendMail(Mail mail) {
  if (!ValidId(mail.to)) return "unknown recipient " + mail.to;
  if (mail.type.empty()) return "a message needs a type";
  if (mail.hops >= kMailMaxHops) {
    return "not sent: it has been forwarded " + std::to_string(mail.hops) +
           " times, which looks like a loop";
  }
  if (mail.id.empty()) mail.id = session::RandomToken(8);
  if (mail.id.empty()) return "cannot name the message";
  if (!mail.created_ms) mail.created_ms = NowMillis();
  if (!mail.expires_ms) mail.expires_ms = mail.created_ms + kMailLifetimeMs;
  const std::string content = JsonDump(MailToJson(mail));
  if (content.size() > kMailBytes) {
    return "a message holds at most " + std::to_string(kMailBytes / 1024) +
           " KiB";
  }
  const std::string dir = MailboxDir(mail.to);
  if (dir.empty()) return "unknown recipient " + mail.to;
  size_t count = 0;
  for (auto& [path, pending] : ReadPending(mail.to)) {
    if (pending.from == mail.from && pending.type == mail.type &&
        pending.body == mail.body) {
      return "";  // the same message is still waiting
    }
    if (mail.type == kMailTaskProgress && pending.type == mail.type &&
        pending.from == mail.from &&
        pending.correlation_id == mail.correlation_id) {
      std::error_code error;
      fs::remove(path, error);
      continue;
    }
    ++count;
  }
  if (count >= kMailboxPending) {
    return "not sent: the recipient has " + std::to_string(count) +
           " messages waiting; try again after it has read them";
  }
  if (OverRate(mail.from)) {
    return "not sent: at most " + std::to_string(kMailSenderPerMinute) +
           " messages a minute; wait before sending more";
  }
  // Time, then pid and a per-process count: one sender's messages keep their
  // order within a millisecond.
  static std::atomic<uint64_t> sequence{0};
  char stamp[64];
  snprintf(stamp, sizeof stamp, "%013" PRId64 "-%010d-%08" PRIu64,
           mail.created_ms, static_cast<int>(getpid()),
           static_cast<uint64_t>(sequence.fetch_add(1) % 100000000));
  std::string error;
  if (!AtomicWriteFile(dir + "/new/" + stamp + "-" + mail.id + ".json", content,
                       kPrivateFileMode, false, error)) {
    return error;
  }
  return "";
}

std::vector<Mail> TakeMail(const std::string& id,
                           const std::function<bool(const Mail&)>& accept) {
  std::vector<Mail> taken;
  if (!ValidId(id)) return taken;
  const fs::path cur = fs::path(MailboxDir(id)) / "cur";
  const int64_t now = NowMillis();
  for (auto& [path, mail] : ReadPending(id)) {
    std::error_code error;
    if (mail.expires_ms && mail.expires_ms < now) {
      fs::remove(path, error);
      continue;
    }
    if (!accept(mail)) continue;
    fs::rename(path, cur / path.filename(), error);
    if (!error) taken.push_back(std::move(mail));
  }
  return taken;
}

std::vector<Mail> PendingMail(const std::string& id) {
  std::vector<Mail> pending;
  if (!ValidId(id)) return pending;
  for (auto& [path, mail] : ReadPending(id)) pending.push_back(std::move(mail));
  return pending;
}

void AckMail(const std::string& id, const std::vector<std::string>& ids) {
  if (!ValidId(id) || ids.empty()) return;
  for (const fs::path& path : Messages(fs::path(MailboxDir(id)) / "cur")) {
    const std::string name = path.stem().string();
    for (const std::string& acked : ids) {
      if (name.ends_with("-" + acked)) {
        std::error_code error;
        fs::remove(path, error);
        break;
      }
    }
  }
}

void RecoverMail(const std::string& id) {
  if (!ValidId(id)) return;
  const fs::path dir = MailboxDir(id);
  for (const fs::path& path : Messages(dir / "cur")) {
    std::error_code error;
    fs::rename(path, dir / "new" / path.filename(), error);
  }
}

MailboxWatch::MailboxWatch(const std::string& id) {
  const std::string dir = MailboxDir(id);
  if (dir.empty()) return;
  const std::string pending = dir + "/new";
#if defined(__linux__)
  Fd watcher(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
  if (watcher && inotify_add_watch(watcher.Get(), pending.c_str(),
                                   IN_MOVED_TO | IN_CLOSE_WRITE) >= 0) {
    fd_ = std::move(watcher);
  }
#elif defined(__APPLE__)
  // The directory's descriptor must outlive the watch; kqueue keeps it.
  Fd queue(kqueue());
  int folder = open(pending.c_str(), O_EVTONLY | O_CLOEXEC);
  if (!queue || folder < 0) {
    if (folder >= 0) close(folder);
    return;
  }
  fcntl(queue.Get(), F_SETFD, FD_CLOEXEC);
  struct kevent change;
  EV_SET(&change, folder, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE, 0,
         nullptr);
  if (kevent(queue.Get(), &change, 1, nullptr, 0, nullptr) == 0) {
    fd_ = std::move(queue);
  } else {
    close(folder);
  }
#endif
}

void MailboxWatch::Drain() const {
  if (!fd_) return;
#if defined(__linux__)
  char buffer[4096];
  while (read(fd_.Get(), buffer, sizeof buffer) > 0) {
  }
#elif defined(__APPLE__)
  struct kevent event;
  timespec immediately = {0, 0};
  while (kevent(fd_.Get(), nullptr, 0, &event, 1, &immediately) > 0) {
  }
#endif
}

}  // namespace uagent
