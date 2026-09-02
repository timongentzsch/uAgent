// Copyright 2026 Timon Gentzsch

#include "include/core/sandbox.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
  // An ancestor of ~/.uagent would hand over the config and the trust store by
  // inheritance. A descendant is fine and is how terminals/logs gets in.
  return !SandboxPathWithin(global_base, root);
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
// so one pass folds the whole tree away and the duplicates with it.
void FoldNested(std::vector<std::string>* roots) {
  std::sort(roots->begin(), roots->end());
  std::vector<std::string> kept;
  for (std::string& root : *roots) {
    if (!kept.empty() && SandboxPathWithin(root, kept.back())) continue;
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

}  // namespace

bool SandboxPathWithin(std::string_view path, std::string_view root) {
  if (root.empty() || path.size() < root.size()) return false;
  if (path.substr(0, root.size()) != root) return false;
  // Guard the boundary in both directions: "/devices" is not inside "/dev",
  // and a root recorded with a trailing slash must still match its own path.
  if (path.size() == root.size()) return true;
  return root.back() == '/' || path[root.size()] == '/';
}

SandboxPolicyResult BuildSandboxPolicy(const SandboxInputs& inputs) {
  SandboxPolicyResult result;
  result.policy.allow_network = inputs.allow_network;
  std::vector<std::string> accepted;
  Offer(inputs.workspace, inputs, &accepted, &result.rejected);
  Offer(inputs.tmpdir, inputs, &accepted, &result.rejected);
  for (std::string_view root : kFixedRoots) {
    Offer(root, inputs, &accepted, &result.rejected);
  }
  Offer(inputs.cache_dir, inputs, &accepted, &result.rejected);
  Offer(inputs.data_dir, inputs, &accepted, &result.rejected);
  Offer(inputs.terminal_logs, inputs, &accepted, &result.rejected);
  for (size_t start = 0; start < inputs.extra_roots.size();) {
    size_t end = inputs.extra_roots.find(':', start);
    if (end == std::string::npos) end = inputs.extra_roots.size();
    Offer(std::string_view(inputs.extra_roots).substr(start, end - start),
          inputs, &accepted, &result.rejected);
    start = end + 1;
  }
  FoldNested(&accepted);
  result.policy.writable_roots = std::move(accepted);
  return result;
}

std::string SeatbeltProfile(const SandboxPolicy& policy) {
  std::string out = "(version 1)\n(allow default)\n(deny file-write*)\n";
  for (const std::string& root : policy.writable_roots) {
    out += "(allow file-write* (subpath " + QuoteSbpl(root) + "))\n";
  }
  for (const std::string& denied : policy.denied_writes) {
    out += "(deny file-write* (subpath " + QuoteSbpl(denied) + "))\n";
  }
  if (!policy.allow_network) out += "(deny network*)\n";
  if (out.size() > kSeatbeltProfileLimit) return {};
  return out;
}

std::vector<std::string> EncodeSandboxPolicy(const SandboxPolicy& policy) {
  std::vector<std::string> words;
  words.reserve(policy.writable_roots.size() + 2);
  words.emplace_back(policy.allow_network ? "net=1" : "net=0");
  words.emplace_back("roots=" + std::to_string(policy.writable_roots.size()));
  words.insert(words.end(), policy.writable_roots.begin(),
               policy.writable_roots.end());
  return words;
}

bool DecodeSandboxPolicy(const std::vector<std::string>& words,
                         SandboxPolicy* policy, size_t* consumed) {
  if (words.size() < 2) return false;
  if (words[0] == "net=1") {
    policy->allow_network = true;
  } else if (words[0] == "net=0") {
    policy->allow_network = false;
  } else {
    return false;
  }
  constexpr std::string_view kRootsPrefix = "roots=";
  if (!words[1].starts_with(kRootsPrefix)) return false;
  std::string_view digits =
      std::string_view(words[1]).substr(kRootsPrefix.size());
  if (digits.empty()) return false;
  size_t count = 0;
  for (char digit : digits) {
    if (digit < '0' || digit > '9') return false;
    count = count * 10 + static_cast<size_t>(digit - '0');
    // The encoder never emits more roots than a policy holds, so a count this
    // large is a malformed argv rather than a big policy.
    if (count > words.size()) return false;
  }
  if (words.size() < 2 + count) return false;
  auto first = words.begin() + 2;
  policy->writable_roots.assign(first, first + static_cast<ptrdiff_t>(count));
  *consumed = 2 + count;
  return true;
}

}  // namespace uagent
