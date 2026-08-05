// Copyright 2026 Timon Gentzsch

#include "include/tools/memory.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <system_error>
#include <vector>

#include "include/core/checked.h"
#include "include/core/env.h"
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

std::filesystem::path MemoryDirectory(const std::string& scope,
                                      const std::filesystem::path& cwd) {
  namespace fs = std::filesystem;
  fs::path base = fs::path(GlobalBase()) / kMemoryDir;
  if (scope == "global") return base / "global";
  fs::path identity = RepositoryIdentity(cwd);
  fs::path label =
      identity.filename() == ".git" ? identity.parent_path() : ProjectRoot(cwd);
  std::string name = SafeFileComponent(label.filename().string()) + "-" +
                     WorkspaceId(identity.string());
  return base / "projects" / name;
}

std::optional<std::filesystem::path> CurrentWorkspace(std::string& error) {
  std::error_code code;
  std::filesystem::path cwd = std::filesystem::current_path(code);
  if (!code) return cwd;
  error = code.message();
  return std::nullopt;
}

ToolResult AccessMemory(const std::string& name, const std::string& scope,
                        const std::optional<std::string>& content,
                        bool forget) {
  namespace fs = std::filesystem;
  if (Trim(name).empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: memory name must not be empty");
  }
  if (scope != "project" && scope != "global") {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: scope must be \"project\" or \"global\"");
  }
  if (forget && content) {
    return ToolFailure(
        ToolErrorCode::kInvalidArguments,
        "error: memory content and forget are mutually exclusive");
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
  fs::path path = MemoryDirectory(scope, *cwd) / filename;

  if (forget) {
    std::error_code error;
    if (fs::remove(path, error)) return ToolSuccess("forgot " + path.string());
    return ToolFailure(error ? FileToolError(error) : ToolErrorCode::kNotFound,
                       "error: no such memory");
  }

  if (!content) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return ToolFailure(ToolErrorCode::kNotFound, "error: no such memory");
    }
    std::string body(static_cast<size_t>(max_bytes) + 1, '\0');
    input.read(body.data(), static_cast<std::streamsize>(body.size()));
    size_t read = static_cast<size_t>(input.gcount());
    if (read > static_cast<size_t>(max_bytes) ||
        input.peek() != std::char_traits<char>::eof()) {
      return ToolFailure(ToolErrorCode::kLimitExceeded,
                         "error: saved memory exceeds configured limit");
    }
    body.resize(read);
    return ToolSuccess("[memory " + scope + "/" + SafeFileComponent(name) +
                       "; non-authoritative evidence]\n" + body);
  }

  if (Trim(*content).empty()) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: memory content must not be empty; use forget");
  }
  if (!fs::exists(path)) {
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
  return ToolWritePrivateFile(path.string(), *content);
}

}  // namespace

ToolResult ToolMemoryAction(const std::string& action, const std::string& key,
                            const std::optional<std::string>& content,
                            bool allow_set) {
  size_t slash = key.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 == key.size() ||
      key.find('/', slash + 1) != std::string::npos) {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: memory key must be project/<name> or "
                       "global/<name>");
  }
  std::string scope = key.substr(0, slash);
  std::string name = key.substr(slash + 1);
  if (scope != "project" && scope != "global") {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: memory key must start with project/ or global/");
  }
  const bool has_content =
      content.has_value() && (action == "set" || !Trim(*content).empty());
  if (action == "get" && !has_content) {
    return AccessMemory(name, scope, std::nullopt, false);
  }
  if (action == "set" && has_content) {
    if (!allow_set) {
      return ToolFailure(ToolErrorCode::kPermissionDenied,
                         "error: memory contributions are disabled by "
                         "UAGENT_MEMORY_GENERATE=0");
    }
    return AccessMemory(name, scope, RedactMemorySecrets(*content), false);
  }
  if (action == "forget" && !has_content) {
    return AccessMemory(name, scope, std::nullopt, true);
  }
  if (action != "get" && action != "set" && action != "forget") {
    return ToolFailure(ToolErrorCode::kInvalidArguments,
                       "error: action must be get, set, or forget");
  }
  return ToolFailure(
      ToolErrorCode::kInvalidArguments,
      "error: " + action +
          (action == "set" ? " requires content" : " does not accept content"));
}

std::string RedactMemorySecrets(std::string text) {
  static const std::regex kAssignment(
      R"((api[_-]?key|access[_-]?token|auth[_-]?token|password|passwd|secret|)"
      R"(authorization)([ \t]*[:=][ \t]*[\"']?)([^\"'\s,;}]+))",
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
  for (const char* scope : {"global", "project"}) {
    std::error_code error;
    for (fs::directory_iterator it(MemoryDirectory(scope, cwd), error), end;
         it != end && !error; it.increment(error)) {
      if (!it->is_regular_file(error) || it->path().extension() != ".md") {
        continue;
      }
      entries.push_back({std::string(scope) + "/" + it->path().stem().string(),
                         it->path().string()});
    }
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
MemoryIndex LoadAlwaysOnMemory(const std::filesystem::path& cwd,
                               size_t max_bytes) {
  MemoryIndex index;
  size_t used = 0;
  for (const MemoryEntry& memory : ListMemories(cwd)) {
    if (!memory.key.starts_with("global/")) continue;
    std::ifstream input(memory.path, std::ios::binary);
    if (!input) continue;
    // Reserve the block header/footer overhead so the admission test below
    // never fails solely because the body consumed the whole budget.
    size_t overhead = 1 /* '#' */ + memory.key.size() + 3 /* "\n\n" */ +
                      +4 /* leading '\n' after '#' + trailing */;
    if (used >= max_bytes || overhead >= max_bytes - used) {
      index.truncated = true;
      break;
    }
    size_t body_cap = max_bytes - used - overhead;
    std::string body(body_cap + 1, '\0');
    input.read(body.data(), static_cast<std::streamsize>(body.size()));
    size_t read = static_cast<size_t>(input.gcount());
    body.resize(std::min(read, body_cap));
    body = Utf8Prefix(std::move(body), body_cap);
    body = RedactMemorySecrets(std::move(body));
    if (Trim(body).empty()) continue;
    std::string block = "# " + memory.key + "\n" + body + "\n\n";
    std::optional<size_t> total = CheckedAdd(used, block.size());
    if (!total || *total > max_bytes) {
      // Skip oversized entries rather than starving the rest of the list.
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
