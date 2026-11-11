// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "include/app/workspace_files.h"
#include "include/core/fs.h"
#include "include/core/limits.h"
#include "include/core/skills.h"
#include "include/core/strings.h"
#include "include/media/attachments.h"
#include "src/web/master_internal.h"

namespace uagent::web::master {
namespace fs = std::filesystem;
using workspace_files::Resolve;
namespace {

// The page may reach its own folder's address and Google Fonts, nothing else:
// no connection, image or form can carry the address, and so its token, away.
// The address is written out rather than as 'self', which engines resolve
// differently in an opaque origin. WebAssembly is allowed for the sheet
// engine, which a sheet page loads from that address.
std::string PreviewPolicy(const std::string& origin, const std::string& base) {
  const std::string self = origin + base;
  return "sandbox allow-scripts allow-forms allow-modals; default-src " + self +
         "; script-src " + self +
         " 'unsafe-inline' 'wasm-unsafe-eval'; style-src " + self +
         " 'unsafe-inline' https://fonts.googleapis.com; font-src " + self +
         " https://fonts.gstatic.com data:; img-src " + self +
         " data: blob:; media-src " + self +
         " data: blob:; form-action 'none'; base-uri 'none'; "
         "frame-ancestors " +
         origin;
}

// What a sheet page loads under `~sheet/` of its own address: the engine and
// the code that draws a sheet, installed beside the skills (or in a test's
// own web build), the same for every folder. A folder of that name at the
// top of a workspace is not reachable from a page.
constexpr std::string_view kSheetPrefix = "~sheet/";

std::filesystem::path SheetDir() {
  if (const char* dist = getenv("UAGENT_INTERNAL_WEB_DIST")) {
    return std::filesystem::path(dist) / "share/sheet";
  }
  return InstalledSkillsPath().parent_path() / "sheet";
}

std::string SheetMime(const std::filesystem::path& file) {
  const std::string ext = file.extension().string();
  return ext == ".js"     ? "text/javascript"
         : ext == ".css"  ? "text/css"
         : ext == ".wasm" ? "application/wasm"
                          : "";
}

// A whole-number query parameter, kept within [low, high].
int ParamNumber(const Request& request, const char* name, int fallback, int low,
                int high) {
  const std::string text = request.get_param_value(name);
  int value = fallback;
  std::from_chars(text.data(), text.data() + text.size(), value);
  return std::clamp(value, low, high);
}

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

// A files operation on a conversation's folder, where the folder is.
json Master::WorkspaceFiles(const WorkspaceIdentity& workspace,
                            const std::string& session, json operation) {
  if (workspace.Remote()) {
    operation["op"] = "files";
    return RemoteCall(host_.Execution(session), std::move(operation));
  }
  const auto root = CanonicalDirectory(workspace.path);
  return root ? workspace_files::Request(*root, operation)
              : json{{"error", "not found"}, {"status", 404}};
}

void Master::Files(const Request& request, Response& response) {
  const std::string id = request.matches[1].str();
  const WorkspaceIdentity workspace = host_.WorkspaceOf(id);
  if (workspace.path.empty()) {
    Error(response, "unknown session", 404);
    return;
  }
  int modes = 0;
  std::string mode, path;
  for (const char* name :
       {"list", "read", "raw", "find", "changes", "diff", "png", "pdf"}) {
    if (!request.has_param(name)) continue;
    ++modes;
    mode = name;
    path = request.get_param_value(name);
  }
  if (modes != 1) {
    Error(response,
          "name one of list, read, raw, find, changes, diff, png or pdf");
    return;
  }
  if (mode == "png" || mode == "pdf") {
    Picture(request, response, workspace);
    return;
  }
  std::string bytes, name;
  if (workspace.Remote() || mode != "raw") {
    const json result =
        WorkspaceFiles(workspace, id, {{"mode", mode}, {"path", path}});
    if (result.contains("error")) {
      Error(response, JsonValue(result, "error", "File unavailable"),
            JsonValue(result, "status", workspace.Remote() ? 503 : 404));
      return;
    }
    if (mode != "raw") {
      Reply(response, result);
      return;
    }
    if (!HexDecode(JsonValue(result, "data", ""), bytes, kUploadBytes)) {
      Error(response, "Invalid remote file", 502);
      return;
    }
    name = JsonValue(result, "name", "file");
  } else {
    // Local downloads keep their direct byte path, without RPC encoding.
    const auto root = CanonicalDirectory(workspace.path);
    const auto target = root ? Resolve(*root, path) : std::nullopt;
    std::string error;
    if (!target ||
        !ReadRegularFile(target->string(), kUploadBytes, bytes, error)) {
      Error(response, "not found", 404);
      return;
    }
    name = target->filename().string();
  }
  // Only an image opens in the browser; HTML and other files download.
  std::string mime = RasterMime(bytes);
  if (mime.empty()) mime = SvgMime(bytes);
  // Shown here, never attached: models do not read AVIF.
  if (mime.empty() &&
      std::string_view(bytes).substr(0, 12).ends_with("ftypavif")) {
    mime = "image/avif";
  }
  const bool image = !mime.empty();
  ServeFile(response, std::move(bytes), image ? mime : kOctetStreamMime, name,
            image && !request.has_param("download"));
}

// A person's edit of a text file in the conversation's folder, saved only if
// the file is still as they read it, or with `moves` their reordering of a
// folder's pages. Making files is the agent's.
void Master::FileWrite(const Request& request, Response& response) {
  const std::string id = request.matches[1].str();
  const WorkspaceIdentity workspace = host_.WorkspaceOf(id);
  const json body = request.body.size() > kFileViewBytes + KiB(64)
                        ? json()
                        : json::parse(request.body, nullptr, false);
  if (workspace.path.empty() || !body.is_object()) {
    Error(response,
          workspace.path.empty() ? "unknown session" : "invalid request", 400);
    return;
  }
  // Only these fields travel: a browser never names a request's identity.
  const std::string path = JsonValue(body, "path", "");
  const json result = WorkspaceFiles(
      workspace, id,
      body.contains("moves")
          ? json{{"mode", "move"}, {"path", path}, {"moves", body["moves"]}}
          : json{{"mode", "write"},
                 {"path", path},
                 {"text", JsonValue(body, "text", "")},
                 {"revision", JsonValue(body, "revision", "")}});
  if (result.contains("error")) {
    Error(response, JsonValue(result, "error", "cannot save"),
          JsonValue(result, "status", 503));
  } else {
    Reply(response, result);
  }
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
  const WorkspaceIdentity workspace = host_.WorkspaceOf(session);
  const std::string* origin = ExpectedOrigin(request);
  if (workspace.path.empty() || !origin ||
      relative.find('\0') != std::string::npos) {
    Error(response, "not found", 404);
    return;
  }
  if (relative.starts_with(kSheetPrefix)) {
    const std::filesystem::path file =
        SheetDir() / relative.substr(kSheetPrefix.size());
    const std::string mime = SheetMime(file);
    std::string bytes, error;
    if (mime.empty() || relative.find("..") != std::string::npos ||
        !ReadRegularFile(file.string(), kUploadBytes, bytes, error)) {
      Error(response, "not found", 404);
      return;
    }
    // Loaded by a page in an opaque origin, and the same until the host is
    // updated.
    for (const auto& [name, value] :
         {std::pair{"Cross-Origin-Resource-Policy", "cross-origin"},
          {"Access-Control-Allow-Origin", "*"},
          {"Cache-Control", "private, max-age=3600"}}) {
      response.headers.erase(name);
      response.set_header(name, value);
    }
    response.set_content(std::move(bytes), mime);
    return;
  }
  const bool deck = request.has_param("deck");
  const json result =
      WorkspaceFiles(workspace, session,
                     {{"mode", deck ? "deck" : "preview"},
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
  const std::string mime = JsonValue(result, "mime", "text/html");
  if (deck) {
    bytes = JsonValue(result, "text", "");
  } else if (mime == "text/html") {
    bytes = WithScript(std::move(bytes), design_helper_);
  }
  response.headers.erase("Content-Security-Policy");
  response.set_header(
      "Content-Security-Policy",
      deck ? "default-src " + *origin + "/preview/" + token +
                 "/; script-src 'none'; style-src 'unsafe-inline'; "
                 "form-action 'none'; frame-ancestors 'none'"
           : PreviewPolicy(*origin, "/preview/" + token + "/"));
  if (!deck) {
    // The page's own origin is opaque: its loads are cross-origin.
    response.headers.erase("Cross-Origin-Resource-Policy");
    response.set_header("Cross-Origin-Resource-Policy", "cross-origin");
    response.set_header("Access-Control-Allow-Origin", "*");
  }
  response.set_content(std::move(bytes),
                       mime == "text/html" ? "text/html; charset=utf-8" : mime);
}

// A page drawn by the agent's Chrome in a throwaway context of its own, on
// this host's loopback: a PNG at the artboard's size, or a PDF on the page's
// paper. A folder's PDF is its deck, every slide a sheet.
void Master::Picture(const Request& request, Response& response,
                     const WorkspaceIdentity& workspace) {
  const bool pdf = request.has_param("pdf");
  const std::string path = request.get_param_value(pdf ? "pdf" : "png");
  std::optional<fs::path> target;
  bool directory = false, regular = false;
  const std::string session = request.matches[1].str();
  if (workspace.Remote()) {
    const json stat =
        WorkspaceFiles(workspace, session, {{"mode", "stat"}, {"path", path}});
    if (stat.contains("error")) {
      Error(response, JsonValue(stat, "error", "not found"),
            JsonValue(stat, "status", 503));
      return;
    }
    directory = JsonValue(stat, "directory", false);
    regular = JsonValue(stat, "regular", false);
    target = fs::path(workspace.path) / path;
  } else if (const auto root = CanonicalDirectory(workspace.path)) {
    target = Resolve(*root, path);
    directory = target && fs::is_directory(*target);
    regular = target && fs::is_regular_file(*target);
  }
  const std::string base = PreviewBase(session);
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
