// Copyright 2026 Timon Gentzsch
#include "include/app/prompt_control.h"

#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

#include "include/agent/prompt.h"
#include "include/app/library.h"
#include "include/cli.h"
#include "include/core/config_document.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/lease.h"
#include "include/tools/files.h"

namespace uagent {
json PromptControl(const json& request, AdaptiveSystemState* state,
                   const std::string& base, const json& context) {
  for (const auto* field : {"scope", "action", "mode", "text", "old", "new",
                            "revision", "operation"}) {
    if (request.contains(field) && !request[field].is_string()) {
      return {{"error", std::string(field) + " must be a string."}};
    }
  }
  const auto scope =
      JsonValue(request, "scope", state ? "conversation" : "global");
  const auto action = JsonValue(request, "action", "show");
  if ((scope != "global" && scope != "project" && scope != "conversation") ||
      (scope == "conversation" && !state)) {
    return {{"error", "Choose global, project or an active conversation."}};
  }
  const bool write = action == "set" || action == "edit" || action == "reset";
  if (!write && action != "show" && action != "preview") {
    return {
        {"error", "Prompt action must be show, preview, set, edit or reset."}};
  }
  std::string error;
  FileLease lease;
  if (write && scope != "conversation" &&
      !lease.Acquire(UagentDir("library") + "/write.lock", error)) {
    return {{"error", error}};
  }
  auto documents = PromptDocuments(state);
  auto& item = documents[scope == "global" ? 0 : scope == "project" ? 1 : 2];
  auto current = ResolvePrompt(base, documents, context);
  auto inspect = [&scope](json value) {
    // Return the selected document and its inherited text once, plus source
    // provenance. Repeating every source body balloons bounded IPC frames.
    if (JsonArray(value, "sources")) {
      for (auto& source : value["sources"]) {
        if (!source.contains("revision")) {
          source["revision"] = HashHex(JsonValue(source, "text", ""));
        }
        source.erase("text");
      }
    }
    if (value.contains("inherited")) {
      auto inherited = value["inherited"][scope];
      value["inherited"] = {{scope, std::move(inherited)}};
    }
    value.erase("editable");
    return value;
  };
  // An invalid file remains inspectable and can be explicitly reset/replaced.
  if (action == "show") {
    current["item"] = item;
    return inspect(std::move(current));
  }
  if (!request.contains("revision") ||
      request["revision"] != item["revision"]) {
    return {{"error", "System prompt changed. Reload before saving."},
            {"conflict", true}};
  }
  const auto old_mode = JsonValue(item, "mode", "inherit");
  const auto old_text = JsonValue(item, "text", "");
  auto mode = action == "reset"
                  ? "inherit"
                  : JsonValue(request, "mode",
                              old_mode == "inherit" ? "overlay" : old_mode);
  auto text = action == "reset" ? "" : JsonValue(request, "text", "");
  if ((action == "set" || action == "preview") && mode != "inherit" &&
      !request.contains("text") &&
      JsonValue(request, "operation", "") != "edit") {
    return {{"error", "set requires text; use reset to inherit."}};
  }
  if (action == "edit" ||
      (action == "preview" && JsonValue(request, "operation", "") == "edit")) {
    text = old_text;
    if (old_mode == "inherit") {
      mode = "replace";
      text = JsonValue(JsonValue(current, "inherited", json::object()),
                       scope.c_str(), "");
    }
    if (auto failure =
            ApplyFileEdits(text, "system prompt",
                           {{JsonValue(request, "old", ""),
                             JsonValue(request, "new", ""), false}})) {
      return {{"error", failure->output}};
    }
  }
  if ((mode != "overlay" && mode != "replace" && mode != "inherit") ||
      text.size() > kAdaptiveSystemBytes ||
      text.find('\0') != std::string::npos) {
    return {{"error",
             "Prompt needs inherit, overlay or replace mode and at most 64 KiB "
             "of text."}};
  }
  if (mode == "inherit") text.clear();
  item.erase("error");
  item["mode"] = mode;
  item["text"] = text;
  auto result = ResolvePrompt(base, documents, context);
  if (result.contains("error")) return result;
  result["item"] = item;
  result["diff"] = ConfigUnifiedDiff(
      old_mode + "\n" + old_text, mode + "\n" + text, scope + " system prompt");
  result["applies"] =
      "Next model request; requests already in flight are unchanged.";
  if (!write ||
      (!current.contains("error") && old_mode == mode && old_text == text)) {
    return inspect(std::move(result));
  }
  if (scope == "conversation") {
    state->mode = mode == "inherit" ? "overlay" : mode;
    state->instructions = text;
    ++state->revision;
  } else {
    const auto path = PromptDocumentPath(scope);
    const auto root = std::filesystem::path(path).parent_path();
    if (!LibraryPath(root, path) || !LibraryPath(root.parent_path(), root)) {
      return {{"error", "Unsafe system prompt path."}};
    }
    if (mode == "inherit") {
      std::error_code ec;
      std::filesystem::remove(path, ec);
      if (ec) return {{"error", ec.message()}};
    } else {
      CreatePrivateDirectories(root);
      if (!AtomicWriteFile(path,
                           JsonDump({{"mode", mode}, {"text", text}}, 2) + "\n",
                           kPrivateFileMode, true, error)) {
        return {{"error", error}};
      }
    }
    LibraryChanged();
  }
  result["item"] = ReadPromptDocument(scope, state);
  Emit(Event{EventId::kPromptChanged,
             {{"scope", scope}, {"revision", result["item"]["revision"]}}});
  return inspect(std::move(result));
}

json PromptCommand(const std::string& argument, const PromptController& control,
                   bool browser) {
  std::istringstream input(argument);
  std::string action = "show", flag, value, file;
  input >> action;
  json request = {{"action", action}, {"scope", "conversation"}};
  while (input >> flag) {
    if (!(input >> std::quoted(value)) ||
        (flag != "--scope" && flag != "--mode" && flag != "--file")) {
      return {{"error",
               "Use /prompt show|edit|set|reset [--scope "
               "global|project|conversation] [--mode overlay|replace] [--file "
               "PATH]."}};
    }
    if (flag == "--file") {
      file = value;
    } else {
      request[flag.substr(2)] = value;
    }
  }
  if (action == "show") return control(request);
  auto current = control({{"action", "show"}, {"scope", request["scope"]}});
  if (!current.contains("item")) return current;
  request["revision"] = current["item"]["revision"];
  if (action == "edit") {
    if (current.contains("error")) return current;
    if (browser) return {{"editor", true}, {"scope", request["scope"]}};
    const auto mode = JsonValue(current["item"], "mode", "inherit");
    auto text = mode == "inherit"
                    ? current["inherited"][request["scope"].get<std::string>()]
                          .get<std::string>()
                    : JsonValue(current["item"], "text", "");
    bool cancelled = false;
    text = ReadInteraction(
        {.kind = "editor", .prompt = "Edit system prompt", .initial = text},
        &cancelled);
    if (cancelled) return {{"error", "Prompt edit cancelled; nothing saved."}};
    request["action"] = "set";
    request["mode"] = mode == "inherit" ? "replace" : mode;
    request["text"] = text;
  } else if (action == "set") {
    std::string text, error;
    if (file.empty() ||
        !ReadRegularFile(file, kAdaptiveSystemBytes, text, error)) {
      return {{"error", file.empty() ? "set requires --file PATH" : error}};
    }
    request["text"] = text;
    if (!request.contains("mode")) request["mode"] = "overlay";
  }
  return control(request);
}
}  // namespace uagent
