// Copyright 2026 Timon Gentzsch

#include <unistd.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/jobs.h"
#include "include/core/config.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"
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
  json twin = json::array(
      {{{"role", "user"},
        {"content", AttachmentContent("again", {image_attachment}, error)}}});
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
  CHECK(JsonValue(request[0]["content"][2], "text", "")
            .find("earlier attachment") != std::string::npos);
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
  json resolved = json::array({{{"path", image_path.string()},
                                {"name", "steer.png"},
                                {"mime", "image/png"},
                                {"image", true},
                                {"id", "abc123"}}});
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

// HEIC stills and SVG diagrams enter the image pipeline; MP4 video,
// HTML, and mislabeled files must not. Sniffing and inspection need no
// converter, so this stays deterministic on every platform. Conversion
// itself reuses the exercised PreparedImage machinery.
void TestVectorAndHeicAttachments() {
  // HEIC shares the ISO BMFF ftyp box with MP4: the brand decides.
  const std::string box = std::string("\x00\x00\x00\x18", 4) + "ftyp";
  const std::string pad = std::string("\x00\x00\x00\x00", 4);
  for (const char* brand : {"heic", "heix", "hevc", "mif1", "msf1"}) {
    CHECK(RasterMime(box + brand + pad) == "image/heic");
  }
  for (const char* brand : {"isom", "mp42", "avc1", "avif"}) {
    CHECK(RasterMime(box + brand + pad).empty());
  }
  CHECK(RasterMime("ftyp").empty());
  CHECK(RasterMime("").empty());
  CHECK(ImageExtension("image/heic") == ".heic");
  CHECK(ImageExtension("image/svg+xml") == ".svg");

  // SVG recognition scans the prolog, not just the first bytes.
  CHECK(SvgMime("<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>") ==
        "image/svg+xml");
  CHECK(SvgMime("<?xml version=\"1.0\"?>\n<svg width=\"8\">") ==
        "image/svg+xml");
  CHECK(SvgMime("\xEF\xBB\xBF  \n<!-- c -->\n<!DOCTYPE svg>\n<svg>") ==
        "image/svg+xml");
  CHECK(SvgMime("<?xml version=\"1.0\"?>" + std::string(200, ' ') + "<svg>") ==
        "image/svg+xml");
  CHECK(SvgMime("<SVG></SVG>") == "image/svg+xml");
  CHECK(SvgMime("<html><body></body></html>").empty());
  CHECK(SvgMime("<?xml version=\"1.0\"?><html/>").empty());
  CHECK(SvgMime("hello world").empty());
  CHECK(SvgMime("<!-- unterminated").empty());
  CHECK(SvgMime("<svgx></svgx>").empty());

  namespace fs = std::filesystem;
  fs::path root =
      fs::temp_directory_path() /
      ("uagent-vector-test-" + std::to_string(static_cast<int64_t>(getpid())));
  fs::create_directories(root);
  Attachment attachment;
  std::string error;
  fs::path diagram = root / "diagram.svg";
  CHECK(ToolWriteFile(diagram.string(),
                      "<?xml version=\"1.0\"?>\n<svg "
                      "xmlns=\"http://www.w3.org/2000/svg\"></svg>")
            .Ok());
  CHECK(InspectAttachment(diagram.string(), attachment, error));
  CHECK(attachment.image);
  CHECK(attachment.mime == "image/svg+xml");
  // A long prolog hides <svg past the old 32-byte sniff window.
  fs::path prolog = root / "prolog.svg";
  CHECK(ToolWriteFile(prolog.string(), "<?xml version=\"1.0\"?>\n<!--" +
                                           std::string(300, 'x') +
                                           "-->\n<svg></svg>")
            .Ok());
  error.clear();
  CHECK(InspectAttachment(prolog.string(), attachment, error));
  CHECK(attachment.mime == "image/svg+xml");
  // Text renamed to .svg fails closed instead of reaching a converter.
  fs::path fake = root / "fake.svg";
  CHECK(ToolWriteFile(fake.string(), "just some text").Ok());
  error.clear();
  CHECK(!InspectAttachment(fake.string(), attachment, error));
  CHECK(error.find("not an SVG document") != std::string::npos);
  // A HEIC header inspects as an image; decoding happens later.
  fs::path photo = root / "photo.heic";
  CHECK(ToolWriteFile(photo.string(), box + "heic" + pad + pad).Ok());
  error.clear();
  CHECK(InspectAttachment(photo.string(), attachment, error));
  CHECK(attachment.image);
  CHECK(attachment.mime == "image/heic");
  // MP4 bytes never enter the image pipeline, whatever the name.
  fs::path clip = root / "clip.mp4";
  CHECK(ToolWriteFile(clip.string(), box + "isom" + pad + pad).Ok());
  error.clear();
  CHECK(InspectAttachment(clip.string(), attachment, error));
  CHECK(!attachment.image);
  // Truncated HEIC bytes can never pass for a photo at request time:
  // conversion fails loudly instead of sending provider-rejected bytes.
  ProviderCapabilities capabilities;
  capabilities.SetInputModalities(json::array({"text", "image"}));
  // Re-inspect the photo so the part below carries the image claim.
  error.clear();
  CHECK(InspectAttachment(photo.string(), attachment, error));
  json request = json::array(
      {{{"role", "user"},
        {"content", AttachmentContent("look", {attachment}, error)}}});
  json deliveries;
  CHECK(PrepareAttachments(request, capabilities, false, "vision", error,
                           &deliveries));
  CHECK(!error.empty());
  CHECK(JsonDump(request).find("image_url") == std::string::npos);
  CHECK(deliveries[0]["delivery"] == "File reference");

  std::error_code cleanup;
  fs::remove_all(root, cleanup);
}

// Speech and video ride the same inspect/prepare flow as images: magic
// first, declared container on inconclusive sniff, provider-judged after.
void TestAudioVideoAttachments() {
  namespace fs = std::filesystem;
  fs::path root = fs::temp_directory_path() / "uagent-av-attachments";
  std::error_code setup;
  fs::remove_all(root, setup);
  fs::create_directories(root, setup);
  Attachment attachment;
  std::string error;
  // ID3 tags, RIFF/WAVE, FLAC and OggS sniff to speech types.
  fs::path song = root / "song.mp3";
  CHECK(
      ToolWriteFile(song.string(), std::string("ID3\x04\x00\x00\x00", 7)).Ok());
  CHECK(InspectAttachment(song.string(), attachment, error));
  CHECK(attachment.mime == "audio/mpeg");
  CHECK(!attachment.image);
  fs::path wave = root / "clip.wav";
  CHECK(
      ToolWriteFile(wave.string(), std::string("RIFF\x24\x00\x00\x00WAVE", 12))
          .Ok());
  CHECK(InspectAttachment(wave.string(), attachment, error));
  CHECK(attachment.mime == "audio/wav");
  // BMFF video brands sniff to video/mp4, the inverse of the HEIC check.
  fs::path clip = root / "clip.mp4";
  CHECK(ToolWriteFile(clip.string(), std::string("\x00\x00\x00\x20", 4) +
                                         "ftyp" + "mp41" +
                                         std::string(64, '\0'))
            .Ok());
  CHECK(InspectAttachment(clip.string(), attachment, error));
  CHECK(attachment.mime == "video/mp4");
  CHECK(!attachment.image);
  // Magic wins over the declared type the way it does for images: a PNG
  // renamed .mp3 stays an image and never enters the speech pipeline.
  fs::path spoof = root / "spoof.mp3";
  CHECK(
      ToolWriteFile(spoof.string(), std::string("\x89PNG\r\n\x1a\n", 8) +
                                        std::string("\x00\x00\x00\x0dIHDR", 8) +
                                        std::string(16, '\0'))
          .Ok());
  CHECK(InspectAttachment(spoof.string(), attachment, error));
  CHECK(attachment.image);
  // An inconclusive sniff keeps the declared container for the provider.
  fs::path memo = root / "memo.m4a";
  CHECK(ToolWriteFile(memo.string(), "voice memo bytes").Ok());
  CHECK(InspectAttachment(memo.string(), attachment, error));
  CHECK(attachment.mime == "audio/mp4");
  // Wire shapes: raw base64 plus a format word for speech, a data URL
  // mirroring image_url for video.
  ProviderCapabilities capabilities;
  capabilities.SetInputModalities(json::array({"text", "audio", "video"}));
  CHECK(capabilities.audio_input);
  CHECK(capabilities.video_input);
  CHECK(!capabilities.image_input);
  CHECK(!capabilities.file_input);
  error.clear();
  CHECK(InspectAttachment(song.string(), attachment, error));
  json request = json::array(
      {{{"role", "user"},
        {"content", AttachmentContent("transcribe", {attachment}, error)}}});
  json deliveries;
  CHECK(PrepareAttachments(request, capabilities, false, "hearing", error,
                           &deliveries));
  CHECK(error.empty());
  CHECK(deliveries[0]["delivery"] == "Audio");
  const json* audio = JsonObject(request[0]["content"][1], "input_audio");
  CHECK(audio != nullptr);
  CHECK(JsonValue(*audio, "format", "") == "mp3");
  CHECK(!JsonValue(*audio, "data", "").empty());
  error.clear();
  CHECK(InspectAttachment(clip.string(), attachment, error));
  request = json::array(
      {{{"role", "user"},
        {"content", AttachmentContent("watch", {attachment}, error)}}});
  deliveries = json();
  CHECK(PrepareAttachments(request, capabilities, false, "seeing", error,
                           &deliveries));
  CHECK(error.empty());
  CHECK(deliveries[0]["delivery"] == "Video");
  CHECK(JsonValue(request[0]["content"][1]["video_url"], "url", "")
            .starts_with("data:video/mp4;base64,"));
  // Without the flags both kinds degrade to file paths, like images.
  capabilities.SetInputModalities(json::array({"text"}));
  error.clear();
  CHECK(InspectAttachment(song.string(), attachment, error));
  request = json::array(
      {{{"role", "user"},
        {"content", AttachmentContent("transcribe", {attachment}, error)}}});
  deliveries = json();
  CHECK(PrepareAttachments(request, capabilities, false, "deaf", error,
                           &deliveries));
  CHECK(deliveries[0]["delivery"] == "File reference");
  CHECK(JsonDump(request).find("input_audio") == std::string::npos);
  CHECK(std::string(ModelAudioInputInstruction(false))
            .find("Audio input unavailable") != std::string::npos);
  CHECK(std::string(ModelVideoInputInstruction(false))
            .find("Video input unavailable") != std::string::npos);
  CHECK(std::string(ModelAudioInputInstruction(true)).empty());
  CHECK(std::string(ModelVideoInputInstruction(true)).empty());

  std::error_code cleanup;
  fs::remove_all(root, cleanup);
}

}  // namespace uagent
