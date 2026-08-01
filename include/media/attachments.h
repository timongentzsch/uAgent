// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_MEDIA_ATTACHMENTS_H_
#define UAGENT_INCLUDE_MEDIA_ATTACHMENTS_H_
// Local multimodal attachments for Chat Completions: images use image_url data
// URLs; PDFs/documents use file_data. No upload API or decoding dependency.

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "include/tools/tool.h"
#include "third_party/json.hpp"

namespace uagent {

using nlohmann::json;

struct Attachment {
  std::string path, name, mime;
  uintmax_t bytes = 0;
  bool image = false;
};

std::string ImageExtension(const std::string& mime);

std::string ImageDetail();

bool InspectAttachment(std::string path, Attachment& out, std::string& error);

// Files the model asked to read. Drained into the next request as user content;
// attachment_content owns the byte limit, the tool's call budget the count.
// Cleared when an endpoint rejects image input, so nothing keeps offering
// pictures to a model that cannot read them. Learned rather than declared: a
// catalogue's modality list can be wrong when a route fans out to other models.
bool ImageInputAvailable();
void SetImageInputAvailable(bool available);

std::string ImageInputError(const Attachment& attachment);

const char* ModelImageInputInstruction();

class AttachmentQueue {
 public:
  ToolResult Add(const std::string& path);
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
                       std::string& error);

// Remove only image parts after an endpoint rejects them. Other attachment
// types remain available on the retry, and the text part retains every path.
size_t StripImageContentParts(json& messages);

}  // namespace uagent

#endif  // UAGENT_INCLUDE_MEDIA_ATTACHMENTS_H_
