// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MEDIA_ATTACHMENTS_H_
#define UAGENT_INCLUDE_MEDIA_ATTACHMENTS_H_
// Local multimodal attachments for Chat Completions: images use image_url data
// URLs; PDFs/documents use file_data. No upload API or decoding dependency.

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "include/core/json.h"
#include "include/tools/tool.h"

namespace uagent {

struct Attachment {
  std::string path, name, mime;
  uintmax_t bytes = 0;
  bool image = false;
};

std::string ImageExtension(const std::string& mime);

std::string ImageDetail();

bool InspectAttachment(std::string path, Attachment& out, std::string& error);

// Route capability is passed explicitly; attachment helpers do not maintain a
// second process-global copy of negotiated provider state.
std::string ImageInputError(const Attachment& attachment,
                            bool image_input_available,
                            bool image_fallback_available);

const char* ModelImageInputInstruction(bool image_input_available,
                                       bool image_fallback_available);

class AttachmentQueue {
 public:
  ToolResult Add(const std::string& path, bool image_input_available = true,
                 bool image_fallback_available = false);
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

json AttachmentContent(const std::string& prompt,
                       const std::vector<Attachment>& attachments,
                       std::string& error, bool image_input_available = true,
                       bool image_fallback_available = false);

// Remove only image parts after an endpoint rejects them. Other attachment
// types remain available on the retry, and the text part retains every path.
size_t StripImageContentParts(json& messages);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MEDIA_ATTACHMENTS_H_
