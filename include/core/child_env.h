// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_CHILD_ENV_H_
#define UAGENT_INCLUDE_CORE_CHILD_ENV_H_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uagent {

using EnvironmentOverrides = std::vector<std::pair<std::string, std::string>>;

// What a child process is told. Every child loses credentials. A program
// that is not uagent also loses uagent's settings: they configure this
// session, and a build or test run under them would behave as though the
// user had set them.
enum class ChildEnvironmentPolicy {
  kSanitized,      // a program: an MCP server, a script
  kApprovedShell,  // a command the user's allow-list may hand credentials
  kAgent,          // a child uagent, told this session's settings
};

class ChildEnvironment {
 public:
  explicit ChildEnvironment(
      const EnvironmentOverrides& overrides = {},
      ChildEnvironmentPolicy policy = ChildEnvironmentPolicy::kSanitized);
  ChildEnvironment(const ChildEnvironment&) = delete;
  ChildEnvironment& operator=(const ChildEnvironment&) = delete;
  ChildEnvironment(ChildEnvironment&&) = delete;
  ChildEnvironment& operator=(ChildEnvironment&&) = delete;

  char** Data() { return pointers_.data(); }
  bool Contains(std::string_view key) const;

 private:
  std::vector<std::string> values_;
  std::vector<char*> pointers_;
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_CHILD_ENV_H_
