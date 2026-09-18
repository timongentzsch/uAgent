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
// Container sniffing for speech and video (ID3/RIFF/FLAC/OggS, BMFF video
// brands). Empty when inconclusive; the declared extension then decides.
std::string AudioMime(std::string_view bytes);
std::string VideoMime(std::string_view bytes);

std::string ImageDetail();

bool InspectAttachment(std::string path, Attachment& out, std::string& error);

// Route capability is passed explicitly; attachment helpers do not maintain a
// second process-global copy of negotiated provider state.
const char* ModelImageInputInstruction(bool image_input_available,
                                       bool image_fallback_available);
// Same posture for speech and video: silence when the route takes the kind,
// otherwise tell the model attachments degrade to file paths.
const char* ModelAudioInputInstruction(bool audio_input_available);
const char* ModelVideoInputInstruction(bool video_input_available);
// Short wire format for input_audio ("wav", "mp3", ...) from the MIME type.
std::string AudioFormat(const std::string& mime);
bool IsAudioMime(const std::string& mime);
bool IsVideoMime(const std::string& mime);

class AttachmentQueue {
 public:
  ToolResult Add(const std::string& path, std::string source_call_id = {});
  std::vector<Attachment> Take();

 private:
  std::mutex mutex_;
  std::vector<Attachment> pending_;
};

AttachmentQueue& Attachments();

std::string Base64File(const Attachment& attachment, uintmax_t max_bytes,
                       std::string& error, const std::string& prefix = "");

bool Base64Decode(std::string_view input, std::string& output,
                  size_t max_bytes);

// The text part always names every path, so a route that cannot take a kind
// of attachment still learns where it is and can reach it with other tools.
json AttachmentContent(const std::string& prompt,
                       const std::vector<Attachment>& attachments,
                       std::string& error);

// Resolved attachment records ({path,name,mime,bytes,image,id}) back into an
// Attachment. Display-only fields stay out of the model struct.
Attachment AttachmentFromJson(const json& item);

// Compose a steered user message: attachment content when files ride along,
// plain prompt text otherwise. The bool selects the message kind
// (kAttachment vs kUser); error carries compose failures for logging.
std::pair<json, bool> ComposeSteeredContent(const std::string& input,
                                            const json& attachments,
                                            std::string& error);

// Resolve retained references into a request projection; originals stay in
// history.
bool PrepareAttachments(json& messages,
                        const ProviderCapabilities& capabilities,
                        bool vision_fallback, const std::string& route,
                        std::string& error, json* deliveries = nullptr);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MEDIA_ATTACHMENTS_H_
