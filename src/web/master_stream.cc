// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "include/agent/session_view.h"
#include "include/app/session_host.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/platform.h"
#include "include/core/thread_annotations.h"
#include "include/core/time.h"
#include "include/media/attachments.h"
#include "include/web/protocol.h"
#include "src/web/master_internal.h"

namespace uagent::web::master {

namespace {
constexpr size_t kAssetMetadataBytes = KiB(1);
constexpr auto kReplayWait = std::chrono::seconds(15);
}  // namespace

void Master::Snapshot(const Request& request, Response& response) {
  session::SnapshotQuery query;
  query.has_before = request.has_param("before");
  query.has_detail = request.has_param("detail");
  query.raw = request.has_param("raw");
  query.artifact = request.has_param("artifact");
  query.has_http = request.has_param("http");
  query.before = request.get_param_value("before");
  query.detail = request.get_param_value("detail");
  query.http = request.get_param_value("http");
  query.part = request.get_param_value("part");
  query.offset = request.get_param_value("offset");
  auto result = host_.Snapshot(request.matches[1].str(), query);
  Reply(response, result.value, result.status);
}

void Master::Events(const Request& request, Response& response) {
  UniqueLock lock(mutex_);
  if (sse_count_ >= kWebSseConnections) {
    Error(response, "event connection limit reached", 429);
    return;
  }
  std::string device = DeviceId(request);
  std::string cursor = request.get_header_value("Last-Event-ID");
  if (cursor.empty()) {
    cursor = request.get_param_value("cursor");
  }
  size_t colon = cursor.find(':');
  int64_t parsed = 0;
  bool valid =
      colon != std::string::npos && cursor.substr(0, colon) == epoch_ &&
      ParseInt64(cursor.substr(colon + 1).c_str(), parsed) && parsed >= 0;
  uint64_t next = static_cast<uint64_t>(std::max(int64_t{0}, parsed));
  const uint64_t replay_watermark = host_.Cursor();
  ++sse_count_;
  response.set_header("X-Accel-Buffering", "no");
  response.set_chunked_content_provider(
      "text/event-stream",
      [this, device, next, valid, replay_watermark, ready_sent = false](
          size_t, httplib::DataSink& sink) mutable {
        UniqueLock guard(mutex_);
        auto authorized = [&]() UAGENT_REQUIRES(mutex_) {
          return std::any_of(
              devices_.begin(), devices_.end(), [&](const Device& entry) {
                return entry.id == device && entry.expires > NowMillis();
              });
        };
        if (stopping_ || !authorized()) {
          return false;
        }
        auto replay = host_.ReadReplay(next, valid, replay_watermark,
                                       kWebEventBatchBytes);
        if (replay.reset) {
          guard.unlock();
          std::string reset = "event: resync\ndata: {}\n\n";
          if (!sink.write(reset.data(), reset.size())) {
            return false;
          }
          sink.done();
          return true;
        }
        // Caught up: wait for news, then read again. A reader still
        // replaying sends the batch just read.
        if (ready_sent) {
          guard.unlock();
          host_.WaitForReplay(next, kReplayWait);
          guard.lock();
          if (stopping_ || !authorized()) return false;
          replay = host_.ReadReplay(next, valid, host_.Cursor(),
                                    kWebEventBatchBytes);
          if (replay.reset) {
            valid = false;
            return true;
          }
        }
        std::string batch;
        for (const session::HostReplay& event : replay.events) {
          batch += "id: " + epoch_ + ":" + std::to_string(event.sequence) +
                   "\nevent: update\ndata: " + event.frame + "\n\n";
          next = event.sequence;
          if (batch.size() >= kWebEventBatchBytes) {
            break;
          }
        }
        if (!ready_sent && next >= replay_watermark) {
          batch += "event: ready\ndata: " +
                   JsonDump({{"epoch", epoch_}, {"cursor", replay_watermark}}) +
                   "\n\n";
          ready_sent = true;
        }
        if (batch.empty()) {
          batch = ": heartbeat\n\n";
        }
        guard.unlock();
        return sink.is_writable() && sink.write(batch.data(), batch.size());
      },
      [this](bool) {
        std::lock_guard guard(mutex_);
        --sse_count_;
      });
}

void Master::Upload(const Request& request, Response& response,
                    const httplib::ContentReader& reader) {
  std::string bytes;
  bool read = reader([&](const char* data, size_t length) {
    if (length > kUploadBytes - bytes.size()) return false;
    bytes.append(data, length);
    return true;
  });
  if (!read) {
    Error(response, "upload exceeds file/session limit", 413);
    return;
  }
  auto stored = host_.StoreAsset(request.matches[1].str(), bytes,
                                 request.get_param_value("name"));
  if (!stored.error.empty()) {
    Error(response, stored.error, stored.status);
    return;
  }
  Reply(response, stored.value);
}

void Master::AssetRead(const Request& request, Response& response) {
  std::string path;
  {
    std::lock_guard lock(mutex_);
    path = host_.AssetPath(request.matches[1].str());
    if (path.empty()) {
      Error(response, "unknown session", 404);
      return;
    }
  }
  std::string stem =
      path + kSessionAssetsSuffix + "/" + request.matches[2].str();
  if (!EnsurePrivateDirectory(path + kSessionAssetsSuffix)) {
    Error(response, "asset unavailable", 404);
    return;
  }
  std::string metadata, bytes, error;
  if (!ReadRegularFile(stem + ".json", kAssetMetadataBytes, metadata, error)) {
    Error(response, "asset unavailable", 404);
    return;
  }
  json asset = json::parse(metadata, nullptr, false);
  std::string mime = JsonValue(asset, "mime", ""),
              extension = JsonValue(asset, "extension", ImageExtension(mime));
  if ((extension != ".data" &&
       (extension.empty() || extension != ImageExtension(mime))) ||
      !ReadRegularFile(stem + extension, kUploadBytes, bytes, error)) {
    Error(response, "asset unavailable", 404);
    return;
  }
  bool image = JsonValue(asset, "image", !ImageExtension(mime).empty());
  if (image && RasterMime(bytes) != mime && SvgMime(bytes) != mime) {
    Error(response, "invalid image asset", 415);
    return;
  }
  // Images, PDFs and HTML open in the browser; HTML runs sandboxed in an
  // opaque origin, so its scripts can never read this device's session or
  // call the API. Everything else, or ?download, is saved under its name.
  const bool inline_view =
      !request.has_param("download") &&
      (image || mime == "text/html" || mime == "application/pdf");
  ServeFile(response, std::move(bytes), mime,
            JsonValue(asset, "name", "download"), inline_view);
}

void ServeFile(Response& response, std::string bytes, const std::string& mime,
               const std::string& name, bool inline_view) {
  const bool html = mime == "text/html";
  // Replace the app's policy rather than add to it: browsers enforce every
  // CSP header, so the app's would block the file's own scripts and forbid
  // the conversation from framing its preview. HTML runs its scripts in an
  // opaque origin; anything else, SVG included, may run none at all.
  response.headers.erase("Content-Security-Policy");
  response.set_header("Content-Security-Policy",
                      html ? "sandbox allow-scripts allow-forms allow-popups; "
                             "frame-ancestors 'self'"
                      : mime == "image/svg+xml"
                          ? "sandbox; default-src 'none'; img-src data:; "
                            "style-src 'unsafe-inline'; frame-ancestors 'self'"
                          : "default-src 'none'; frame-ancestors 'self'");
  if (!inline_view) {
    // RFC 6266: an ASCII fallback for old clients, the exact UTF-8 name
    // percent-encoded for the rest.
    std::string fallback, encoded;
    for (const char ch : name) {
      const auto byte = static_cast<unsigned char>(ch);
      fallback += byte < 32 || byte > 126 || ch == '"' || ch == '\\' ? '_' : ch;
      if (std::isalnum(byte) || std::strchr("-._~", ch) != nullptr) {
        encoded += ch;
      } else {
        constexpr char kHex[] = "0123456789ABCDEF";
        encoded += '%';
        encoded += kHex[byte >> 4];
        encoded += kHex[byte & 15];
      }
    }
    response.set_header("Content-Disposition",
                        "attachment; filename=\"" + fallback +
                            "\"; filename*=UTF-8''" + encoded);
  }
  response.set_content(std::move(bytes),
                       inline_view || html ? mime : kOctetStreamMime);
}
}  // namespace uagent::web::master
