// Copyright 2026 Timon Gentzsch

#include "include/core/sandbox.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/core/defaults.h"

namespace uagent {
namespace {

// Every candidate root, in the order a rejection notice should list them.
// /dev earns its place on both platforms, not just Linux: under a blanket
// file-write deny even `2>/dev/null` fails, which is most shell commands. One
// entry covers /dev/shm too, since the nested fold below absorbs it.
constexpr std::string_view kFixedRoots[] = {"/tmp", "/var/tmp", "/dev"};

bool Acceptable(std::string_view root, std::string_view global_base) {
  if (root.empty() || root.front() != '/') return false;
  if (root == "/") return false;
  // An ancestor of ~/.uagent would hand over the saved settings and the trust
  // store by inheritance, and so would the folder that holds them. Any other
  // descendant is fine and is how terminals/logs gets in.
  return !SandboxPathWithin(global_base, root) &&
         !SandboxPathWithin(root, std::string(global_base) + "/config");
}

void Offer(std::string_view root, const SandboxInputs& inputs,
           std::vector<std::string>* accepted,
           std::vector<std::string>* rejected) {
  if (root.empty()) return;
  if (!Acceptable(root, inputs.global_base)) {
    rejected->emplace_back(root);
    return;
  }
  accepted->emplace_back(root);
}

// Sorting first puts a parent immediately before everything nested inside it,
// so one pass folds the whole tree away and the duplicates with it. A root
// inside a withheld path stays: folding it into the parent would lose it.
void FoldNested(std::vector<std::string>* roots,
                const std::vector<std::string>& withheld) {
  std::sort(roots->begin(), roots->end());
  std::vector<std::string> kept;
  for (std::string& root : *roots) {
    const bool in_withheld = std::any_of(
        withheld.begin(), withheld.end(),
        [&](const std::string& path) { return SandboxPathWithin(root, path); });
    const bool covered =
        std::any_of(kept.begin(), kept.end(), [&](const std::string& parent) {
          return SandboxPathWithin(root, parent);
        });
    const bool duplicate =
        std::find(kept.begin(), kept.end(), root) != kept.end();
    if (duplicate || (covered && !in_withheld)) continue;
    kept.push_back(std::move(root));
  }
  *roots = std::move(kept);
}

std::string QuoteSbpl(std::string_view path) {
  std::string out = "\"";
  for (char character : path) {
    if (character == '"' || character == '\\') out += '\\';
    out += character;
  }
  out += '"';
  return out;
}

// Denies unlinking, which is how seatbelt names a rename, every folder above
// `path`: the rules match paths, so no ancestor may be moved out from under
// them.
std::string DenyUnlinkAbove(const std::string& path) {
  std::string out = "(deny file-write-unlink";
  for (size_t slash = path.find('/', 1); slash != std::string::npos;
       slash = path.find('/', slash + 1)) {
    out += " (literal " + QuoteSbpl(path.substr(0, slash)) + ")";
  }
  return out + ")\n";
}

// The home folder's visible entries are closed to reads, except the folders
// inside them commands need: the folder, the writable roots and the read
// roots. An entry inside one of those stays open whole.
void ConfineReads(const SandboxInputs& inputs, SandboxPolicy* policy) {
  std::vector<std::string> open = inputs.read_roots;
  open.push_back(inputs.workspace);
  open.insert(open.end(), policy->writable_roots.begin(),
              policy->writable_roots.end());
  for (const std::string& entry : inputs.closed_folders) {
    if (std::none_of(open.begin(), open.end(), [&](const std::string& root) {
          return SandboxPathWithin(entry, root);
        })) {
      policy->unreadable.push_back(entry);
    }
  }
  FoldNested(&policy->unreadable, {});
  for (std::string& root : open) {
    if (std::any_of(policy->unreadable.begin(), policy->unreadable.end(),
                    [&](const std::string& closed) {
                      return SandboxPathWithin(root, closed);
                    })) {
      policy->readable.push_back(std::move(root));
    }
  }
  FoldNested(&policy->readable, {});
}

}  // namespace

bool ParseSandboxPreset(std::string_view text, SandboxPreset* preset) {
  struct Spelling {
    std::string_view text;
    SandboxPreset preset;
  };
  for (const Spelling& spelling :
       {Spelling{"readonly", SandboxPreset::kReadOnly},
        {"default", SandboxPreset::kDefault},
        {"relaxed", SandboxPreset::kRelaxed},
        {"off", SandboxPreset::kOff}}) {
    if (text == spelling.text) {
      *preset = spelling.preset;
      return true;
    }
  }
  return false;
}

bool SandboxPathWithin(std::string_view path, std::string_view root) {
  if (root.empty() || path.size() < root.size()) return false;
  if (path.substr(0, root.size()) != root) return false;
  // Guard the boundary in both directions: "/devices" is not inside "/dev",
  // and a root recorded with a trailing slash must still match its own path.
  if (path.size() == root.size()) return true;
  return root.back() == '/' || path[root.size()] == '/';
}

bool SandboxWritable(const SandboxPolicy& policy, std::string_view path) {
  return std::any_of(
      policy.writable_roots.begin(), policy.writable_roots.end(),
      [&](const std::string& root) { return SandboxPathWithin(path, root); });
}

bool SandboxReadable(const SandboxPolicy& policy, std::string_view path) {
  auto within = [&](const std::vector<std::string>& roots) {
    return std::any_of(
        roots.begin(), roots.end(),
        [&](const std::string& root) { return SandboxPathWithin(path, root); });
  };
  return !within(policy.unreadable) || within(policy.readable);
}

namespace {

std::string WritesNote(const SandboxPolicy& policy,
                       std::string_view workspace) {
  std::string roots;
  for (const std::string& root : policy.writable_roots) {
    roots += (roots.empty() ? "" : ", ") + root;
  }
  // A refused connection reads as a failed lookup, so the note says it too.
  const std::string offline =
      policy.allow_network ? "" : ", and reach no network";
  if (policy.read_only) {
    return "commands here only read: they may write temporary files under " +
           roots + offline;
  }
  if (SandboxWritable(policy, workspace)) {
    std::string note = "commands may write only under " + roots;
    for (const std::string& path : policy.withheld) {
      note += ", not " + path;
    }
    return note + offline;
  }
  // Reads around a hidden path are granted folder by folder as a command
  // starts, so a folder that holds one goes stale for a running command.
  const bool stale = std::any_of(policy.hidden.begin(), policy.hidden.end(),
                                 [&](const std::string& path) {
                                   return SandboxPathWithin(path, workspace);
                                 });
  return "commands cannot write the folder " + std::string(workspace) +
         ", which holds µAgent's own state; they may write only under " +
         roots + offline +
         ". write_file, edit_file and delete_file still work in the "
         "folder" +
         (stale ? ", but a command still running cannot read a file made "
                  "directly in it after the command started"
                : "");
}

}  // namespace

std::string SandboxWritesNote(const SandboxPolicy& policy,
                              std::string_view workspace) {
  std::string note = WritesNote(policy, workspace);
  if (!policy.unreadable.empty()) {
    // The folder that holds it is the one a command reaches for first.
    auto around =
        std::find_if(policy.unreadable.begin(), policy.unreadable.end(),
                     [&](const std::string& closed) {
                       return SandboxPathWithin(workspace, closed);
                     });
    note +=
        "; in the home folder they read only this folder and the folders "
        "their tools use, not other folders such as " +
        (around != policy.unreadable.end() ? *around
                                           : policy.unreadable.front()) +
        " (sandbox.read opens one)";
  }
  return note;
}

SandboxPolicyResult BuildSandboxPolicy(const SandboxInputs& inputs) {
  SandboxPolicyResult result;
  result.policy.allow_network = inputs.allow_network;
  result.policy.read_only = inputs.read_only;
  std::vector<std::string> accepted;
  if (!inputs.read_only) {
    Offer(inputs.workspace, inputs, &accepted, &result.rejected);
  }
  Offer(inputs.tmpdir, inputs, &accepted, &result.rejected);
  Offer(inputs.user_tmpdir, inputs, &accepted, &result.rejected);
  for (std::string_view root : kFixedRoots) {
    Offer(root, inputs, &accepted, &result.rejected);
  }
  // A detached command's log pump writes there; its logs are µAgent's.
  if (inputs.read_only) {
    Offer(inputs.terminal_logs, inputs, &accepted, &result.rejected);
    FoldNested(&accepted, result.policy.withheld);
    result.policy.writable_roots = std::move(accepted);
    ConfineReads(inputs, &result.policy);
    return result;
  }
  for (const std::string& cache : inputs.tool_caches) {
    Offer(cache, inputs, &accepted, &result.rejected);
  }
  Offer(inputs.terminal_logs, inputs, &accepted, &result.rejected);
  for (const std::string& root : inputs.extra_roots) {
    Offer(root, inputs, &accepted, &result.rejected);
  }
  for (const std::string& root : inputs.git_roots) {
    Offer(root, inputs, &accepted, &result.rejected);
  }
  // The relaxed level's home is no ordinary root: it holds ~/.uagent, which
  // Acceptable() would refuse, so it is withheld from it here instead.
  if (!inputs.open_home.empty()) {
    accepted.push_back(inputs.open_home);
    result.policy.withheld = {inputs.global_base};
  }
  FoldNested(&accepted, result.policy.withheld);
  result.policy.writable_roots = std::move(accepted);
  // A trusted folder's .mcp.json carries authority over the session
  // running in it, and a repository's config and hooks run
  // commands in your own git later; all sit inside the folder, which is
  // writable by design. Carving them back out is the folder-scoped twin of
  // the ancestor screen above -- with the same aim and, on Linux, no way to
  // say it: Landlock has no deny form, so expressing this there would mean not
  // granting the folder at all.
  if (!inputs.workspace.empty()) {
    result.policy.denied_writes = {inputs.workspace + "/" + kMcpConfigFile};
    if (inputs.git_repository) {
      result.policy.denied_writes.push_back(inputs.workspace + "/.git/config");
      result.policy.denied_writes.push_back(inputs.workspace + "/.git/hooks");
    }
  }
  ConfineReads(inputs, &result.policy);
  return result;
}

std::string SeatbeltProfile(const SandboxPolicy& policy) {
  std::string out = "(version 1)\n(allow default)\n(deny file-write*)\n";
  for (const std::string& root : policy.writable_roots) {
    out += "(allow file-write* (subpath " + QuoteSbpl(root) + "))\n";
  }
  // A root granted inside a withheld path is granted again after the denial.
  for (const std::string& withheld : policy.withheld) {
    out += "(deny file-write* (subpath " + QuoteSbpl(withheld) + "))\n";
    for (const std::string& root : policy.writable_roots) {
      if (SandboxPathWithin(root, withheld)) {
        out += "(allow file-write* (subpath " + QuoteSbpl(root) + "))\n";
      }
    }
    out += DenyUnlinkAbove(withheld);
  }
  for (const std::string& denied : policy.denied_writes) {
    out += "(deny file-write* (subpath " + QuoteSbpl(denied) + "))\n";
  }
  if (!policy.allow_network) out += "(deny network*)\n";
  // Reading a file, or listing a folder, is file-read-data; stat-ing it is
  // not, so a path still resolves through a closed folder.
  for (const std::string& closed : policy.unreadable) {
    out += "(deny file-read-data (subpath " + QuoteSbpl(closed) + "))\n";
  }
  for (const std::string& open : policy.readable) {
    out += "(allow file-read-data (subpath " + QuoteSbpl(open) + "))\n";
  }
  // Last, so no writable or readable root grants it back. A unix-socket connect
  // is a network operation to seatbelt, hence the second rule; the rules match
  // paths, so no ancestor may be renamed out from under them either.
  for (const std::string& hidden : policy.hidden) {
    out += "(deny file-read* file-write* (subpath " + QuoteSbpl(hidden) +
           "))\n(deny network-outbound (remote unix-socket (path-prefix " +
           QuoteSbpl(hidden) + ")))\n" + DenyUnlinkAbove(hidden);
  }
  if (out.size() > kSeatbeltProfileLimit) return {};
  return out;
}

json SandboxPolicyJson(const SandboxPolicy& policy) {
  return {{"network", policy.allow_network}, {"roots", policy.writable_roots},
          {"denied", policy.denied_writes},  {"withheld", policy.withheld},
          {"hidden", policy.hidden},         {"unreadable", policy.unreadable},
          {"readable", policy.readable},     {"read_only", policy.read_only}};
}

bool SandboxPolicyFromJson(const json& value, SandboxPolicy* policy) {
  if (!value.is_object() || !value.contains("network") ||
      !value["network"].is_boolean() || !value.contains("roots")) {
    return false;
  }
  auto paths = [&](const char* key, std::vector<std::string>* out) {
    out->clear();
    const auto found = value.find(key);
    if (found == value.end()) return true;
    if (!found->is_array()) return false;
    for (const json& path : *found) {
      if (!path.is_string()) return false;
      out->push_back(path.get<std::string>());
    }
    return true;
  };
  policy->allow_network = value["network"].get<bool>();
  policy->read_only = JsonValue(value, "read_only", false);
  return paths("roots", &policy->writable_roots) &&
         paths("denied", &policy->denied_writes) &&
         paths("withheld", &policy->withheld) &&
         paths("hidden", &policy->hidden) &&
         paths("unreadable", &policy->unreadable) &&
         paths("readable", &policy->readable);
}

}  // namespace uagent
