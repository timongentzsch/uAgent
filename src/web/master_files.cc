// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "include/agent/path_policy.h"
#include "include/app/execution.h"
#include "include/app/launch.h"
#include "include/app/session_host.h"
#include "include/app/workspace_files.h"
#include "include/core/capture.h"
#include "include/core/env.h"
#include "include/core/file_watch.h"
#include "include/core/fs.h"
#include "include/core/library.h"
#include "include/core/limits.h"
#include "include/core/strings.h"
#include "include/media/attachments.h"
#include "include/tools/files.h"
#include "src/web/master_internal.h"

namespace uagent::web::master {
namespace fs = std::filesystem;
using workspace_files::Change;
using workspace_files::Changes;
using workspace_files::DeckPage;
using workspace_files::Find;
using workspace_files::List;
using workspace_files::PreviewMime;
using workspace_files::ReadChanges;
using workspace_files::ReadDiff;
using workspace_files::Resolve;
namespace {

// The page may reach its own folder's address and Google Fonts, nothing else:
// no connection, image or form can carry the address, and so its token, away.
// The address is written out rather than as 'self', which engines resolve
// differently in an opaque origin.
std::string PreviewPolicy(const std::string& origin, const std::string& base) {
  const std::string self = origin + base;
  return "sandbox allow-scripts allow-forms allow-modals; default-src " + self +
         "; script-src " + self + " 'unsafe-inline'; style-src " + self +
         " 'unsafe-inline' https://fonts.googleapis.com; font-src " + self +
         " https://fonts.gstatic.com data:; img-src " + self +
         " data: blob:; media-src " + self +
         " data: blob:; form-action 'none'; base-uri 'none'; "
         "frame-ancestors " +
         origin;
}

// A whole-number query parameter, kept within [low, high].
int ParamNumber(const Request& request, const char* name, int fallback, int low,
                int high) {
  const std::string text = request.get_param_value(name);
  int value = fallback;
  std::from_chars(text.data(), text.data() + text.size(), value);
  return std::clamp(value, low, high);
}

// A deck's slides, one sheet each, for printing to one PDF. It is made here
// from the folder's file names alone, so it runs unsandboxed: each slide
// keeps its own sandbox in its frame, and its frame-ancestors let this page
// hold it.
// `script` inlined first in the page: after its doctype, so the page keeps
// standards mode, and the parser puts it first in <head>.
std::string WithScript(std::string html, const std::string& script) {
  size_t at = html.starts_with("\xEF\xBB\xBF") ? 3 : 0;
  if (AsciiLower(html.substr(at, 9)) == "<!doctype") {
    const size_t close = html.find('>', at);
    at = close == std::string::npos ? at : close + 1;
  }
  html.insert(at, "<script>" + script + "</script>");
  return html;
}

}  // namespace

void Master::Files(const Request& request, Response& response) {
  std::string folder;
  {
    std::lock_guard lock(mutex_);
    folder = host_.Folder(request.matches[1].str());
  }
  if (folder.empty()) {
    Error(response, "unknown session", 404);
    return;
  }
  const int modes = request.has_param("list") + request.has_param("read") +
                    request.has_param("raw") + request.has_param("find") +
                    request.has_param("changes") + request.has_param("diff") +
                    request.has_param("png") + request.has_param("pdf");
  if (modes != 1) {
    Error(response,
          "name one of list, read, raw, find, changes, diff, png or pdf");
    return;
  }
  const std::string id = request.matches[1].str();
  if (host_.WorkspaceOf(id).Remote()) {
    const json target = host_.Execution(id);
    if (target.is_null()) {
      Error(response, "Host is disconnected. Choose Reconnect.", 503);
      return;
    }
    execution::Client remote(execution::TargetFromJson(target));
    json operation = {{"op", "files"}};
    for (const char* mode :
         {"list", "read", "raw", "find", "changes", "diff", "png", "pdf"}) {
      if (!request.has_param(mode)) continue;
      if (std::string_view(mode) == "png" || std::string_view(mode) == "pdf") {
        Picture(request, response, fs::path(folder));
        return;
      }
      operation["mode"] = mode;
      operation["path"] = request.get_param_value(mode);
    }
    const json result = remote.Call(operation);
    if (result.contains("error")) {
      Error(response, JsonValue(result, "error", "Remote file unavailable"),
            JsonValue(result, "status", 503));
      return;
    }
    if (JsonValue(operation, "mode", "") == "raw") {
      std::string bytes;
      if (!HexDecode(JsonValue(result, "data", ""), bytes, kUploadBytes)) {
        Error(response, "Invalid remote file", 502);
        return;
      }
      std::string mime = RasterMime(bytes);
      if (mime.empty()) mime = SvgMime(bytes);
      const bool image = !mime.empty();
      ServeFile(response, std::move(bytes), image ? mime : kOctetStreamMime,
                JsonValue(result, "name", "file"),
                image && !request.has_param("download"));
    } else {
      Reply(response, result);
    }
    return;
  }
  std::optional<fs::path> root = CanonicalDirectory(folder);
  if (!root) {
    Error(response, "not found", 404);
    return;
  }
  if (request.has_param("changes") || request.has_param("diff")) {
    const bool listing = request.has_param("changes");
    const Changes changes = ReadChanges(
        *root, listing ? "." : ":(literal)" + request.get_param_value("diff"));
    if (listing) {
      json rows = json::array();
      for (const Change& change : changes.list) {
        rows.push_back(
            {{"path", change.path}, {"state", std::string(1, change.state)}});
      }
      Reply(response, {{"changes", std::move(rows)},
                       {"more", changes.more},
                       {"repository", changes.repository}});
      return;
    }
    const std::optional<json> diff =
        ReadDiff(*root, changes, request.get_param_value("diff"));
    if (!diff) {
      Error(response, "not found", 404);
      return;
    }
    Reply(response, *diff);
    return;
  }
  if (request.has_param("png") || request.has_param("pdf")) {
    Picture(request, response, *root);
    return;
  }
  if (request.has_param("find")) {
    Reply(response, Find(*root, request.get_param_value("find")));
    return;
  }
  const std::string mode = request.has_param("list")   ? "list"
                           : request.has_param("read") ? "read"
                                                       : "raw";
  std::optional<fs::path> target =
      Resolve(*root, request.get_param_value(mode));
  std::optional<json> listing;
  if (target && mode == "list") listing = List(*root, *target);
  if (listing) {
    Reply(response, *listing);
    return;
  }
  // Read a byte past the text shown, so a cut is seen.
  const bool read = mode == "read";
  std::error_code ec;
  const uintmax_t size = target ? fs::file_size(*target, ec) : 0;
  std::string bytes, error;
  if (!target || mode == "list" || ec ||
      !ReadRegularFile(target->string(),
                       read ? kFileViewBytes + 1 : kUploadBytes, bytes, error,
                       read)) {
    Error(response, "not found", 404);
    return;
  }
  if (!read) {
    // Only an image opens in the browser; anything else, HTML included,
    // downloads, so no file in the folder runs on this origin.
    std::string mime = RasterMime(bytes);
    if (mime.empty()) mime = SvgMime(bytes);
    const bool image = !mime.empty();
    ServeFile(response, std::move(bytes), image ? mime : kOctetStreamMime,
              target->filename().string(),
              image && !request.has_param("download"));
    return;
  }
  if (!LikelyTextFile(*target)) {
    Error(response, "not a text file", 415);
    return;
  }
  const bool more = bytes.size() > kFileViewBytes;
  // What a person's edit is saved against: only a file read whole.
  const std::string revision =
      more ? std::string() : DocumentRevision(target->string(), bytes);
  Reply(response, {{"text", Utf8Prefix(std::move(bytes), kFileViewBytes)},
                   {"bytes", size},
                   {"more", more},
                   {"revision", revision}});
}

// A person's edit of a text file in the conversation's folder, saved only if
// the file is still as they read it. Making files is the agent's.
void Master::FileWrite(const Request& request, Response& response) {
  std::string folder;
  {
    std::lock_guard lock(mutex_);
    folder = host_.Folder(request.matches[1].str());
  }
  const json body = request.body.size() > kFileViewBytes + KiB(64)
                        ? json()
                        : json::parse(request.body, nullptr, false);
  const std::string id = request.matches[1].str();
  if (host_.WorkspaceOf(id).Remote()) {
    if (!body.is_object()) {
      Error(response, "invalid request", 400);
      return;
    }
    const json target = host_.Execution(id);
    if (target.is_null()) {
      Error(response, "Host is disconnected. Choose Reconnect.", 503);
      return;
    }
    execution::Client remote(execution::TargetFromJson(target));
    json operation = body;
    operation["op"] = "files";
    operation["mode"] = "write";
    const json result = remote.Call(operation);
    if (result.contains("error")) {
      Error(response, JsonValue(result, "error", "Remote write failed"),
            JsonValue(result, "status", 503));
    } else {
      Reply(response, result);
    }
    return;
  }
  const std::optional<fs::path> root =
      folder.empty() ? std::nullopt : CanonicalDirectory(folder);
  if (!root || !body.is_object()) {
    Error(response, root ? "invalid request" : "unknown session", 400);
    return;
  }
  const std::optional<fs::path> target =
      Resolve(*root, JsonValue(body, "path", ""));
  if (!target || !fs::is_regular_file(*target) || !LikelyTextFile(*target)) {
    Error(response, "not found", 404);
    return;
  }
  const std::string text = JsonValue(body, "text", "");
  if (text.size() > kFileViewBytes) {
    Error(response, "too large to save here", 413);
    return;
  }
  std::string previous, error;
  bool exists = false;
  if (json refused = CheckRevision(body, *target, kFileViewBytes, "file",
                                   previous, exists);
      !refused.is_null()) {
    Error(response, JsonValue(refused, "error", "cannot save"),
          refused.contains("conflict") ? 409 : 500);
    return;
  }
  if (!AtomicWriteFile(target->string(), text, 0644, true, error)) {
    Error(response, error, 500);
    return;
  }
  Reply(response, {{"revision", DocumentRevision(target->string(), text)}});
}

std::string Master::PreviewBase(const std::string& id) {
  if (host_.Folder(id).empty()) return "";
  std::lock_guard lock(previews_mutex_);
  std::string& token = previews_[id];
  if (token.empty()) token = RandomToken();
  if (token.empty()) previews_.erase(id);
  return token.empty() ? "" : "/preview/" + token + "/";
}

void Master::PreviewGrant(const Request& request, Response& response) {
  const std::string base = PreviewBase(request.matches[1].str());
  if (base.empty()) {
    Error(response, "unknown session", 404);
    return;
  }
  Reply(response, {{"base", base}});
}

void Master::Preview(const Request& request, Response& response) {
  const std::string token = request.matches[1].str();
  const std::string relative = request.matches[2].str();
  std::string session;
  {
    std::lock_guard lock(previews_mutex_);
    for (const auto& [id, granted] : previews_) {
      if (EqualSecret(token, granted)) session = id;
    }
  }
  const std::string folder = session.empty() ? "" : host_.Folder(session);
  if (!session.empty() && host_.WorkspaceOf(session).Remote()) {
    const json target = host_.Execution(session);
    const std::string* origin = ExpectedOrigin(request);
    if (target.is_null() || !origin) {
      Error(response, "Host is disconnected", 503);
      return;
    }
    execution::Client remote(execution::TargetFromJson(target));
    const bool deck = request.has_param("deck");
    const json result =
        remote.Call({{"op", "files"},
                     {"mode", deck ? "deck" : "preview"},
                     {"path", relative},
                     {"width", ParamNumber(request, "w", 1920, 1, 8000)},
                     {"height", ParamNumber(request, "h", 1080, 1, 8000)}});
    std::string bytes;
    if (result.contains("error") ||
        (!deck &&
         !HexDecode(JsonValue(result, "data", ""), bytes, kUploadBytes))) {
      Error(response, JsonValue(result, "error", "not found"),
            JsonValue(result, "status", 404));
      return;
    }
    std::string mime = JsonValue(result, "mime", "text/html");
    if (deck) {
      bytes = JsonValue(result, "text", "");
    } else if (mime == "text/html") {
      bytes = WithScript(std::move(bytes), design_helper_);
    }
    response.headers.erase("Content-Security-Policy");
    response.headers.erase("Cross-Origin-Resource-Policy");
    response.set_header(
        "Content-Security-Policy",
        deck ? "default-src " + *origin + "/preview/" + token +
                   "/; style-src 'unsafe-inline'; frame-ancestors 'none'"
             : PreviewPolicy(*origin, "/preview/" + token + "/"));
    response.set_header("Cross-Origin-Resource-Policy", "cross-origin");
    response.set_header("Access-Control-Allow-Origin", "*");
    response.set_content(std::move(bytes), mime == "text/html"
                                               ? "text/html; charset=utf-8"
                                               : mime);
    return;
  }
  std::optional<fs::path> root =
      folder.empty() ? std::nullopt : CanonicalDirectory(folder);
  std::optional<fs::path> target =
      !root || relative.find('\0') != std::string::npos
          ? std::nullopt
          : Resolve(*root, relative);
  const std::string* origin = ExpectedOrigin(request);
  if (target && origin && request.has_param("deck") &&
      fs::is_directory(*target)) {
    response.headers.erase("Content-Security-Policy");
    response.set_header("Content-Security-Policy",
                        "default-src " + *origin + "/preview/" + token +
                            "/; style-src 'unsafe-inline'; frame-ancestors "
                            "'none'");
    response.set_content(
        DeckPage(*target, ParamNumber(request, "w", 1920, 1, 8000),
                 ParamNumber(request, "h", 1080, 1, 8000)),
        "text/html; charset=utf-8");
    return;
  }
  const std::string mime = target ? PreviewMime(*target) : std::string();
  std::string bytes, error;
  if (!target || mime.empty() || !origin ||
      !ReadRegularFile(target->string(), kUploadBytes, bytes, error)) {
    Error(response, "not found", 404);
    return;
  }
  if (mime == "text/html") bytes = WithScript(std::move(bytes), design_helper_);
  response.headers.erase("Content-Security-Policy");
  response.headers.erase("Cross-Origin-Resource-Policy");
  response.set_header("Content-Security-Policy",
                      PreviewPolicy(*origin, "/preview/" + token + "/"));
  // The page's own origin is opaque: its loads are cross-origin.
  response.set_header("Cross-Origin-Resource-Policy", "cross-origin");
  response.set_header("Access-Control-Allow-Origin", "*");
  response.set_content(std::move(bytes),
                       mime == "text/html" ? "text/html; charset=utf-8" : mime);
}

// A page drawn by the agent's Chrome in a throwaway context of its own, on
// this host's loopback: a PNG at the artboard's size, or a PDF on the page's
// paper. A folder's PDF is its deck, every slide a sheet.
void Master::Picture(const Request& request, Response& response,
                     const fs::path& root) {
  const bool pdf = request.has_param("pdf");
  const std::string path = request.get_param_value(pdf ? "pdf" : "png");
  std::optional<fs::path> target;
  bool directory = false, regular = false;
  const std::string session = request.matches[1].str();
  if (host_.WorkspaceOf(session).Remote()) {
    const json value = host_.Execution(session);
    if (!value.is_null()) {
      execution::Client remote(execution::TargetFromJson(value));
      const json stat =
          remote.Call({{"op", "files"}, {"mode", "stat"}, {"path", path}});
      directory = JsonValue(stat, "directory", false);
      regular = JsonValue(stat, "regular", false);
      if (directory || regular) target = root / fs::path(path);
    }
  } else {
    target = Resolve(root, path);
    directory = target && fs::is_directory(*target);
    regular = target && fs::is_regular_file(*target);
  }
  const std::string base = PreviewBase(request.matches[1].str());
  const bool deck = pdf && directory;
  if (!target || !(deck || regular) || base.empty()) {
    Error(response, "not found", 404);
    return;
  }
  if (browser::DataDirectory().empty()) {
    Error(response, "the agent's browser is not set up on this host", 503);
    return;
  }
  const int width = ParamNumber(request, "w", deck ? 1920 : 1440, 1, 8000);
  const int height = ParamNumber(request, "h", deck ? 1080 : 900, 1, 8000);
  std::string url = local_ + base + PercentEncode(path, "/");
  if (deck) {
    url += (url.ends_with("/") ? "?deck&w=" : "/?deck&w=") +
           std::to_string(width) + "&h=" + std::to_string(height);
  } else if (pdf) {
    url += "#paper";
  }
  const json result =
      browser::Request({{"op", "capture"},
                        {"instance_id", "0"},
                        {"url", url},
                        {"width", width},
                        {"height", height},
                        {"scale", ParamNumber(request, "scale", 1, 1, 2)},
                        {"pdf", pdf}},
                       browser::kActionMs);
  const char* kind = pdf ? "pdf" : "png";
  std::string bytes;
  if (!result.contains(kind) ||
      !Base64Decode(JsonValue(result, kind, ""), bytes, kUploadBytes)) {
    Error(response, JsonValue(result, "error", "the browser drew nothing"),
          502);
    return;
  }
  ServeFile(response, std::move(bytes), pdf ? "application/pdf" : "image/png",
            (deck ? target->filename() : target->stem()).string() + "." + kind,
            !request.has_param("download"));
}

}  // namespace uagent::web::master
