// Copyright 2026 Timon Gentzsch

#include <unistd.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/core/config.h"
#include "include/media.h"
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
  CHECK(ToolWriteFile(image_path.string(), "png").output.starts_with("wrote "));
  Attachment image_attachment;
  error.clear();
  CHECK(InspectAttachment(image_path.string(), image_attachment, error));
  CHECK(ImageInputError(image_attachment, true, false).empty());
  CHECK(ImageInputError(image_attachment, false, false)
            .find(image_path.string()) != std::string::npos);
  CHECK(ImageInputError(image_attachment, false, true).empty());
  // A configured vision route reads like native vision: the prompt is silent
  // either way, so the model never plans around the difference.
  CHECK(std::string(ModelImageInputInstruction(false, true)).empty());
  CHECK(ImageInputError(attachment, false, false).empty());

  error.clear();
  json content =
      AttachmentContent("inspect", {image_attachment, attachment}, error);
  CHECK(error.empty());
  CHECK(content[0]["text"].get<std::string>().find(image_path.string()) !=
        std::string::npos);
  image_attachment.source_call_id = "call_attach_2";
  error.clear();
  json attributed = AttachmentContent("inspect", {image_attachment}, error);
  CHECK(error.empty());
  CHECK(attributed[0]["text"].get<std::string>().find("call_attach_2") !=
        std::string::npos);
  json messages =
      json::array({{{"role", "user"}, {"content", std::move(content)}}});
  CHECK(StripContentParts(messages, "image_url") == 1);

  // A route that will not read documents never gets one encoded for it: the
  // part is not built, so a large file is not read or base64'd to be stripped
  // again. The text part still names the path, which is what the model needs
  // to reach it another way.
  error.clear();
  json refused = AttachmentContent("read it", {attachment}, error,
                                   /*image_input_available=*/true,
                                   /*image_fallback_available=*/false,
                                   /*file_input_available=*/false);
  CHECK(error.empty());
  CHECK(refused.size() == 1);
  CHECK(JsonValue(refused[0], "type", "") == "text");
  CHECK(refused[0]["text"].get<std::string>().find(file.string()) !=
        std::string::npos);
  json accepted = AttachmentContent("read it", {attachment}, error);
  CHECK(accepted.size() == 2);
  CHECK(JsonValue(accepted[1], "type", "") == "file");
  CHECK(messages[0]["content"].size() == 2);
  CHECK(messages[0]["content"][0]["text"].get<std::string>().find("withheld") ==
        std::string::npos);
  CHECK(messages[0]["content"][1].value("type", "") == "file");
  CHECK(std::string(ModelImageInputInstruction(false, false))
            .find("Image input unavailable") != std::string::npos);
  CHECK(std::string(ModelImageInputInstruction(true, false)).empty());

  // The attach tool has no per-turn call cap, so this queue ceiling is what
  // bounds a runaway caller -- including an MCP server, which queues images
  // with no model call to budget against.
  setenv("UAGENT_PENDING_ATTACHMENTS", "2", 1);
  ProcessSupervisor attachment_processes;
  std::vector<Tool> attachment_tools = BuiltinTools(attachment_processes, root);
  const Tool* attach_tool = FindTool(attachment_tools, "attach");
  ToolContext attach_context;
  attach_context.call_id = "call_attach_1";
  CHECK(attach_tool != nullptr);
  CHECK(attach_tool &&
        attach_tool->run({{"path", file.string()}}, attach_context).Ok());
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

  setenv("UAGENT_IMAGE_PROTOCOL", "iterm", 1);
  CHECK(DetectTerminalImageProtocol() == TerminalImageProtocol::kIterm);
  std::string iterm;
  auto collect = [](std::string& out) {
    return [&out](std::string_view part) { out.append(part); };
  };
  EmitItermImage("YWJj", 3, 20, false, collect(iterm));
  CHECK(iterm.find("\033]1337;File=inline=1") == 0);
  CHECK(iterm.find(":YWJj\a") != std::string::npos);
  std::string multipart;
  EmitItermImage("YWJj", 3, 20, true, collect(multipart));
  CHECK(multipart.find("MultipartFile=") != std::string::npos);
  CHECK(multipart.find("FilePart=YWJj") != std::string::npos);
  CHECK(multipart.find("FileEnd") != std::string::npos);
  setenv("UAGENT_IMAGE_PROTOCOL", "kitty", 1);
  CHECK(DetectTerminalImageProtocol() == TerminalImageProtocol::kItty);
  std::string kitty;
  EmitKittyPng("YWJj", 20, collect(kitty));
  CHECK(kitty.find("\033_Ga=T,f=100,c=20") == 0);
  CHECK(kitty.find("YWJj\033\\") != std::string::npos);

  bool prior_tty = g_tty;
  g_tty = true;
  setenv("UAGENT_IMAGE_PROTOCOL", "none", 1);
  CHECK(std::string(TerminalImageInstruction()).empty());
  setenv("UAGENT_IMAGE_PROTOCOL", "kitty", 1);
  CHECK(std::string(TerminalImageInstruction())
            .find("Use show_image to display local images") !=
        std::string::npos);
  setenv("UAGENT_IMAGE_PROTOCOL", "ascii", 1);
  CHECK(DetectTerminalImageProtocol() == TerminalImageProtocol::kNone);
  CHECK(std::string(TerminalImageProtocolName(DetectTerminalImageProtocol())) ==
        "none");
  CHECK(std::string(TerminalImageInstruction()).empty());
  g_tty = prior_tty;

  unsetenv("UAGENT_IMAGE_PROTOCOL");
  const char* prior_term = getenv("TERM");
  std::string saved_term = prior_term ? prior_term : "";
  setenv("TERM", "xterm-ghostty", 1);
  CHECK(DetectTerminalImageProtocol() == TerminalImageProtocol::kItty);
  if (prior_term) {
    setenv("TERM", saved_term.c_str(), 1);
  } else {
    unsetenv("TERM");
  }

  std::error_code ec;
  fs::remove_all(root, ec);
}

}  // namespace uagent
