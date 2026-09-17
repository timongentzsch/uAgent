// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_AGENT_MEMORY_STORE_H_
#define UAGENT_INCLUDE_AGENT_MEMORY_STORE_H_
// Durable memory audit records and secret redaction. The memory tool, the
// background extractor and the agent loop share this store: events are the
// append-only audit trail, receipts prove a background write, and redaction
// is the last line of defense before any memory text reaches a prompt.
// Bodies live in src/agent/memory_store.cc.

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
  // What an overwrite replaced. A set on an existing key is the one path that
  // destroys memory without a forget, so the audit record keeps a preview.
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

// Deterministic last line of defense for explicit writes.
std::string RedactMemorySecrets(std::string text);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_AGENT_MEMORY_STORE_H_
