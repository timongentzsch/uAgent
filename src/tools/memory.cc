// Copyright 2026 Timon Gentzsch

#include "include/tools/memory.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "include/core/checked.h"
#include "include/core/debug.h"
#include "include/core/env.h"
#include "include/core/fs.h"
#include "include/core/json.h"
#include "include/core/limits.h"
#include "include/core/project.h"
#include "include/core/strings.h"
#include "include/tools/files.h"

namespace uagent {
namespace {

std::filesystem::path CanonicalOrSelf(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::path canonical =
      std::filesystem::weakly_canonical(path, error);
  return error ? path : canonical;
}

std::optional<std::string> FirstLine(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string line;
  return std::getline(input, line) ? std::optional<std::string>(Trim(line))
                                   : std::nullopt;
}

// Assignment keywords whose value is redacted. UAGENT_MEMORY_REDACT_KEYWORDS
// appends to this list; it can never shorten it, so a typo cannot disable
// redaction. Keywords are matched literally (escaped before they reach the
// pattern) because this input is user configuration, not a trusted regex:
// -fno-exceptions makes a malformed std::regex fatal, and an arbitrary pattern
// invites catastrophic backtracking over every memory body.
const std::vector<std::string>& RedactKeywords() {
  static const std::vector<std::string> kKeywords = [] {
    std::vector<std::string> all = {
        "api_key",      "api-key",     "apikey",     "access_token",
        "access-token", "accesstoken", "auth_token", "auth-token",
        "authtoken",    "password",    "passwd",     "secret",
        "authorization"};
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

// Cheap gate for the redactor: text that mentions no credential at all is the
// common case and needs none of the three regex passes. The keyword markers
// are derived from RedactKeywords() rather than restated, so a keyword can no
// longer be gated out of existence -- `passwd` was missed that way once.
bool MentionsSecret(std::string_view text) {
  static const std::vector<std::string> kMarkers = [] {
    std::vector<std::string> all;
    for (const std::string& keyword : RedactKeywords()) {
      all.push_back(AsciiLower(keyword));
    }
    for (const char* fixed : {"bearer", "sk-", "-----begin", "gh"}) {
      all.emplace_back(fixed);
    }
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

std::filesystem::path RepositoryIdentity(const std::filesystem::path& cwd) {
  namespace fs = std::filesystem;
  fs::path root = ProjectRoot(cwd);
  fs::path dot_git = root / ".git";
  std::error_code error;
  if (fs::is_directory(dot_git, error)) return CanonicalOrSelf(dot_git);

  std::optional<std::string> pointer = FirstLine(dot_git);
  constexpr const char* kGitDir = "gitdir:";
  if (!pointer || !pointer->starts_with(kGitDir)) {
    return CanonicalOrSelf(root);
  }
  fs::path git_dir = Trim(pointer->substr(strlen(kGitDir)));
  if (git_dir.is_relative()) git_dir = root / git_dir;
  git_dir = CanonicalOrSelf(git_dir);
  std::optional<std::string> common = FirstLine(git_dir / "commondir");
  if (!common) return git_dir;
  fs::path common_dir(*common);
  if (common_dir.is_relative()) common_dir = git_dir / common_dir;
  return CanonicalOrSelf(common_dir);
}

std::filesystem::path RepositoryLabel(const std::filesystem::path& cwd,
                                      const std::filesystem::path& identity) {
  return identity.filename() == ".git" ? identity.parent_path()
                                       : ProjectRoot(cwd);
}

struct RepositoryPaths {
  std::filesystem::path identity;
  std::filesystem::path label;
};

RepositoryPaths Repository(const std::filesystem::path& cwd) {
  std::filesystem::path identity = RepositoryIdentity(cwd);
  std::filesystem::path label = RepositoryLabel(cwd, identity);
  return {std::move(identity), std::move(label)};
}

std::filesystem::path MemoryDirectory(const std::string& scope,
                                      const RepositoryPaths& repo) {
  namespace fs = std::filesystem;
  fs::path base = fs::path(GlobalBase()) / kMemoryDir;
  if (scope == "global") return base / "global";
  std::string name = SafeFileComponent(repo.label.filename().string()) + "-" +
                     WorkspaceId(repo.identity.string());
  return base / "projects" / name;
}

void AddMarkdownMemories(std::vector<MemoryEntry>& entries,
                         const std::filesystem::path& directory,
                         const std::string& scope, size_t limit) {
  namespace fs = std::filesystem;
  std::error_code error;
  std::vector<fs::path> paths;
  for (fs::directory_iterator it(directory, error), end; it != end && !error;
       it.increment(error)) {
    if (!it->is_regular_file(error) || it->path().extension() != ".md") {
      continue;
    }
    paths.push_back(it->path());
  }
  std::sort(paths.begin(), paths.end());
  for (size_t index = 0; index < std::min(limit, paths.size()); ++index) {
    entries.push_back(
        {scope + "/" + paths[index].stem().string(), paths[index].string()});
  }
}

std::filesystem::path ClaudeMemoryDirectory(const RepositoryPaths& repo) {
  std::string slug = CanonicalOrSelf(repo.label).string();
  for (char& byte : slug) {
    if (!std::isalnum(static_cast<unsigned char>(byte))) byte = '-';
  }
  return std::filesystem::path(UserHome()) / ".claude" / "projects" / slug /
         "memory";
}

std::optional<std::filesystem::path> CurrentWorkspace(std::string& error) {
  std::error_code code;
  std::filesystem::path cwd = std::filesystem::current_path(code);
  if (!code) return cwd;
  error = code.message();
  return std::nullopt;
}

constexpr size_t kMemoryEventBytes = 256 * 1024;
constexpr size_t kMemoryEventLineBytes = 4096;

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
  int fd = open(path.c_str(), O_CREAT | O_RDWR | O_APPEND | O_CLOEXEC,
                kPrivateFileMode);
  if (fd < 0) {
    error = strerror(errno);
    return false;
  }
  auto fail = [&](const std::string& message) {
    error = message;
    flock(fd, LOCK_UN);
    close(fd);
    return false;
  };
  if (!LockFileExclusive(fd)) return fail(strerror(errno));
  struct stat status{};
  if (fstat(fd, &status) != 0) return fail(strerror(errno));
  if (status.st_size > static_cast<off_t>(kMemoryEventBytes)) {
    off_t keep = static_cast<off_t>(kMemoryEventBytes / 2);
    off_t start = std::max<off_t>(0, status.st_size - keep);
    std::string tail(static_cast<size_t>(status.st_size - start), '\0');
    ssize_t count = pread(fd, tail.data(), tail.size(), start);
    if (count < 0) return fail(strerror(errno));
    tail.resize(static_cast<size_t>(count));
    size_t first_line = tail.find('\n');
    if (start > 0 && first_line != std::string::npos) {
      tail.erase(0, first_line + 1);
    }
    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0 ||
        !WriteFully(fd, tail)) {
      return fail(strerror(errno));
    }
  }
  if (!WriteFully(fd, line + "\n")) return fail(strerror(errno));
  flock(fd, LOCK_UN);
  if (close(fd) != 0) {
    error = strerror(errno);
    return false;
  }
  return true;
}

std::string MemoryPreview(const std::string& content) {
  return Utf8Trunc(OneLine(RedactMemorySecrets(content)), 160);
}

ToolResult ListMemoryKeys() {
  std::string output;
  for (const MemoryEntry& memory : ListMemories()) {
    if (!output.empty()) output += '\n';
    output += memory.key;
  }
  return ToolSuccess(output.empty() ? "(no memories)" : std::move(output));
}

ToolResult SearchMemoryText(const std::string& query) {
  std::string needle = AsciiLower(Trim(query));
  if (needle.empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: memory search requires a non-empty key query");
  }
  constexpr size_t kMaxMatches = 16;
  constexpr size_t kMaxLineBytes = 320;
  std::string output = "[memory search; non-authoritative evidence]\n";
  size_t matches = 0;
  auto capped = [&] {
    if (++matches < kMaxMatches) return false;
    output += "[more matches; narrow the query]";
    return true;
  };
  for (const MemoryEntry& memory : ListMemories()) {
    std::ifstream input(memory.path);
    if (!input) continue;
    std::string line;
    size_t scanned = 0;
    bool key_match = AsciiLower(memory.key).find(needle) != std::string::npos;
    bool emitted = false;
    while (scanned < 64 * 1024 && std::getline(input, line)) {
      scanned += line.size() + 1;
      if (!key_match && AsciiLower(line).find(needle) == std::string::npos) {
        continue;
      }
      output += "- " + memory.key + ": " +
                Utf8Trunc(OneLine(RedactMemorySecrets(line)), kMaxLineBytes) +
                "\n";
      emitted = true;
      if (capped()) return ToolSuccess(std::move(output));
      break;
    }
    if (key_match && !emitted) {
      output += "- " + memory.key + "\n";
      if (capped()) return ToolSuccess(std::move(output));
    }
  }
  return matches
             ? ToolSuccess(std::move(output))
             : ToolFailure(ToolErrorCode::kNotFound,
                           "error: no memory matches: " + TerminalSafe(query));
}

ToolResult ReadMemoryFile(const MemoryEntry& memory) {
  bool writable =
      memory.key.starts_with("global/") || memory.key.starts_with("project/");
  std::ifstream input(memory.path, std::ios::binary);
  if (!input) {
    return ToolFailure(ToolErrorCode::kNotFound, "error: no such memory");
  }
  size_t max_bytes = static_cast<size_t>(MemoryBytes());
  std::string body;
  bool truncated = ReadBounded(input, max_bytes, body);
  if (truncated && writable) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "error: saved memory exceeds configured limit");
  }
  body = RedactMemorySecrets(std::move(body));
  if (truncated) body += "\n[external memory truncated; use search to narrow]";
  return ToolSuccess("[memory " + memory.key + "; non-authoritative evidence" +
                     (writable ? "" : "; read-only") + "]\n" + body);
}

ToolResult AccessMemory(const std::string& name, const std::string& scope,
                        const std::optional<std::string>& content,
                        bool forget) {
  namespace fs = std::filesystem;
  if (Trim(name).empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: memory name must not be empty");
  }
  int64_t max_bytes = MemoryBytes();
  if (content && static_cast<int64_t>(content->size()) > max_bytes) {
    return ToolFailure(ToolErrorCode::kLimitExceeded,
                       "error: a memory is limited to " +
                           std::to_string(max_bytes) +
                           " bytes; keep it to the durable lesson");
  }

  std::string workspace_error;
  std::optional<fs::path> cwd = CurrentWorkspace(workspace_error);
  if (!cwd) {
    return ToolFailure(
        ToolErrorCode::kInternal,
        "error: cannot resolve the workspace: " + workspace_error);
  }
  std::string filename = SafeFileComponent(name) + ".md";
  fs::path path = MemoryDirectory(scope, Repository(*cwd)) / filename;

  if (forget) {
    std::error_code error;
    if (fs::remove(path, error)) return ToolSuccess("forgot " + path.string());
    return ToolFailure(error ? FileToolError(error) : ToolErrorCode::kNotFound,
                       "error: no such memory");
  }

  if (!content) {
    return ReadMemoryFile(
        {scope + "/" + SafeFileComponent(name), path.string()});
  }

  if (Trim(*content).empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: memory content must not be empty; use forget");
  }
  bool existed = fs::exists(path);
  if (!existed) {
    int64_t count = 0;
    for (const MemoryEntry& memory : ListMemories(*cwd)) {
      count += memory.key.starts_with(scope + "/");
    }
    if (count >= MaxMemories()) {
      return ToolFailure(ToolErrorCode::kLimitExceeded,
                         "error: " + scope + " memory is full (" +
                             std::to_string(MaxMemories()) +
                             "); delete or consolidate one before adding "
                             "another");
    }
  }
  std::string memory_root = MakePrivateDir(GlobalBase(), kMemoryDir);
  if (scope == "global") {
    MakePrivateDir(memory_root, "global");
  } else {
    std::string projects = MakePrivateDir(memory_root, "projects");
    std::string project = path.parent_path().filename().string();
    MakePrivateDir(projects, project.c_str());
  }
  std::string previous;
  if (existed) {
    std::ifstream input(path, std::ios::binary);
    previous.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
  }
  std::string action = !existed               ? "created"
                       : previous == *content ? "unchanged"
                                              : "updated";
  ToolResult saved = action == "unchanged"
                         ? ToolSuccess("unchanged " + path.string())
                         : ToolWritePrivateFile(path.string(), *content);
  if (!saved.Ok()) return saved;

  std::string source = EnvStr("UAGENT_INTERNAL_MEMORY_SOURCE");
  std::string workspace =
      scope == "project" ? path.parent_path().filename().string() : "";
  MemoryEvent event{action,
                    scope + "/" + SafeFileComponent(name),
                    MemoryPreview(*content),
                    action == "updated" ? MemoryPreview(previous) : "",
                    source.empty() ? "" : WorkspaceId(source),
                    std::move(workspace),
                    UtcStamp(),
                    !source.empty()};
  std::string event_error;
  std::string receipt = source.empty() ? "" : EnvStr("UAGENT_MEMORY_RECEIPT");
  if (!WriteMemoryEvent(event, receipt, event_error)) {
    DebugLog("memory_event_write_error",
             {{"error", event_error}, {"key", event.key}});
  }
  return saved;
}

}  // namespace

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

ToolResult ToolMemoryAction(const std::string& action, const std::string& key,
                            const std::optional<std::string>& content) {
  if (action != "get" && action != "set" && action != "forget" &&
      action != "list" && action != "search") {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: action must be get, set, forget, list, or "
                       "search");
  }
  if (action == "list") {
    if (content || !Trim(key).empty()) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "error: list does not accept key or content");
    }
    return ListMemoryKeys();
  }
  if (action == "search") {
    if (content) {
      return ToolFailure(ToolErrorCode::kInvalidArguments,
                         "error: search does not accept content");
    }
    return SearchMemoryText(key);
  }
  size_t slash = key.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 == key.size() ||
      key.find('/', slash + 1) != std::string::npos) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: memory key must be project/<name> or "
                       "global/<name>");
  }
  std::string scope = key.substr(0, slash);
  std::string name = key.substr(slash + 1);
  if (scope == "codex" || scope == "claude") {
    if (action != "get" || content) {
      return ToolFailure(ToolErrorCode::kPermissionDenied,
                         "error: " + scope + " memories are read-only");
    }
    std::vector<MemoryEntry> memories = ListMemories();
    auto found = std::find_if(
        memories.begin(), memories.end(),
        [&](const MemoryEntry& memory) { return memory.key == key; });
    return found == memories.end()
               ? ToolFailure(ToolErrorCode::kNotFound, "error: no such memory")
               : ReadMemoryFile(*found);
  }
  if (scope != "project" && scope != "global") {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: memory key must start with project/, global/, "
                       "codex/, or claude/");
  }
  const bool has_content =
      content.has_value() && (action == "set" || !Trim(*content).empty());
  if (action == "get" && !has_content) {
    return AccessMemory(name, scope, std::nullopt, false);
  }
  if (action == "set" && has_content) {
    return AccessMemory(name, scope, RedactMemorySecrets(*content), false);
  }
  if (action == "forget" && !has_content) {
    return AccessMemory(name, scope, std::nullopt, true);
  }
  return ToolFailure(
      ToolErrorCode::kInvalidArguments,
      "error: " + action +
          (action == "set" ? " requires content" : " does not accept content"));
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
  static const std::regex kKnownToken(
      R"((sk-(?:proj-)?[A-Za-z0-9_-]{16,}|gh[pousr]_[A-Za-z0-9_]{16,}|)"
      R"(github_pat_[A-Za-z0-9_]{16,}))");
  text = std::regex_replace(text, kAssignment, "$1$2[REDACTED]");
  text = std::regex_replace(text, kBearer, "$1[REDACTED]");
  text = std::regex_replace(text, kKnownToken, "[REDACTED]");

  constexpr std::string_view kBegin = "-----BEGIN PRIVATE KEY-----";
  constexpr std::string_view kEnd = "-----END PRIVATE KEY-----";
  for (size_t begin = text.find(kBegin); begin != std::string::npos;
       begin = text.find(kBegin, begin + 10)) {
    size_t end = text.find(kEnd, begin + kBegin.size());
    if (end == std::string::npos) {
      text.replace(begin, text.size() - begin, "[REDACTED PRIVATE KEY]");
      break;
    }
    text.replace(begin, end + kEnd.size() - begin, "[REDACTED PRIVATE KEY]");
  }
  return text;
}

std::vector<MemoryEntry> ListMemories() {
  std::string workspace_error;
  std::optional<std::filesystem::path> cwd = CurrentWorkspace(workspace_error);
  if (!cwd) return {};
  return ListMemories(*cwd);
}

std::vector<MemoryEntry> ListMemories(const std::filesystem::path& cwd) {
  namespace fs = std::filesystem;
  std::vector<MemoryEntry> entries;
  RepositoryPaths repo = Repository(cwd);
  for (const char* scope : {"global", "project"}) {
    AddMarkdownMemories(entries, MemoryDirectory(scope, repo), scope,
                        static_cast<size_t>(MaxMemories()));
  }
  std::string home = UserHome();
  if (!home.empty()) {
    AddMarkdownMemories(entries, fs::path(home) / ".codex" / "memories",
                        "codex", static_cast<size_t>(MaxMemories()));
    AddMarkdownMemories(entries, ClaudeMemoryDirectory(repo), "claude",
                        static_cast<size_t>(MaxMemories()));
  }
  std::sort(entries.begin(), entries.end(),
            [](const MemoryEntry& left, const MemoryEntry& right) {
              return left.key < right.key;
            });
  return entries;
}

MemoryIndex LoadMemoryIndex(const std::filesystem::path& cwd,
                            size_t max_bytes) {
  MemoryIndex index;
  size_t used = 0;
  for (const MemoryEntry& memory : ListMemories(cwd)) {
    std::string line = "- " + memory.key + "\n";
    std::optional<size_t> total = CheckedAdd(used, line.size());
    if (!total || *total > max_bytes) {
      index.truncated = true;
      break;
    }
    used = *total;
    index.text += line;
    index.sources.push_back(memory.path);
  }
  return index;
}

// Behavioral "always-on" slice: the full content of global-scope memories is
// injected into the startup context in addition to the index, capped by
// UAGENT_MEMORY_ALWAYS_BYTES. Global scope is the applies-everywhere bucket, so
// these are exactly the standing preferences/corrections the agent should not
// have to remember to go look up.
//
// Newest first, and every entry is admitted whole or skipped: a lesson cut
// mid-sentence is worse than an absent one, and when the cap binds the freshest
// standing preferences are the ones worth keeping.
MemoryIndex LoadAlwaysOnMemory(const std::filesystem::path& cwd,
                               size_t max_bytes) {
  namespace fs = std::filesystem;
  std::vector<std::pair<fs::file_time_type, MemoryEntry>> globals;
  for (const MemoryEntry& memory : ListMemories(cwd)) {
    if (!memory.key.starts_with("global/")) continue;
    std::error_code error;
    fs::file_time_type modified = fs::last_write_time(memory.path, error);
    globals.emplace_back(error ? fs::file_time_type{} : modified, memory);
  }
  std::stable_sort(globals.begin(), globals.end(),
                   [](const auto& left, const auto& right) {
                     return left.first > right.first;
                   });

  MemoryIndex index;
  size_t used = 0;
  size_t body_cap = static_cast<size_t>(MemoryBytes());
  for (const auto& [modified, memory] : globals) {
    std::ifstream input(memory.path, std::ios::binary);
    if (!input) continue;
    std::string body;
    if (ReadBounded(input, body_cap, body)) {
      // Larger than a single memory is allowed to be: it belongs in the index
      // for an explicit get, not in every request.
      index.truncated = true;
      continue;
    }
    body = RedactMemorySecrets(std::move(body));
    if (Trim(body).empty()) continue;
    std::string block = "# " + memory.key + "\n" + body + "\n\n";
    std::optional<size_t> total = CheckedAdd(used, block.size());
    if (!total || *total > max_bytes) {
      // Skip what no longer fits rather than starving the rest of the list.
      index.truncated = true;
      continue;
    }
    used = *total;
    index.text += block;
    index.sources.push_back(memory.path);
  }
  return index;
}

}  // namespace uagent
