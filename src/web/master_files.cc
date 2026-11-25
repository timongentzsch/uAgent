// Copyright 2026 Timon Gentzsch

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "include/agent/conversation.h"
#include "include/agent/session_store.h"
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

// The page may reach its own folder's address only, so nothing can carry
// the address, and its token, away. The address is written out: engines
// resolve 'self' differently in an opaque origin. WebAssembly is
// allowed for the sheet engine, and a worker the page makes, under this same
// policy, for the engine to calculate in.
std::string PreviewPolicy(const std::string& origin, const std::string& base) {
  const std::string self = origin + base;
  return "sandbox allow-scripts allow-forms allow-modals; default-src " + self +
         "; script-src " + self +
         " 'unsafe-inline' 'wasm-unsafe-eval'; worker-src blob:; style-src " +
         self + " 'unsafe-inline'; font-src " + self + " data:; img-src " +
         self + " data: blob:; media-src " + self +
         " data: blob:; form-action 'none'; base-uri 'none'; "
         "frame-ancestors " +
         origin;
}

// What a sheet page loads under `~sheet/` of its own address: the engine and
// the code that draws a sheet, installed with the binary or in a test's own
// web build. A folder of that name at the top of a workspace is not
// reachable from a page.
constexpr std::string_view kSheetPrefix = "~sheet/";

std::filesystem::path SheetDir() {
  if (const char* dist = getenv("UAGENT_INTERNAL_WEB_DIST")) {
    return std::filesystem::path(dist) / "share/sheet";
  }
  return InstalledShare("sheet");
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

// After the doctype, so the page keeps standards mode and the parser puts
// the host's script first in <head>, before any slide can report readiness.
std::string WithHead(std::string html, const std::string& markup) {
  size_t at = html.starts_with("\xEF\xBB\xBF") ? 3 : 0;
  if (AsciiLower(html.substr(at, 9)) == "<!doctype") {
    const size_t close = html.find('>', at);
    at = close == std::string::npos ? at : close + 1;
  }
  html.insert(at, markup);
  return html;
}

bool ReadFileMoves(const std::string& path, const json& header, json& moves) {
  if (!JsonValue(header, "has_file_moves", true)) return true;
  const auto loaded = SessionStore::Inspect(path);
  if (!loaded.record) return false;
  moves = Conversation::FileMoves(loaded.record->state.display);
  return true;
}
}  // namespace

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

// A reference reports its original workspace. Opening that workspace still
// goes through the same session and folder authorization as the Library.
void Master::FileOrigin(const Request& request, Response& response) {
  const std::string id = request.matches[1].str();
  const auto current = host_.WorkspaceOf(id);
  if (current.path.empty()) return Error(response, "unknown session", 404);
  const bool at = request.has_param("sequence");
  const bool by_id = request.has_param("origin");
  const bool has_path = request.has_param("path");
  if ((by_id && (at || has_path)) || (!by_id && !at && !has_path)) {
    return Error(response, "name a sequence, origin or path");
  }
  const std::string origin = request.get_param_value("origin");
  const std::string path = request.get_param_value("path");
  uint64_t sequence = 0;
  if (at) {
    const std::string value = request.get_param_value("sequence");
    const char* end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, sequence);
    if (parsed.ec != std::errc() || parsed.ptr != end || !sequence) {
      return Error(response, "invalid reference sequence");
    }
  }
  const auto location = [&](const std::string& cwd) {
    return json{{"cwd", cwd},
                {"host", current.host},
                {"origin", WorkspaceId(cwd, current.host)},
                {"current", cwd == current.path}};
  };
  if (by_id && origin == WorkspaceId(current.path, current.host)) {
    return Reply(response, location(current.path));
  }
  // Moves are local. For new unmoved sessions a small header read suffices.
  json moves = json::array(), header;
  const auto stored = host_.AssetPath(id);
  if (!current.Remote() && !stored.empty() && PathExists(stored)) {
    header = SessionHeader(stored);
    if (JsonValue(header, kSessionHeaderCwd, "") != current.path ||
        !ReadFileMoves(stored, header, moves)) {
      return Error(response, "workspace history changed, try again", 409);
    }
  }
  std::string folder = current.path;
  uint64_t first_move = UINT64_MAX, next_move = UINT64_MAX;
  std::set<std::string> folders{folder};
  for (const auto& move : moves) {
    const std::string from = JsonValue(move, "from", "");
    const auto boundary = JsonValue(move, "sequence", uint64_t{0});
    if (from.empty() || !boundary) continue;
    if (by_id && WorkspaceId(from, current.host) == origin) {
      return Reply(response, location(from));
    }
    folders.insert(from);
    first_move = std::min(first_move, boundary);
    if (at && sequence < boundary && boundary < next_move) {
      next_move = boundary;
      folder = from;
    }
  }
  if (at && sequence >= first_move) return Reply(response, location(folder));
  if (at) folders = {folder};
  if (by_id) return Error(response, "unknown file origin", 404);
  if (folders.size() == 1) return Reply(response, location(folder));
  if (at && path.empty()) {
    return Error(response, "original workspace is uncertain", 409);
  }
  // Old shared URLs contain only a relative path. Resolve an exact, unique
  // match; a same-named file in two workspaces needs an explicit choice.
  json matches = json::array();
  for (const auto& candidate : folders) {
    const auto root = CanonicalDirectory(candidate);
    if (!root || path.empty()) continue;
    const fs::path written(path);
    const auto relative =
        written.is_absolute() ? written.lexically_relative(*root) : written;
    if (relative.empty()) continue;
    const auto file = Resolve(*root, relative.string());
    std::error_code error;
    if (!file || !fs::exists(*file, error)) continue;
    matches.push_back(location(candidate));
  }
  if (matches.empty()) return Error(response, "file no longer found", 404);
  if (matches.size() == 1) return Reply(response, matches.front());
  std::string message =
      "File exists in multiple workspaces. Open it from Files in one of these "
      "folders:";
  for (const auto& match : matches) {
    message += "\n" + JsonValue(match, "cwd", "");
  }
  Reply(response, {{"error", message}, {"choices", std::move(matches)}}, 409);
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
  for (const char* name : {"list", "stamps", "read", "raw", "find", "changes",
                           "diff", "png", "pdf"}) {
    if (!request.has_param(name)) continue;
    ++modes;
    mode = name;
    path = request.get_param_value(name);
  }
  if (modes != 1) {
    Error(response,
          "name one of list, stamps, read, raw, find, changes, diff, png or "
          "pdf");
    return;
  }
  if (mode == "png" || mode == "pdf") {
    Picture(request, response, workspace);
    return;
  }
  std::string bytes, name;
  if (workspace.Remote() || mode != "raw") {
    json operation = {{"mode", mode}, {"path", path}};
    if (mode == "diff" && request.has_param("scope")) {
      operation["scope"] = request.get_param_value("scope");
    }
    const json result = WorkspaceFiles(workspace, id, std::move(operation));
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

// A person's edit of a text file, saved only if the file is still as they
// read it; with `moves` their reordering of a folder's pages; with `create`
// a sheet they saved from an Excel file.
void Master::FileWrite(const Request& request, Response& response) {
  const std::string id = request.matches[1].str();
  const WorkspaceIdentity workspace = host_.WorkspaceOf(id);
  if (workspace.path.empty()) {
    Error(response, "unknown session", 404);
    return;
  }
  const json body = request.body.size() > kFileViewBytes + KiB(64)
                        ? json()
                        : json::parse(request.body, nullptr, false);
  if (!body.is_object()) {
    Error(response, "invalid request");
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
                 {"revision", JsonValue(body, "revision", "")},
                 {"create", JsonValue(body, "create", false)}});
  if (result.contains("error")) {
    Error(response, JsonValue(result, "error", "cannot save"),
          JsonValue(result, "status", 503));
  } else {
    Reply(response, result);
  }
}

// Publish by atomic replacement. A failed write must leave the old URL revoked.
void Master::PublishWorkerPreview(PreviewToken& grant) {
  if (grant.file.empty() || grant.token.empty()) return;
  std::string error;
  if (!EnsurePrivateDirectory(fs::path(grant.file).parent_path().string()) ||
      !WritePrivateFile(grant.file, local_ + "/preview/" + grant.token + "/",
                        error)) {
    grant.token.clear();
  }
}

std::string Master::PreviewAddress(const std::string& id, bool worker) {
  const auto workspace = host_.WorkspaceOf(id);
  if (workspace.path.empty()) return "";
  const auto key = workspace.Key();
  std::lock_guard lock(previews_mutex_);
  auto& grant = previews_[id];
  const bool attach = worker && grant.file.empty();
  if (attach) grant.file = directory_ + "/worker-previews/" + id;
  if (grant.token.empty() || grant.workspace_key != key) {
    grant.workspace_key = key;
    grant.token = RandomToken();
    PublishWorkerPreview(grant);
  } else if (attach) {
    PublishWorkerPreview(grant);
  }
  if (worker) return grant.file;
  return grant.token.empty() ? "" : "/preview/" + grant.token + "/";
}

void Master::RevokePreviews() {
  std::lock_guard lock(previews_mutex_);
  std::erase_if(previews_, [this](auto& item) {
    const auto& [id, grant] = item;
    if (grant.file.empty()) return true;
    if (host_.HasWorker(id)) return false;
    std::error_code ec;
    fs::remove(grant.file, ec);
    return true;
  });
  for (auto& item : previews_) {
    auto& grant = item.second;
    grant.token = RandomToken();
    PublishWorkerPreview(grant);
  }
}

void Master::ClearWorkerPreviews() const {
  std::error_code ec;
  const fs::path dir = fs::path(directory_) / "worker-previews";
  if (!fs::is_directory(fs::symlink_status(dir, ec))) return;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (session::OpaqueId(entry.path().filename().string())) {
      fs::remove(entry.path(), ec);
    }
  }
  fs::remove(dir, ec);
}

void Master::PreviewGrant(const Request& request, Response& response) {
  const std::string base = PreviewAddress(request.matches[1].str());
  if (base.empty()) {
    Error(response, "unknown session", 404);
    return;
  }
  Reply(response, {{"base", base}});
}

void Master::Preview(const Request& request, Response& response) {
  const std::string token = request.matches[1].str();
  const std::string relative = request.matches[2].str();
  std::string session, workspace_key;
  {
    std::lock_guard lock(previews_mutex_);
    for (const auto& [id, granted] : previews_) {
      if (EqualSecret(token, granted.token)) {
        session = id;
        workspace_key = granted.workspace_key;
        break;
      }
    }
  }
  const WorkspaceIdentity workspace = host_.WorkspaceOf(session);
  const std::string* origin = ExpectedOrigin(request);
  if (workspace.path.empty() || workspace.Key() != workspace_key || !origin ||
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
  const bool review = deck && request.has_param("review");
  const json result =
      WorkspaceFiles(workspace, session,
                     {{"mode", deck ? "deck" : "preview"},
                      {"review", review},
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
    if (!review) {
      bytes = WithHead(std::move(bytes),
                       R"(<script src="/deck-print.js"></script>)");
    }
  } else if (request.has_param("sheet") &&
             (mime == "text/csv" || mime.ends_with("spreadsheetml.sheet"))) {
    // An Excel or CSV file as a sheet: a page that names the file, which the
    // sheet code then reads from this address and draws.
    bytes =
        "<!doctype html><meta charset=\"utf-8\"><meta name=\"sheet\" "
        "content=\"" +
        PercentEncode(std::filesystem::path(relative).filename().string()) +
        "\">";
    mime = "text/html";
  }
  if (!deck && mime == "text/html") {
    bytes =
        WithHead(std::move(bytes), "<script>" + design_helper_ + "</script>");
  }
  response.headers.erase("Content-Security-Policy");
  response.set_header(
      "Content-Security-Policy",
      deck ? "default-src " + *origin + "/preview/" + token + "/; script-src " +
                 (review ? "'none'" : *origin + "/deck-print.js") +
                 "; style-src 'unsafe-inline'; base-uri 'none'; form-action "
                 "'none'; frame-ancestors 'none'"
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

// A page drawn by the agent's Chrome in a throwaway context on this host's
// loopback: a PNG at the artboard's size, or a PDF (a folder's is its deck).
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
  const std::string base = PreviewAddress(session);
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
  } else if (request.has_param("part")) {
    // A document's page or a sheet's tab.
    url += "#" + std::to_string(ParamNumber(request, "part", 1, 1, 2000));
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
  const std::string name =
      deck && path.empty()
          ? "deck"
          : (deck ? target->filename() : target->stem()).string();
  ServeFile(response, std::move(bytes), pdf ? "application/pdf" : "image/png",
            name + "." + kind, !request.has_param("download"));
}

}  // namespace uagent::web::master
