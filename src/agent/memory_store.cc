// Copyright 2026 Timon Gentzsch

#include "include/agent/memory_store.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/env.h"
#include "include/core/fd.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/strings.h"

namespace uagent {


// Assignment keywords whose value is redacted. UAGENT_MEMORY_REDACT_KEYWORDS
// appends to this list; it can never shorten it, so a typo cannot disable
// redaction. Keywords are matched literally (escaped before they reach the
// pattern) because this input is user configuration, not a trusted regex:
// -fno-exceptions makes a malformed std::regex fatal, and an arbitrary pattern
// invites catastrophic backtracking over every memory body.
const std::vector<std::string>& RedactKeywords() {
  static const std::vector<std::string> kKeywords = [] {
    std::vector<std::string> all = CredentialKeyStems();
    for (const char* variant :
         {"api-key", "apikey", "access-token", "accesstoken", "auth_token",
          "auth-token", "authtoken"}) {
      all.emplace_back(variant);
    }
    constexpr size_t kMaxExtra = 32;
    constexpr size_t kMaxKeywordBytes = 64;
    std::string configured = EnvStr("UAGENT_MEMORY_REDACT_KEYWORDS");
    size_t added = 0;
    for (size_t begin = 0; begin <= configured.size() && added < kMaxExtra;) {
      size_t end = configured.find(',', begin);
      std::string keyword = Trim(configured.substr(
          begin, end == std::string::npos ? std::string::npos : end - begin));
      // An empty alternative would match everywhere and redact the whole text.
      if (!keyword.empty() && keyword.size() <= kMaxKeywordBytes) {
        all.push_back(std::move(keyword));
        ++added;
      }
      if (end == std::string::npos) break;
      begin = end + 1;
    }
    return all;
  }();
  return kKeywords;
}

// Vendor token shapes. The marker is the literal the pre-scan below searches
// for (lowercase -- that scan folds case), the pattern is what gets redacted.
// The pair lives in one row so a new vendor cannot reach the regex without
// also reaching the gate that decides whether the regex ever runs.
struct TokenShape {
  const char* marker;
  const char* pattern;
};


constexpr TokenShape kTokenShapes[] = {
    {"sk-", R"(sk-(?:proj-)?[A-Za-z0-9_-]{16,})"},
    {"ghp_", R"(ghp_[A-Za-z0-9_]{16,})"},
    {"gho_", R"(gho_[A-Za-z0-9_]{16,})"},
    {"ghu_", R"(ghu_[A-Za-z0-9_]{16,})"},
    {"ghs_", R"(ghs_[A-Za-z0-9_]{16,})"},
    {"ghr_", R"(ghr_[A-Za-z0-9_]{16,})"},
    {"github_pat_", R"(github_pat_[A-Za-z0-9_]{16,})"},
    {"akia", R"(AKIA[0-9A-Z]{16})"},
    {"xox", R"(xox[baprs]-[A-Za-z0-9-]{10,})"},
    {"aiza", R"(AIza[0-9A-Za-z_-]{35})"},
};

// Cheap gate for the redactor: text that mentions no credential at all is
// the common case and needs none of the three regex passes. The keyword markers
// are derived from RedactKeywords() rather than restated, so a keyword can no
// longer be gated out of existence -- `passwd` was missed that way once.
bool MentionsSecret(std::string_view text) {
  static const std::vector<std::string> kMarkers = [] {
    std::vector<std::string> all;
    for (const std::string& keyword : RedactKeywords()) {
      all.push_back(AsciiLower(keyword));
    }
    for (const char* fixed : {"bearer", "-----begin"}) {
      all.emplace_back(fixed);
    }
    for (const TokenShape& shape : kTokenShapes) all.emplace_back(shape.marker);
    return all;
  }();
  return std::any_of(
      kMarkers.begin(), kMarkers.end(), [&](const std::string& marker) {
        return std::search(text.begin(), text.end(), marker.begin(),
                           marker.end(), [](char left, char right) {
                             return std::tolower(static_cast<unsigned char>(
                                        left)) == right;
                           }) != text.end();
      });
}

std::string MemoryEventsPath() {
  return UagentDir(kMemoryDir) + "/events.jsonl";
}

json MemoryEventJson(const MemoryEvent& event) {
  return {{"version", 1},
          {"action", event.action},
          {"key", event.key},
          {"preview", event.preview},
          {"previous", event.previous},
          {"source_session", event.source_session},
          {"workspace", event.workspace},
          {"timestamp", event.timestamp},
          {"automatic", event.automatic}};
}

bool ParseMemoryEvent(const json& value, MemoryEvent& event) {
  if (!value.is_object() || JsonValue(value, "version", 0) != 1) return false;
  event.action = JsonValue(value, "action", "");
  event.key = JsonValue(value, "key", "");
  event.preview = JsonValue(value, "preview", "");
  event.previous = JsonValue(value, "previous", "");
  event.source_session = JsonValue(value, "source_session", "");
  event.workspace = JsonValue(value, "workspace", "");
  event.timestamp = JsonValue(value, "timestamp", "");
  event.automatic = JsonValue(value, "automatic", false);
  return !event.action.empty();
}

bool AppendBoundedMemoryEvent(const std::string& line, std::string& error) {
  if (line.size() + 1 > kMemoryEventLineBytes) {
    error = "memory event exceeds its private record limit";
    return false;
  }
  std::string path = MemoryEventsPath();
  Fd fd(open(path.c_str(), O_CREAT | O_RDWR | O_APPEND | O_CLOEXEC,
             kPrivateFileMode));
  if (!fd) {
    error = strerror(errno);
    return false;
  }
  auto fail = [&](const std::string& message) {
    error = message;
    flock(fd.Get(), LOCK_UN);
    return false;
  };
  if (!LockFileExclusive(fd.Get())) return fail(strerror(errno));
  struct stat status{};
  if (fstat(fd.Get(), &status) != 0) return fail(strerror(errno));
  if (status.st_size > static_cast<off_t>(kMemoryEventJournalBytes)) {
    off_t keep = static_cast<off_t>(kMemoryEventJournalBytes / 2);
    off_t start = std::max<off_t>(0, status.st_size - keep);
    std::string tail(static_cast<size_t>(status.st_size - start), '\0');
    ssize_t count = pread(fd.Get(), tail.data(), tail.size(), start);
    if (count < 0) return fail(strerror(errno));
    tail.resize(static_cast<size_t>(count));
    size_t first_line = tail.find('\n');
    if (start > 0 && first_line != std::string::npos) {
      tail.erase(0, first_line + 1);
    }
    if (ftruncate(fd.Get(), 0) != 0 || lseek(fd.Get(), 0, SEEK_SET) < 0 ||
        !WriteFully(fd.Get(), tail)) {
      return fail(strerror(errno));
    }
  }
  if (!WriteFully(fd.Get(), line + "\n")) return fail(strerror(errno));
  flock(fd.Get(), LOCK_UN);
  if (fd.Close() != 0) {
    error = strerror(errno);
    return false;
  }
  return true;
}

bool WriteMemoryEvent(const MemoryEvent& event, const std::string& receipt_path,
                      std::string& error) {
  std::string serialized = JsonDump(MemoryEventJson(event));
  bool appended = AppendBoundedMemoryEvent(serialized, error);
  if (receipt_path.empty()) return appended;
  std::string receipt_error;
  bool receipt =
      AtomicWriteFile(receipt_path, serialized + "\n", kPrivateFileMode,
                      /*preserve_mode=*/false, receipt_error);
  if (!receipt) {
    if (!error.empty()) error += "; ";
    error += "receipt: " + receipt_error;
  }
  return appended && receipt;
}

bool ReadMemoryReceipt(const std::string& path, MemoryEvent& event,
                       std::string& error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "receipt not found";
    return false;
  }
  json value = json::parse(input, nullptr, false);
  if (value.is_discarded() || !ParseMemoryEvent(value, event)) {
    error = "invalid memory receipt";
    return false;
  }
  return true;
}

std::vector<MemoryEvent> LoadMemoryEvents(size_t limit) {
  std::ifstream input(MemoryEventsPath());
  std::vector<MemoryEvent> events;
  std::string line;
  while (std::getline(input, line)) {
    json value = json::parse(line, nullptr, false);
    MemoryEvent event;
    if (!value.is_discarded() && ParseMemoryEvent(value, event)) {
      events.push_back(std::move(event));
      if (events.size() > limit) events.erase(events.begin());
    }
  }
  return events;
}

// Escapes every ECMAScript metacharacter so a configured keyword is matched as
// a literal: no user input reaches the pattern as syntax.
std::string RegexLiteral(std::string_view keyword) {
  std::string escaped;
  escaped.reserve(keyword.size() * 2);
  for (char value : keyword) {
    if (std::strchr(R"(^$\.*+?()[]{}|/)", value) != nullptr) escaped += '\\';
    escaped += value;
  }
  return escaped;
}

std::string RedactMemorySecrets(std::string text) {
  if (!MentionsSecret(text)) return text;
  static const std::regex kAssignment(
      [] {
        std::string alternation;
        for (const std::string& keyword : RedactKeywords()) {
          if (!alternation.empty()) alternation += '|';
          alternation += RegexLiteral(keyword);
        }
        return "(" + alternation + R"()([ \t]*[:=][ \t]*[\"']?)([^\"'\s,;}]+))";
      }(),
      std::regex_constants::icase);
  static const std::regex kBearer(R"((Bearer[ \t]+)[A-Za-z0-9._~+/=-]{12,})",
                                  std::regex_constants::icase);
  static const std::regex kKnownToken([] {
    std::string alternation;
    for (const TokenShape& shape : kTokenShapes) {
      if (!alternation.empty()) alternation += '|';
      alternation += shape.pattern;
    }
    return alternation;
  }());
  text = std::regex_replace(text, kAssignment, "$1$2[REDACTED]");
  text = std::regex_replace(text, kBearer, "$1[REDACTED]");
  text = std::regex_replace(text, kKnownToken, "[REDACTED]");

  // Any PEM private key, labelled (RSA, EC, OPENSSH, DSA, ENCRYPTED) or not.
  static const std::regex kPrivateKey(
      R"(-----BEGIN (?:[A-Z0-9 ]+ )?PRIVATE KEY-----[\s\S]*?)"
      R"(-----END (?:[A-Z0-9 ]+ )?PRIVATE KEY-----)");
  static const std::regex kPrivateKeyBegin(
      R"(-----BEGIN (?:[A-Z0-9 ]+ )?PRIVATE KEY-----)");
  text = std::regex_replace(text, kPrivateKey, "[REDACTED PRIVATE KEY]");
  // Whatever BEGIN survives has no END: the key is truncated, so is the text.
  std::smatch unterminated;
  if (std::regex_search(text, unterminated, kPrivateKeyBegin)) {
    const size_t begin = static_cast<size_t>(unterminated.position());
    text.replace(begin, text.size() - begin, "[REDACTED PRIVATE KEY]");
  }
  return text;
}

}  // namespace uagent
