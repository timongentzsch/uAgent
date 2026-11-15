// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MEDIA_ATTACHMENTS_H_
#define UAGENT_INCLUDE_MEDIA_ATTACHMENTS_H_
// Retained local attachment references and capability-aware request
// preparation.

#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/api/capabilities.h"
#include "include/core/json.h"
#include "include/core/thread_annotations.h"
#include "include/tools/tool.h"

namespace uagent {

struct Attachment {
  std::string path, name, mime;
  uintmax_t bytes = 0;
  bool image = false;
  std::string source_call_id;
  std::string asset_id = "";
};

std::string ImageExtension(const std::string& mime);
std::string AttachmentMime(const std::string& name);
// Web uploads require a recognized image signature, not a filename claim:
// raster and HEIC magic, or an SVG document root within the scan window.
std::string RasterMime(std::string_view bytes);
std::string SvgMime(std::string_view bytes);
// Container sniffing; empty when inconclusive, and the extension decides.
std::string AudioMime(std::string_view bytes);
std::string VideoMime(std::string_view bytes);

bool InspectAttachment(std::string path, Attachment& out, std::string& error);

// Silence when the route takes the kind; otherwise the model is told that
// attachments degrade to file paths.
const char* ModelImageInputInstruction(bool image_input_available,
                                       bool image_fallback_available);
const char* ModelAudioInputInstruction(bool audio_input_available);
const char* ModelVideoInputInstruction(bool video_input_available);
// Short wire format for input_audio ("wav", "mp3", ...) from the MIME type.
std::string AudioFormat(const std::string& mime);
bool IsAudioMime(const std::string& mime);
bool IsVideoMime(const std::string& mime);

class AttachmentQueue {
 public:
  ToolResult Add(const std::string& path, std::string source_call_id = {});
  ToolResult Add(Attachment attachment);
  std::vector<Attachment> Take();
  std::vector<Attachment> TakeForCall(const std::string& call_id);

 private:
  std::mutex mutex_;
  std::vector<Attachment> pending_ UAGENT_GUARDED_BY(mutex_);
};

AttachmentQueue& Attachments();

std::string Base64File(const Attachment& attachment, uintmax_t max_bytes,
                       std::string& error, const std::string& prefix = "");

bool Base64Decode(std::string_view input, std::string& output,
                  size_t max_bytes);

// Leads files attached during a turn (by a tool, or queued with /attach): they
// share the user role and kind with the person's own messages.
inline constexpr char kAttachedOnRequest[] = "[attached on request]";
// Separates a message's own words from the list of files it names.
inline constexpr char kAttachedList[] = "\n\nAttached:";

// The MIME types spelled outside the extension table in attachments.cc.
inline constexpr char kPngMime[] = "image/png";
inline constexpr char kJpegMime[] = "image/jpeg";
inline constexpr char kGifMime[] = "image/gif";
inline constexpr char kWebpMime[] = "image/webp";
inline constexpr char kOctetStreamMime[] = "application/octet-stream";

// The text part always names every path, so a route that cannot take a kind
// of attachment still learns where it is and can reach it with other tools.
json AttachmentContent(const std::string& prompt,
                       const std::vector<Attachment>& attachments,
                       std::string& error);

Attachment AttachmentFromJson(const json& item);
// What clients show for an attachment: asset id, name, type and size, never
// the private upload path.
json AttachmentDisplayJson(const Attachment& attachment);

// A steered user message; the bool is true when files ride along.
std::pair<json, bool> ComposeSteeredContent(const std::string& input,
                                            const json& attachments,
                                            std::string& error);

// Resolves retained references for one request; history keeps the originals.
bool PrepareAttachments(json& messages,
                        const ProviderCapabilities& capabilities,
                        bool vision_fallback, const std::string& route,
                        std::string& error, json* deliveries = nullptr);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MEDIA_ATTACHMENTS_H_
