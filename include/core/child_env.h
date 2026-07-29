// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_CHILD_ENV_H_
#define UAGENT_INCLUDE_CORE_CHILD_ENV_H_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uagent {

using EnvironmentOverrides = std::vector<std::pair<std::string, std::string>>;

enum class ChildEnvironmentPolicy {
  kSanitized,
  kApprovedShell,
};

bool SensitiveEnvironmentKey(std::string_view key);

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
