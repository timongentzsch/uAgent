// Copyright 2026 Timon Gentzsch

#include <unistd.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/core/config.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"
#include "include/tools/jobs.h"
#include "include/tools/registry.h"
#include "tests/unit/test_support.h"

namespace uagent {

void TestAttachmentEncoding() {
  namespace fs = std::filesystem;
  fs::path root = fs::temp_directory_path() /
                  ("uagent-attachment-test-" +
                   std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(root);
  fs::path file = root / "tiny.txt";
  CHECK(ToolWriteFile(file.string(), "x").output.starts_with("wrote "));
  Attachment attachment;
  std::string error;
  CHECK(InspectAttachment(file.string(), attachment, error));
  CHECK(Base64File(attachment, 1, error, "data:text/plain;base64,") ==
        "data:text/plain;base64,eA==");
  error.clear();
  CHECK(Base64File(attachment, 0, error).empty());
  CHECK(!error.empty());

  fs::path image_path = root / "tiny.png";
  std::string png;
  CHECK(
      Base64Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP"
                   "8/x8AAwMCAO+a5WQAAAAASUVORK5CYII=",
                   png, 1024));
  CHECK(ToolWriteFile(image_path.string(), png).Ok());
  Attachment image_attachment;
  error.clear();
  CHECK(InspectAttachment(image_path.string(), image_attachment, error));
  CHECK(image_attachment.image);
  CHECK(image_attachment.mime == "image/png");
  image_attachment.source_call_id = "call_attach_2";
  json content =
      AttachmentContent("inspect", {image_attachment, attachment}, error);
  CHECK(error.empty());
  CHECK(content[0]["text"].get<std::string>().find("call_attach_2") !=
        std::string::npos);
  CHECK(content[1]["type"] == "attachment");
  CHECK(JsonDump(content).find("base64,") == std::string::npos);
  const json original = json::array({{{"role", "user"}, {"content", content}}});
  ProviderCapabilities capabilities;
  capabilities.SetInputModalities(json::array({"text"}));
  json request = original, deliveries;
  CHECK(PrepareAttachments(request, capabilities, false, "text-only", error,
                           &deliveries));
  CHECK(error.empty());
  CHECK(deliveries[0]["delivery"] == "File reference");
  CHECK(deliveries[1]["delivery"] == "Text");
  CHECK(JsonDump(request).find("image_url") == std::string::npos);
  CHECK(JsonDump(request).find("Attached text") != std::string::npos);
  capabilities.SetInputModalities(json::array({"text", "image", "pdf"}));
  request = original;
  CHECK(PrepareAttachments(request, capabilities, false, "vision", error,
                           &deliveries));
  CHECK(error.empty());
  CHECK(deliveries[0]["delivery"] == "Image");
  CHECK(JsonDump(request).find("image_url") != std::string::npos);
  CHECK(original[0]["content"][1]["type"] == "attachment");
  fs::path document = root / "paper.pdf";
  CHECK(ToolWriteFile(document.string(), "%PDF-1.4\\n").Ok());
  Attachment pdf;
  CHECK(InspectAttachment(document.string(), pdf, error));
  request =
      json::array({{{"role", "user"},
                    {"content", AttachmentContent("read", {pdf}, error)}}});
  CHECK(PrepareAttachments(request, capabilities, false, "vision", error,
                           &deliveries));
  CHECK(deliveries[0]["delivery"] == "Document");
  CHECK(JsonDump(request).find("file_data") != std::string::npos);
  fs::path invalid = root / "invalid.png";
  CHECK(ToolWriteFile(invalid.string(), "not a PNG").Ok());
  CHECK(!InspectAttachment(invalid.string(), image_attachment, error));
  CHECK(std::string(ModelImageInputInstruction(false, false))
            .find("Image input unavailable") != std::string::npos);

  // Re-reading an unchanged file in the same request references the first
  // copy: one payload, one delivery row, no repeat rendering per step.
  error.clear();
  json twin =
      json::array({{{"role", "user"},
                    {"content", AttachmentContent("again", {image_attachment},
                                                    error)}}});
  CHECK(error.empty());
  twin[0]["content"].push_back(twin[0]["content"][1]);
  capabilities.SetInputModalities(json::array({"text", "image", "pdf"}));
  request = twin;
  CHECK(PrepareAttachments(request, capabilities, false, "vision", error,
                           &deliveries));
  CHECK(error.empty());
  CHECK(deliveries.size() == 1);
  CHECK(deliveries[0]["delivery"] == "Image");
  CHECK(request[0]["content"].size() == 3);
  CHECK(JsonValue(request[0]["content"][2], "text", "").find(
            "earlier attachment") != std::string::npos);
  size_t image_parts = 0;
  for (const json& part : request[0]["content"]) {
    if (JsonValue(part, "type", "") == "image_url") ++image_parts;
  }
  CHECK(image_parts == 1);

  // The media read has no per-turn call cap, so this queue ceiling is what
  // bounds a runaway caller -- including an MCP server, which queues images
  // with no model call to budget against.
  setenv("UAGENT_PENDING_ATTACHMENTS", "2", 1);
  ProcessSupervisor attachment_processes;
  std::vector<Tool> attachment_tools = BuiltinTools(attachment_processes, root);
  const Tool* attach_tool = FindTool(attachment_tools, "read_path");
  ToolContext attach_context;
  attach_context.call_id = "call_attach_1";
  CHECK(attach_tool != nullptr);
  CHECK(attach_tool &&
        attach_tool->run({{"path", image_path.string()}}, attach_context).Ok());
  CHECK(Attachments().Add(file.string()).Ok());
  ToolResult refused_queue = Attachments().Add(file.string());
  CHECK(!refused_queue.Ok());
  CHECK(refused_queue.output.find("too many attachments pending") !=
        std::string::npos);
  // Draining the queue for the next request clears the ceiling again.
  std::vector<Attachment> drained = Attachments().Take();
  CHECK(drained.size() == 2);
  CHECK(drained[0].source_call_id == "call_attach_1");
  CHECK(Attachments().Add(file.string()).Ok());
  CHECK(Attachments().Take().size() == 1);
  unsetenv("UAGENT_PENDING_ATTACHMENTS");

  // Steered content composes exactly like submitted content.
  std::string steer_error;
  auto [plain, plain_kind] =
      ComposeSteeredContent("go", json::array(), steer_error);
  CHECK(steer_error.empty());
  CHECK(!plain_kind);
  CHECK(plain.get<std::string>() == "go");
  auto [missing, missing_kind] = ComposeSteeredContent(
      "go", json::array({{{"path", (root / "gone.png").string()}}}),
      steer_error);
  CHECK(!steer_error.empty());
  CHECK(!missing_kind);
  json resolved = json::array(
      {{{"path", image_path.string()}, {"name", "steer.png"},
        {"mime", "image/png"}, {"image", true}, {"id", "abc123"}}});
  steer_error.clear();
  auto [composed, composed_kind] =
      ComposeSteeredContent("look", resolved, steer_error);
  CHECK(steer_error.empty());
  CHECK(composed_kind);
  CHECK(composed.is_array());
  CHECK(composed[0]["text"].get<std::string>().find("\n\nAttached:\n- path ") !=
        std::string::npos);
  CHECK(AttachmentFromJson(resolved[0]).asset_id == "abc123");
  CHECK(AttachmentFromJson(json::object()).path.empty());

  std::error_code ec;
  fs::remove_all(root, ec);
}

}  // namespace uagent
