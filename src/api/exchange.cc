// Copyright 2026 Timon Gentzsch
#include "include/api/exchange.h"

#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "include/core/debug.h"
#include "include/core/events.h"
#include "include/core/fs.h"
#include "include/core/strings.h"

namespace uagent {
namespace {
std::string CaptureUrl(std::string url) {
  url = RedactedUrl(std::move(url));
  auto query = url.find_first_of("?#");
  if (query != std::string::npos) {
    url.replace(query, std::string::npos, "?[redacted]");
  }
  return url;
}
std::string PrivateBody(std::string_view body) {
  CreatePrivateDirectories(UagentDir(kArtifactsDir));
  std::string path;
  Fd file(CreateTempFile(UagentDir(kArtifactsDir) + "/http-XXXXXX", path));
  if (file && WriteFully(file.Get(), body)) return path;
  if (file) unlink(path.c_str());
  return {};
}
}  // namespace
HttpExchange::HttpExchange(bool enabled, std::string_view request,
                           json metadata)
    : metadata_(std::move(metadata)) {
  if (!enabled) return;
  metadata_["url"] = CaptureUrl(JsonValue(metadata_, "url", ""));
  std::string path = PrivateBody(request);
  if (path.empty()) return;
  metadata_["request_path"] = path;
  metadata_["id"] = std::filesystem::path(path).filename().string();
  metadata_["time"] = UtcStamp();
  metadata_["request_headers"] = "";
  metadata_["response_headers"] = "";
  metadata_["state"] = "receiving";
  std::string response;
  response_.Reset(
      CreateTempFile(UagentDir(kArtifactsDir) + "/http-XXXXXX", response));
  metadata_["response_path"] = response;
  metadata_["complete"] = static_cast<bool>(response_);
  Publish();
}
void HttpExchange::Headers(std::string_view bytes, bool outgoing) {
  if (!Enabled()) return;
  std::string clean;
  while (!bytes.empty()) {
    size_t end = bytes.find('\n');
    std::string_view line = bytes.substr(0, end);
    std::string lower = AsciiLower(std::string(line));
    size_t colon = line.find(':');
    std::string key = lower.substr(0, colon);
    std::replace(key.begin(), key.end(), '-', '_');
    bool secret =
        (colon != std::string_view::npos && CredentialLikeKey(key)) ||
        lower.starts_with("authorization:") ||
        lower.starts_with("proxy-authorization:") ||
        lower.starts_with("cookie:") || lower.starts_with("set-cookie:") ||
        lower.starts_with("x-api-key:") || lower.starts_with("api-key:");
    if (outgoing && line.starts_with("POST ")) {
      size_t end_target = line.find(' ', 5);
      if (end_target != std::string_view::npos) {
        clean += "POST " +
                 CaptureUrl(std::string(line.substr(5, end_target - 5))) +
                 std::string(line.substr(end_target)) + '\n';
        if (end == std::string_view::npos) break;
        bytes.remove_prefix(end + 1);
        continue;
      }
    }
    clean += secret ? std::string(line.substr(0, colon + 1)) + " [redacted]\r"
                    : std::string(line);
    clean += '\n';
    if (end == std::string_view::npos) break;
    bytes.remove_prefix(end + 1);
  }
  auto& value = metadata_[outgoing ? "request_headers" : "response_headers"];
  std::string prior = value.get<std::string>();
  if (prior.size() + clean.size() <= size_t{16} * 1024) {
    value = prior + clean;
  } else {
    metadata_["headers_complete"] = false;
  }
}
void HttpExchange::Body(std::string_view bytes) {
  if (!Enabled()) return;
  constexpr size_t kBodyLimit = size_t{64} * 1024 * 1024;
  if (bytes.size() > kBodyLimit - bytes_ ||
      !WriteFully(response_.Get(), bytes)) {
    metadata_["complete"] = false;
    response_.Reset();
    return;
  }
  bytes_ += bytes.size();
}
void HttpExchange::Publish() {
  if (metadata_.contains("id")) Emit(Event{EventId::kHttpExchange, metadata_});
}
json HttpExchange::Finish(int64_t status, bool interrupted,
                          const std::string& error) {
  if (!metadata_.contains("id")) return nullptr;
  response_.Reset();
  metadata_["status"] = status;
  metadata_["state"] = interrupted     ? "interrupted"
                       : error.empty() ? "complete"
                                       : "failed";
  metadata_["error"] = error;
  metadata_["bytes"] = bytes_;
  Publish();
  return metadata_;
}
json ContextPreview(const json& body) {
  auto path = PrivateBody(JsonDump(body));
  return path.empty()
             ? json{{"error", "cannot retain context preview"}}
             : json{{"id", std::filesystem::path(path).filename().string()},
                    {"request_path", path},
                    {"preview", true},
                    {"state", "not sent"}};
}
}  // namespace uagent
