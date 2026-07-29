// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_OUTCOME_H_
#define UAGENT_INCLUDE_CORE_OUTCOME_H_

namespace uagent {

enum class CompletionStatus {
  kSuccess,
  kFailed,
  kCancelled,
  kTimedOut,
};

inline const char* CompletionStatusName(CompletionStatus status) {
  switch (status) {
    case CompletionStatus::kSuccess:
      return "success";
    case CompletionStatus::kFailed:
      return "failed";
    case CompletionStatus::kCancelled:
      return "cancelled";
    case CompletionStatus::kTimedOut:
      return "timed_out";
  }
  return "failed";
}

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_OUTCOME_H_
