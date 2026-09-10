// Copyright 2026 Timon Gentzsch
#include <array>
#include <string>
#include <utility>

#include "include/agent/prompt.h"
#include "include/core/config.h"
#include "include/core/file_watch.h"
#include "include/core/fs.h"

namespace uagent {
std::string PromptDocumentPath(const std::string& scope) {
  return (scope == "global" ? std::filesystem::path(GlobalBase())
                            : ProjectBase(CanonicalCwd())) /
         "system-prompt.json";
}

json ReadPromptDocument(const std::string& scope,
                        const AdaptiveSystemState* state) {
  json result = {
      {"scope", scope}, {"mode", "inherit"}, {"text", ""}, {"revision", ""}};
  if (scope == "conversation") {
    if (state) {
      result["mode"] = state->instructions.empty() && state->mode == "overlay"
                           ? "inherit"
                           : state->mode;
      result["text"] = state->instructions;
      result["revision"] = std::to_string(state->revision);
    }
    return result;
  }
  const auto path = PromptDocumentPath(scope);
  result["path"] = path;
  struct Cached {
    std::string path;
    FileStamp stamp;
    json value;
  };
  static thread_local std::array<Cached, 2> cache;
  auto& entry = cache[scope == "global" ? 0 : 1];
  const auto stamp = SnapshotFile(path);
  if (entry.path == path && entry.stamp == stamp) return entry.value;
  if (PathExists(path)) {
    std::string bytes, error;
    if (!ReadRegularFile(path, kAdaptiveSystemBytes * 6 + 256, bytes, error)) {
      result["error"] = error;
    } else {
      result["revision"] = DocumentRevision(path, bytes);
      auto document = json::parse(bytes, nullptr, false);
      const auto mode = JsonValue(document, "mode", "");
      if (!document.is_object() || !document.contains("text") ||
          !document["text"].is_string() ||
          (mode != "overlay" && mode != "replace") ||
          document["text"].get_ref<const std::string&>().size() >
              kAdaptiveSystemBytes) {
        result["error"] =
            "Invalid system prompt document; file preserved: " + path;
      } else {
        result["mode"] = mode;
        result["text"] = document["text"];
      }
    }
  }
  entry = {path, stamp, result};
  return result;
}

json PromptDocuments(const AdaptiveSystemState* state) {
  json documents = json::array();
  for (const auto* scope : {"global", "project", "conversation"}) {
    documents.push_back(ReadPromptDocument(scope, state));
  }
  return documents;
}

json ResolvePrompt(const std::string& base, const json& documents,
                   const json& context) {
  std::string text = base;
  json sources = json::array({{{"scope", "built-in"},
                               {"mode", "replace"},
                               {"text", base},
                               {"active", true}}});
  json inherited = json::object();
  for (auto document : documents) {
    if (document.contains("error")) return document;
    const auto scope = JsonValue(document, "scope", "");
    inherited[scope] = text;
    const auto mode = JsonValue(document, "mode", "inherit");
    const auto body = JsonValue(document, "text", "");
    document["active"] = mode != "inherit";
    if (mode == "replace") {
      text = body;
      for (auto& source : sources) source["active"] = false;
    } else if (mode == "overlay" && !body.empty()) {
      if (!text.empty()) text += "\n\n";
      text += body;
    }
    sources.push_back(std::move(document));
  }
  const auto editable = text;
  for (auto source : context) {
    const auto body = JsonValue(source, "text", "");
    if (body.empty()) continue;
    if (!text.empty()) text += "\n\n";
    text += body;
    source["active"] = true;
    sources.push_back(std::move(source));
  }
  if (text.size() > kAdaptiveSystemBytes) {
    return {{"error",
             "The resolved system prompt exceeds 64 KiB. Shorten a layer or "
             "use replacement."}};
  }
  return {{"effective", text},       {"editable", editable},
          {"inherited", inherited},  {"sources", sources},
          {"digest", HashHex(text)}, {"bytes", text.size()}};
}
}  // namespace uagent
