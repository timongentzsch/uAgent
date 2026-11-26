// Copyright 2026 Timon Gentzsch

#include <unistd.h>

#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/activity.h"
#include "include/app/present.h"
#include "include/core/capture.h"
#include "include/core/config.h"
#include "include/core/env.h"
#include "include/core/limits.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"
#include "include/tools/registry.h"
#include "tests/unit/test_support.h"

namespace uagent {

TEST_CASE(TestAttachmentEncoding) {
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
  capabilities.SetInputModalities(json::array({"text"}),
                                  WireApi::kChatCompletions);
  json request = original, deliveries;
  CHECK(PrepareAttachments(request, capabilities, false, "text-only", error,
                           &deliveries));
  CHECK(error.empty());
  CHECK(deliveries[0]["delivery"] == "File reference");
  CHECK(deliveries[1]["delivery"] == "Text");
  CHECK(JsonDump(request).find("image_url") == std::string::npos);
  CHECK(JsonDump(request).find("Attached text") != std::string::npos);
  capabilities.SetInputModalities(json::array({"text", "image", "pdf"}),
                                  WireApi::kChatCompletions);
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
  const json attached =
      json::array({{{"role", "user"},
                    {"content", AttachmentContent("read", {pdf}, error)}}});
  const std::string sent = JsonDump(request);
  request = attached;
  CHECK(PrepareAttachments(request, capabilities, false, "vision", error));
  CHECK(JsonDump(request) == sent);
  CHECK(ToolWriteFile(document.string(), "%PDF-1.5 revised").Ok());
  request = attached;
  CHECK(PrepareAttachments(request, capabilities, false, "vision", error));
  CHECK(JsonDump(request) != sent);
  CHECK(JsonDump(request).find("JVBERi0xLjUgcmV2aXNlZA==") !=
        std::string::npos);
  fs::path invalid = root / "invalid.png";
  CHECK(ToolWriteFile(invalid.string(), "not a PNG").Ok());
  CHECK(!InspectAttachment(invalid.string(), image_attachment, error));
  CHECK(std::string(ModelImageInputInstruction(false, false))
            .find("Image input unavailable") != std::string::npos);

  // An unchanged file read twice in one request references the first copy.
  error.clear();
  json twin = json::array(
      {{{"role", "user"},
        {"content", AttachmentContent("again", {image_attachment}, error)}}});
  CHECK(error.empty());
  twin[0]["content"].push_back(twin[0]["content"][1]);
  capabilities.SetInputModalities(json::array({"text", "image", "pdf"}),
                                  WireApi::kChatCompletions);
  request = twin;
  CHECK(PrepareAttachments(request, capabilities, false, "vision", error,
                           &deliveries));
  CHECK(error.empty());
  CHECK(deliveries.size() == 1);
  CHECK(deliveries[0]["delivery"] == "Image");
  // The first copy names where it is kept, for write_file's `from`.
  CHECK(request[0]["content"].size() == 4);
  CHECK(JsonValue(request[0]["content"][2], "text", "")
            .starts_with("[Attached: "));
  CHECK(JsonValue(request[0]["content"][3], "text", "")
            .find("earlier attachment") != std::string::npos);
  size_t image_parts = 0;
  for (const json& part : request[0]["content"]) {
    if (JsonValue(part, "type", "") == "image_url") ++image_parts;
  }
  CHECK(image_parts == 1);

  // The media read has no per-turn call cap: the queue ceiling bounds a runaway
  // caller, an MCP server included.
  ActivitySupervisor attachment_processes;
  std::vector<Tool> attachment_tools = BuiltinTools(attachment_processes, root);
  const Tool* attach_tool = FindTool(attachment_tools, "read_path");
  ToolContext attach_context;
  attach_context.call_id = "call_attach_1";
  CHECK(attach_tool != nullptr);
  CHECK(attach_tool &&
        attach_tool->run({{"path", image_path.string()}}, attach_context).Ok());
  for (size_t queued = 1; queued < MaxPendingAttachments(); ++queued) {
    CHECK(Attachments().Add(file.string()).Ok());
  }
  ToolResult refused_queue = Attachments().Add(file.string());
  CHECK(!refused_queue.Ok());
  CHECK(refused_queue.output.find("too many attachments pending") !=
        std::string::npos);
  std::vector<Attachment> drained = Attachments().Take();
  CHECK(drained.size() == MaxPendingAttachments());
  CHECK(drained[0].source_call_id == "call_attach_1");
  CHECK(Attachments().Add(file.string()).Ok());
  CHECK(Attachments().Take().size() == 1);

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

// A region of an image is cut at full detail; a plain read names the size
// the region is given in.
TEST_CASE(TestImageRegion) {
  TestWorkspace workspace("image-region");
  std::string png;
  CHECK(
      Base64Decode("iVBORw0KGgoAAAANSUhEUgAAAAQAAAACCAIAAADwyuo0AAAAFklEQVQI"
                   "12P8//8/AwwwQWlGRgYGBgBIDwMDBa70RgAAAABJRU5ErkJggg==",
                   png, 1024));
  CHECK(ToolWriteFile("small.png", png).Ok());
  // A picture its decoder warns about (a broken colour profile) still has a
  // size.
  std::string warned;
  CHECK(Base64Decode(
      "iVBORw0KGgoAAAANSUhEUgAAAAQAAAACCAIAAADwyuo0AAAAE2lDQ1B4AAB4nGNgoAwAAABA"
      "AAFqdW5rB8/N0gAAABBJREFUeJxjEGj4AEcMyBwAlRoMAZaWcPQAAAAASUVORK5CYII=",
      warned, 1024));
  CHECK(ToolWriteFile("warned.png", warned).Ok());
  int warned_width = 0, warned_height = 0;
  if (ImageSize("warned.png", warned_width, warned_height).Ok()) {
    CHECK(warned_width == 4 && warned_height == 2);
  }
  int width = 0, height = 0;
  if (!ImageSize("small.png", width, height).error.empty()) {
    CHECK(ToolReadImageRegion("small.png", "0,0,1,1", "cut")
              .output.find("install ImageMagick") != std::string::npos);
    return;
  }
  CHECK(ToolReadFile("small.png", 1, 1, "read").output.ends_with("; 4x2 px"));
  const ToolResult region = ToolReadImageRegion("small.png", "2,1,5,5", "cut");
  CHECK(region.Ok() && region.body_kind == "image");
  CHECK(region.hint.find("Region 2,1,2,1 of small.png (4x2 px)") !=
        std::string::npos);
  const std::vector<Attachment> queued = Attachments().Take();
  CHECK(queued.size() == 2 && queued[1].mime == "image/png" &&
        ImageSize(queued[1].path, width, height).Ok() && width == 2 &&
        height == 1);
  CHECK(ToolReadImageRegion("small.png", "4,0,1,1", "out")
            .output.find("outside the 4x2 px image") != std::string::npos);
  CHECK(!ToolReadImageRegion("small.png", "1,2", "bad").Ok());
  CHECK(ToolWriteFile("notes.txt", "text").Ok());
  CHECK(ToolReadImageRegion("notes.txt", "0,0,1,1", "text")
            .output.find("raster image") != std::string::npos);
  // ImageMagick 6 has convert and identify, and no magick.
  const char* found = getenv("PATH");
  const std::string path = found ? found : "";
  const std::filesystem::path six = workspace.root / "imagemagick-6";
  std::filesystem::create_directories(six);
  for (const char* tool : {"convert", "identify"}) {
    for (const std::string& dir : SplitPathList(path, ':')) {
      if (std::filesystem::exists(dir + "/" + tool)) {
        std::filesystem::create_symlink(dir + "/" + tool, six / tool);
        break;
      }
    }
  }
  if (!std::filesystem::exists(six / "identify")) return;
  setenv("PATH", six.c_str(), 1);
  const CapturedProcess size = ImageSize("small.png", width, height);
  const ToolResult cut = ToolReadImageRegion("small.png", "1,0,2,2", "six");
  setenv("PATH", path.c_str(), 1);
  CHECK(size.Ok() && width == 4 && height == 2);
  CHECK(cut.Ok());
  CHECK(Attachments().Take().size() == 1);
}

// A PDF reads as page text through poppler; without it the file is
// attached as before and the result says what to install.
TEST_CASE(TestPdfPages) {
  TestWorkspace workspace("pdf-pages");
  const auto pdf = [](const std::string& first, const std::string& second) {
    std::string out =
        "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
        "2 0 obj<</Type/Pages/Kids[3 0 R 5 0 R]/Count 2>>endobj\n";
    int object = 3;
    for (const std::string& text : {first, second}) {
      const std::string stream =
          text.empty() ? "" : "BT /F1 12 Tf 10 50 Td (" + text + ") Tj ET";
      out += std::to_string(object) +
             " 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 100]"
             "/Resources<</Font<</F1 7 0 R>>>>/Contents " +
             std::to_string(object + 1) + " 0 R>>endobj\n" +
             std::to_string(object + 1) + " 0 obj<</Length " +
             std::to_string(stream.size()) + ">>stream\n" + stream +
             "\nendstream endobj\n";
      object += 2;
    }
    return out +
           "7 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\n"
           "trailer<</Root 1 0 R>>\n%%EOF\n";
  };
  CHECK(ToolWriteFile("paper.pdf", pdf("Page one", "Page two")).Ok());
  CHECK(ToolWriteFile("scan.pdf", pdf("", "")).Ok());
  const char* found = getenv("PATH");
  const std::string path = found ? found : "";
  setenv("PATH", workspace.root.c_str(), 1);
  const ToolResult missing = ToolReadFile("paper.pdf", 1, 1, "missing");
  setenv("PATH", path.c_str(), 1);
  CHECK(missing.output.find("install poppler") != std::string::npos);
  CHECK(Attachments().Take().size() == 1);
  if (!CaptureProcess({"pdfinfo", "-v"}).error.empty()) return;
  const ToolResult pages = ToolReadFile("paper.pdf", 1, 0);
  CHECK(pages.output == "[page 1]\nPage one\n\n[page 2]\nPage two\n\n");
  CHECK(pages.hint == "[paper.pdf pages 1-2 of 2]");
  const ToolResult last = ToolReadFile("paper.pdf", -1, 0);
  CHECK(last.output == "[page 2]\nPage two\n\n");
  CHECK(last.hint == "[paper.pdf pages 2-2 of 2]");
  CHECK(ToolReadFile("paper.pdf", 1, 1).hint == "[paper.pdf pages 1-1 of 2]");
  CHECK(
      ToolReadFile("paper.pdf", 2, std::numeric_limits<int64_t>::max()).hint ==
      "[paper.pdf pages 2-2 of 2]");
  CHECK(!ToolReadFile("paper.pdf", 3, 0).Ok());
  const ToolResult scanned = ToolReadFile("scan.pdf", 1, 0);
  CHECK(scanned.output == "[page 1] (no text)\n[page 2] (no text)\n");
  CHECK(scanned.hint ==
        "[scan.pdf pages 1-2 of 2; no text layer, likely scanned]");
}

// A picture's size comes from its header, as a browser shows it: a JPEG
// turned by its EXIF orientation, and nothing from a file cut short.
TEST_CASE(TestRasterSize) {
  const auto bytes = [](std::initializer_list<int> values) {
    std::string out;
    for (int value : values) out.push_back(static_cast<char>(value));
    return out;
  };
  const auto size = [](const std::string& file) {
    int width = -1, height = -1;
    return RasterSize(file, width, height)
               ? std::to_string(width) + "x" + std::to_string(height)
               : std::string();
  };
  const std::string png = std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16) +
                          bytes({0, 0, 3, 0x20, 0, 0, 2, 0x58});
  const std::string gif = "GIF89a" + bytes({0x20, 3, 0x58, 2, 0, 0, 0});
  const std::string riff = "RIFF" + bytes({0, 0, 0, 0}) + "WEBP";
  const std::string lossy = riff + "VP8 " + bytes({0, 0, 0, 0, 0, 0, 0}) +
                            bytes({0x9d, 1, 0x2a, 0x20, 3, 0x58, 2});
  // 800 - 1 and 600 - 1 in 14 bits each.
  const uint32_t bits = 799 | 599u << 14;
  const std::string lossless =
      riff + "VP8L" + bytes({0, 0, 0, 0, 0x2f}) +
      bytes({static_cast<int>(bits & 0xff), static_cast<int>(bits >> 8 & 0xff),
             static_cast<int>(bits >> 16 & 0xff),
             static_cast<int>(bits >> 24)});
  const std::string extended = riff + "VP8X" + bytes({0, 0, 0, 0, 0, 0, 0, 0}) +
                               bytes({0x1f, 3, 0, 0x57, 2, 0});
  const std::string sof = bytes({0xff, 0xc0, 0, 17, 8, 2, 0x58, 3, 0x20});
  const std::string jpeg = bytes({0xff, 0xd8}) + sof;
  // APP1 with one IFD entry: orientation 6, a quarter turn.
  const std::string exif = bytes({0xff, 0xe1, 0, 34}) +
                           std::string("Exif\0\0", 6) + "MM" +
                           bytes({0, 42, 0, 0, 0, 8, 0, 1, 1, 0x12, 0, 3,
                                  0, 0,  0, 1, 0, 6, 0, 0, 0, 0,    0, 0});
  const std::string turned = bytes({0xff, 0xd8}) + exif + sof;
  for (const auto& [file, expected] :
       std::vector<std::pair<std::string, std::string>>{{png, "800x600"},
                                                        {gif, "800x600"},
                                                        {lossy, "800x600"},
                                                        {lossless, "800x600"},
                                                        {extended, "800x600"},
                                                        {jpeg, "800x600"},
                                                        {turned, "600x800"}}) {
    CHECK(size(file) == expected);
    CHECK(size(file.substr(0, file.size() - 1)).empty());
  }
  CHECK(size("plain text").empty());
}

// HEIC stills and SVG diagrams enter the image pipeline; MP4 video, HTML and
// mislabeled files do not. Sniffing and inspection need no converter.
TEST_CASE(TestVectorAndHeicAttachments) {
  // HEIC shares the ISO BMFF ftyp box with MP4: the brand decides.
  const std::string box = std::string("\x00\x00\x00\x18", 4) + "ftyp";
  const std::string pad = std::string("\x00\x00\x00\x00", 4);
  for (const char* brand : {"heic", "heix", "hevc", "mif1", "msf1"}) {
    CHECK(RasterMime(box + brand + pad) == "image/heic");
  }
  for (const char* brand : {"isom"}) {
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
  // A long prolog hides <svg past a 32-byte sniff window.
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
  fs::path clip = root / "clip.mp4";
  CHECK(ToolWriteFile(clip.string(), box + "isom" + pad + pad).Ok());
  error.clear();
  CHECK(InspectAttachment(clip.string(), attachment, error));
  CHECK(!attachment.image);
  // Truncated HEIC bytes can never pass for a photo at request time:
  // conversion fails loudly instead of sending provider-rejected bytes.
  ProviderCapabilities capabilities;
  capabilities.SetInputModalities(json::array({"text", "image"}),
                                  WireApi::kChatCompletions);
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
  // The model reads why it got no picture.
  CHECK(JsonDump(request).find("contents not included: ") != std::string::npos);

  std::error_code cleanup;
  fs::remove_all(root, cleanup);
}

// Speech and video ride the same inspect/prepare flow as images: magic
// first, declared container on inconclusive sniff, provider-judged after.
TEST_CASE(TestAudioVideoAttachments) {
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
  // Magic wins over the declared type: a PNG renamed .mp3 stays an image.
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
  capabilities.SetInputModalities(json::array({"text", "audio", "video"}),
                                  WireApi::kChatCompletions);
  CHECK(capabilities.audio_input);
  CHECK(capabilities.video_input);
  CHECK(!capabilities.image_input);
  CHECK(!capabilities.file_input);
  json deliveries;
  auto prepare = [&](const fs::path& path, const char* label) {
    error.clear();
    CHECK(InspectAttachment(path.string(), attachment, error));
    json prepared = json::array(
        {{{"role", "user"},
          {"content", AttachmentContent("take this", {attachment}, error)}}});
    deliveries = json();
    CHECK(PrepareAttachments(prepared, capabilities, false, label, error,
                             &deliveries));
    return prepared;
  };
  json request = prepare(song, "hearing");
  CHECK(error.empty());
  CHECK(deliveries[0]["delivery"] == "Audio");
  const json* audio = JsonObject(request[0]["content"][1], "input_audio");
  CHECK(audio != nullptr);
  CHECK(JsonValue(*audio, "format", "") == "mp3");
  CHECK(!JsonValue(*audio, "data", "").empty());
  // An MP4 audio container is m4a on the wire, not mp3.
  request = prepare(memo, "memo");
  CHECK(JsonValue(request[0]["content"][1]["input_audio"], "format", "") ==
        "m4a");
  // Responses and Anthropic Messages have no speech or video parts, so a
  // route there degrades to file paths instead of silently dropping them.
  for (WireApi dialect : {WireApi::kResponses, WireApi::kAnthropicMessages}) {
    ProviderCapabilities other = CapabilitiesForRoute(dialect, {});
    CHECK(!other.audio_input && !other.video_input);
    other.SetInputModalities(json::array({"text", "audio", "video"}), dialect);
    CHECK(!other.audio_input && !other.video_input);
  }
  request = prepare(clip, "seeing");
  CHECK(error.empty());
  CHECK(deliveries[0]["delivery"] == "Video");
  CHECK(JsonValue(request[0]["content"][1]["video_url"], "url", "")
            .starts_with("data:video/mp4;base64,"));
  capabilities.SetInputModalities(json::array({"text"}),
                                  WireApi::kChatCompletions);
  request = prepare(song, "deaf");
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

TEST_CASE(TestWriteFileCopiesAttachments) {
  namespace fs = std::filesystem;
  fs::path root = fs::temp_directory_path() / "uagent-copy-attachments";
  fs::remove_all(root);
  fs::create_directories(root / "history.jsonl.assets");
  fs::create_directories(root / "work");
  const std::string png("\x89PNG\r\n\x1a\n\0bytes", 13);
  CHECK(ToolWriteFile((root / "history.jsonl.assets/logo.png").string(), png)
            .Ok());
  CHECK(ToolWriteFile((root / "elsewhere.png").string(), png).Ok());
  ActivitySupervisor processes;
  std::vector<Tool> tools = BuiltinTools(processes, root / "work");
  CopyAttachments(tools, (root / "history.jsonl").string());
  const Tool* write = FindTool(tools, "write_file");
  CHECK(write && write->parameters["properties"].contains("from"));
  if (!write) return;
  ToolContext context;
  const std::string target = (root / "work/logo.png").string();
  CHECK(write
            ->run({{"path", target},
                   {"from", (root / "history.jsonl.assets/logo.png").string()}},
                  context)
            .Ok());
  CHECK(ReadFile(target, 1024).value_or("") == png);
  CHECK(!write
             ->run({{"path", (root / "work/other.png").string()},
                    {"from", (root / "elsewhere.png").string()}},
                   context)
             .Ok());
  CHECK(
      !write->run({{"path", (root / "work/none.txt").string()}}, context).Ok());
  CHECK(write
            ->run({{"path", (root / "work/note.txt").string()},
                   {"content", "plain"}},
                  context)
            .Ok());
  // An empty `from` is none: the content is written, and without content
  // the call is refused as one that names neither.
  const json filled = {{"path", (root / "work/filled.txt").string()},
                       {"content", "plain"},
                       {"from", ""}};
  CHECK(write->run(filled, context).Ok());
  CHECK(ReadFile((root / "work/filled.txt").string(), 1024).value_or("") ==
        "plain");
  CHECK(!write->approval_preview(filled).starts_with("a copy of"));
  CHECK(!write
             ->run({{"path", (root / "work/empty.txt").string()}, {"from", ""}},
                   context)
             .Ok());
  fs::remove_all(root);
}

}  // namespace uagent
