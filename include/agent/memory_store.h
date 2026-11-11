// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_MEMORY_STORE_H_
#define UAGENT_INCLUDE_AGENT_MEMORY_STORE_H_
// Memory's append-only audit events, the receipts that prove a background
// write, and secret redaction before memory text reaches a prompt.

#include <string>
#include <vector>

#include "include/core/json.h"

namespace uagent {

struct MemoryEntry {
  std::string key;
  std::string path;
};

struct MemoryEvent {
  std::string action;
  std::string key;
  std::string preview;
  // A preview of what an overwrite replaced.
  std::string previous;
  std::string source_session;
  std::string workspace;
  std::string timestamp;
  bool automatic = false;
};

struct MemoryIndex {
  std::string text;
  std::vector<std::string> sources;
  bool truncated = false;
};

std::vector<MemoryEvent> LoadMemoryEvents(size_t limit = 128);
bool ReadMemoryReceipt(const std::string& path, MemoryEvent& event,
                       std::string& error);
bool WriteMemoryEvent(const MemoryEvent& event, const std::string& receipt_path,
                      std::string& error);

std::string RedactMemorySecrets(std::string text);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_MEMORY_STORE_H_
