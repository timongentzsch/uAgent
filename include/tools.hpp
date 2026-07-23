#pragma once
// Tool registry. Each Tool bundles its OpenAI schema and its handler, so
// adding a capability — including MCP-backed tools later — means appending
// to the registry; nothing in the agent loop changes.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "json.hpp"
#include "media.hpp"
#include "util.hpp"

using nlohmann::json;
extern char** environ;

struct ToolContext {
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max();
    long timeout_s = 0;

    ToolContext with_timeout(long seconds) const {
        ToolContext out = *this;
        out.timeout_s = std::max(0L, seconds);
        if (seconds > 0) {
            auto local = std::chrono::steady_clock::now() +
                         std::chrono::seconds(seconds);
            out.deadline = std::min(out.deadline, local);
        }
        return out;
    }

    long remaining_seconds(long configured) const {
        if (deadline == std::chrono::steady_clock::time_point::max())
            return configured;
        long remaining = static_cast<long>(
            std::chrono::duration_cast<std::chrono::seconds>(
                deadline - std::chrono::steady_clock::now())
                .count());
        remaining = std::max(1L, remaining);
        return configured > 0 ? std::min(configured, remaining) : remaining;
    }
};

struct Tool {
    using Run = std::function<std::string(const json&, const ToolContext&)>;
    using Summary = std::function<std::string(const json&)>;
    using Approval = std::function<bool(const json&)>;

    std::string name;
    std::string description;
    json parameters;                                   // JSON-schema for the args
    bool mutating = false;                             // gated behind user approval
    Run run;
    Summary summary;                                   // args -> one-line display
    bool parallel_safe = false;                        // safe beside another tool call
    Approval needs_approval;                           // dynamic policy (e.g. external read)
    std::string provider;                              // owner for live registry refresh
    json output_schema;                               // optional MCP output contract
    long timeout_s = -1;                              // -1 = global default; 0 = turn limit
    long result_chars = -1;                           // -1 = global result cap
    long max_calls_per_turn = -1;                     // -1 = global turn budget
    bool full_terminal_output = false;                // show complete call + result
};

inline Tool make_tool(std::string name, std::string description, json parameters,
                      Tool::Run run) {
    Tool tool;
    tool.name = std::move(name);
    tool.description = std::move(description);
    tool.parameters = std::move(parameters);
    tool.run = std::move(run);
    return tool;
}

inline Tool& add_tool(std::vector<Tool>& tools, Tool tool) {
    tools.push_back(std::move(tool));
    return tools.back();
}

inline std::string tool_description(const Tool& tool) {
    if (tool.max_calls_per_turn < 0) return tool.description;
    return tool.description + " Per-turn call limit: " +
           std::to_string(tool.max_calls_per_turn) + ".";
}

// One execution policy for built-ins, MCP, and future providers. Tool-specific
// schemas may refine the wording, but every registered tool exposes the same
// optional foreground timeout without repeating it at each registration site.
inline json tool_parameters(const Tool& tool, long default_timeout_s = 30) {
    json parameters = tool.parameters;
    if (!parameters.is_object()) parameters = json::object();
    if (!parameters.contains("type")) parameters["type"] = "object";
    if (!parameters.contains("properties") || !parameters["properties"].is_object())
        parameters["properties"] = json::object();
    if (!parameters["properties"].contains("timeout")) {
        long timeout = tool.timeout_s >= 0 ? tool.timeout_s : default_timeout_s;
        parameters["properties"]["timeout"] = {
            {"type", "integer"},
            {"minimum", 0},
            {"description", "foreground seconds (default " +
                                std::to_string(timeout) + "; 0=turn limit)"}};
    }
    return parameters;
}

// One-line display for a call: the tool's own formatter, else `path` (the
// common case), else the raw args. Shared by the approval prompt and the
// call trace so both name the same action the same way.
inline std::string tool_summary(const Tool& t, const json& args) {
    try {
        if (t.summary) return t.summary(args);
        if (args.contains("path") && args["path"].is_string())
            return args["path"].get<std::string>();
    } catch (const std::exception&) {}
    return args.dump();
}

inline const Tool* find_tool(const std::vector<Tool>& tools, const std::string& name) {
    for (auto& t : tools)
        if (t.name == name) return &t;
    return nullptr;
}

// first schema-required argument missing from args, or "" if all present —
// so a call like write_file without `content` errors instead of truncating
inline std::string missing_required(const Tool& t, const json& args) {
    if (t.parameters.contains("required"))
        for (auto& r : t.parameters["required"])
            if (!args.contains(r.get<std::string>())) return r.get<std::string>();
    return "";
}

inline std::string invalid_argument_type(const Tool& tool, const json& args) {
    json parameters = tool_parameters(tool);
    for (const auto& [name, schema] : parameters["properties"].items()) {
        if (!args.contains(name) || !schema.is_object() ||
            !schema.contains("type") || !schema["type"].is_string())
            continue;
        const json& value = args[name];
        const std::string type = schema["type"].get<std::string>();
        bool valid = (type == "string" && value.is_string()) ||
                     (type == "integer" && value.is_number_integer()) ||
                     (type == "number" && value.is_number()) ||
                     (type == "boolean" && value.is_boolean()) ||
                     (type == "object" && value.is_object()) ||
                     (type == "array" && value.is_array()) ||
                     (type == "null" && value.is_null());
        if (!valid) return "`" + name + "` must be " + type;
        if (type == "integer" && schema.contains("minimum") &&
            value.get<long>() < schema["minimum"].get<long>())
            return "`" + name + "` is below its minimum";
    }
    return "";
}

// registry -> the `tools` array for a chat request
inline json tool_schemas(const std::vector<Tool>& tools, long default_timeout_s = 30) {
    json out = json::array();
    for (auto& t : tools)
        out.push_back({{"type", "function"},
                       {"function", {{"name", t.name},
                                     {"description", tool_description(t)},
                                     {"parameters", tool_parameters(t, default_timeout_s)}}}});
    return out;
}

inline json available_tool_schemas(
    const std::vector<Tool>& tools, const json& schemas,
    const std::unordered_map<std::string, long>& counts) {
    json available = json::array();
    for (size_t i = 0; i < tools.size() && i < schemas.size(); ++i) {
        const Tool& tool = tools[i];
        auto count = counts.find(tool.name);
        if (tool.max_calls_per_turn >= 0 && count != counts.end() &&
            count->second >= tool.max_calls_per_turn)
            continue;
        available.push_back(schemas[i]);
    }
    return available;
}

inline std::filesystem::path canonical_access_path(const std::string& path) {
    std::error_code ec;
    std::filesystem::path p = path.empty() ? "." : path;
    auto canonical = std::filesystem::weakly_canonical(p, ec);
    return ec ? std::filesystem::absolute(p, ec) : canonical;
}

inline bool path_within(const std::filesystem::path& path,
                        const std::filesystem::path& root) {
    auto p = path.lexically_normal();
    auto r = root.lexically_normal();
    auto pi = p.begin(), ri = r.begin();
    for (; ri != r.end(); ++ri, ++pi)
        if (pi == p.end() || *pi != *ri) return false;
    return true;
}

// --- built-in tool implementations ------------------------------------------

inline std::string tool_read_file(const std::string& path, long offset, long limit) {
    if (limit == 0) limit = env_long("UAGENT_READ_FILE_LINES", 200);  // 0 = unset
    long max_lines = std::max(1L, env_long("UAGENT_READ_FILE_MAX_LINES", 10000));
    if (limit <= 0 || limit > max_lines) limit = max_lines;
    long max_bytes = std::max(1024L, env_long("UAGENT_READ_FILE_BYTES", 4 * 1024 * 1024));
    if (offset < 1) offset = 1;
    std::ifstream f(path);
    if (!f) return "error: cannot open " + path;
    std::string line, out;
    long total = 0, shown = 0, first = 0, last = 0;
    bool output_limited = false;
    while (shown < limit && std::getline(f, line)) {
        if (abort_requested()) return "error: read cancelled";
        ++total;
        if (total >= offset) {
            if (out.size() + line.size() + 1 >
                static_cast<size_t>(max_bytes)) {
                output_limited = true;
                break;
            }
            out += line;
            out += '\n';
            if (!first) first = total;
            last = total;
            ++shown;
        }
    }
    bool more = output_limited ||
                (shown >= limit && f.peek() != std::char_traits<char>::eof());
    if (more && env_long("UAGENT_READ_FILE_COUNT_TOTAL", 0) != 0) {
        // Optional compatibility mode: exact totals require scanning the tail.
        char blk[1 << 16];
        bool ended_nl = true;
        while (f.read(blk, sizeof blk), f.gcount() > 0) {
            if (abort_requested()) return "error: read cancelled";
            total += (long)std::count(blk, blk + f.gcount(), '\n');
            ended_nl = blk[f.gcount() - 1] == '\n';
        }
        if (!ended_nl) ++total;  // final line without trailing newline
    }
    if (total == 0) return "(empty file)";
    if (offset > total && !more)
        return "error: offset " + std::to_string(offset) + " is beyond EOF (" +
               std::to_string(total) + " lines)";
    std::string header = "[" + path + " lines " + std::to_string(first) + "-" +
                         std::to_string(last);
    if (output_limited)
        header += "; output byte limit reached; more available";
    else if (more && env_long("UAGENT_READ_FILE_COUNT_TOTAL", 0) == 0)
        header += "; more available";
    else
        header += " of " + std::to_string(total);
    return header + "]\n" + out;
}

// Atomic write: temp file in the same directory, then rename — a disk-full or
// crash mid-write can never leave the target truncated. Keeps an existing
// file's permissions.
inline std::string tool_write_file_mode(const std::string& path, const std::string& content,
                                        mode_t create_mode) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    if (ec) return "error: cannot create parent directory for " + path + ": " + ec.message();
    if (fs::is_symlink(p, ec)) {
        fs::path resolved = fs::canonical(p, ec);
        if (ec) return "error: cannot resolve symlink " + path + ": " + ec.message();
        p = std::move(resolved);
    }
    fs::path parent = p.has_parent_path() ? p.parent_path() : fs::path(".");
    std::string pattern = (parent / ("." + p.filename().string() + ".uagent.XXXXXX")).string();
    std::vector<char> temp(pattern.begin(), pattern.end());
    temp.push_back('\0');
    int fd = mkstemp(temp.data());
    if (fd < 0) return "error: cannot create temporary file for " + path + ": " + strerror(errno);
    struct stat st;
    mode_t mode = stat(p.c_str(), &st) == 0 ? st.st_mode & 07777 : create_mode;
    fchmod(fd, mode);
    size_t offset = 0;
    while (offset < content.size()) {
        ssize_t n = write(fd, content.data() + offset, content.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            unlink(temp.data());
            return "error: write to " + path + " failed: " + strerror(errno);
        }
        offset += static_cast<size_t>(n);
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        unlink(temp.data());
        return "error: write to " + path + " failed: " + strerror(errno);
    }
    if (rename(temp.data(), p.c_str()) != 0) {
        std::string error = strerror(errno);
        unlink(temp.data());
        return "error: cannot replace " + path + ": " + error;
    }
    return "wrote " + std::to_string(content.size()) + " bytes to " + path;
}

inline std::string tool_write_file(const std::string& path, const std::string& content) {
    return tool_write_file_mode(path, content, 0644);
}

inline std::string tool_write_private_file(const std::string& path, const std::string& content) {
    return tool_write_file_mode(path, content, 0600);
}

// strip read_file-style "   123\t" prefixes, but only if every non-empty line has one
inline std::string strip_line_numbers(const std::string& s) {
    std::istringstream in(s);
    std::string line, out;
    bool any = false, first = true;
    while (std::getline(in, line)) {
        std::string body = line;
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        size_t d = i;
        while (d < line.size() && isdigit((unsigned char)line[d])) d++;
        if (d > i && d < line.size() && line[d] == '\t') {
            body = line.substr(d + 1);
            any = true;
        } else if (!trim(line).empty()) {
            return s;  // a non-empty line without a prefix: don't strip anything
        }
        if (!first) out += '\n';
        out += body;
        first = false;
    }
    if (!any) return s;
    if (!s.empty() && s.back() == '\n') out += '\n';
    return out;
}

inline long count_occurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    long n = 0;
    for (size_t pos = 0; (pos = hay.find(needle, pos)) != std::string::npos; pos += needle.size())
        ++n;
    return n;
}

inline std::string tool_edit_file(const std::string& path, const std::string& old_s,
                                  const std::string& new_s) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "error: cannot open " + path;
    std::error_code size_ec;
    auto bytes = std::filesystem::file_size(path, size_ec);
    long max_bytes = env_long("UAGENT_EDIT_FILE_BYTES", 10 * 1024 * 1024);
    if (!size_ec && max_bytes > 0 && bytes > static_cast<uintmax_t>(max_bytes))
        return "error: " + path + " is too large to edit atomically (" +
               std::to_string(bytes) + " bytes; limit " + std::to_string(max_bytes) + ")";
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    if (max_bytes > 0 && data.size() > static_cast<size_t>(max_bytes))
        return "error: " + path + " grew beyond the edit limit while reading";
    std::string old_eff = old_s;
    long n = count_occurrences(data, old_eff);
    if (n == 0) {  // retry with read_file line-number prefixes stripped
        std::string stripped = strip_line_numbers(old_s);
        if (stripped != old_s) { old_eff = stripped; n = count_occurrences(data, old_eff); }
    }
    if (n == 0) return "error: `old` not found in " + path;
    if (n > 1)
        return "error: `old` matches " + std::to_string(n) + " times in " + path +
               "; it must match exactly once — add surrounding context";
    if (max_bytes > 0 && new_s.size() > old_eff.size() &&
        new_s.size() - old_eff.size() >
            static_cast<size_t>(max_bytes) - data.size())
        return "error: edited output would exceed the edit byte limit";
    data.replace(data.find(old_eff), old_eff.size(), new_s);
    std::string w = tool_write_file(path, data);  // atomic replace, keeps permissions
    if (w.rfind("error:", 0) == 0) return w;
    return "edited " + path + " (replaced 1 occurrence, " + std::to_string(old_eff.size()) +
           " -> " + std::to_string(new_s.size()) + " chars)";
}

inline std::string tool_list_dir(const std::string& path, long offset = 0, long limit = 0) {
    std::string p = path.empty() ? "." : path;
    if (offset < 0) offset = 0;
    if (limit <= 0) limit = env_long("UAGENT_LIST_DIR_ENTRIES", 1000);
    long scan_cap = std::max(1L, env_long("UAGENT_LIST_DIR_SCAN_ENTRIES", 100000));
    std::error_code ec;
    std::vector<std::string> names;
    for (auto& e : std::filesystem::directory_iterator(p, ec)) {
        if (static_cast<long>(names.size()) >= scan_cap)
            return "error: directory exceeds scan limit (" +
                   std::to_string(scan_cap) + " entries)";
        std::error_code ec2;
        names.push_back(e.path().filename().string() + (e.is_directory(ec2) ? "/" : ""));
    }
    if (ec) return "error: cannot open directory " + p;
    std::sort(names.begin(), names.end());
    if (names.empty()) return "(empty directory)";
    if (offset >= static_cast<long>(names.size()))
        return "error: offset is beyond directory entries (" + std::to_string(names.size()) + ")";
    size_t end = std::min(names.size(), static_cast<size_t>(offset + limit));
    std::string out = "[" + p + " entries " + std::to_string(offset + 1) + "-" +
                      std::to_string(end) + " of " + std::to_string(names.size()) + "]\n";
    for (size_t i = static_cast<size_t>(offset); i < end; ++i) {
        out += names[i];
        out += '\n';
    }
    return out;
}

// --- background jobs ---------------------------------------------------------
// The shell runner writes stdout+stderr to a log file from the start. Commands that
// finish within a short poll window return output directly; longer ones keep
// running in a supervised process group and the model checks on them with
// read_file(log) or wait_background(pid). Memory stays bounded because only
// the log tail is ever read. Normal shutdown terminates every remaining group.
//
// Completed jobs are auto-detected at step boundaries: the agent loop checks
// the ProcessSupervisor before each model call and injects results — no
// background thread needed, no LLM output wasted.

struct BgJob {
    pid_t pid;
    std::string log, cmd;
    bool join_before_final = false;
    bool detached = false;
};

class ProcessSupervisor {
public:
    ProcessSupervisor() = default;
    ~ProcessSupervisor();
    ProcessSupervisor(const ProcessSupervisor&) = delete;
    ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

    const std::vector<BgJob>& jobs() const { return jobs_; }
    std::vector<BgJob>& jobs() { return jobs_; }
    bool pending() const {
        return std::any_of(jobs_.begin(), jobs_.end(),
                           [](const BgJob& job) { return !job.detached; });
    }
    size_t pending_count() const {
        return static_cast<size_t>(std::count_if(
            jobs_.begin(), jobs_.end(),
            [](const BgJob& job) { return !job.detached; }));
    }
    size_t detached_count() const {
        return static_cast<size_t>(std::count_if(
            jobs_.begin(), jobs_.end(),
            [](const BgJob& job) { return job.detached; }));
    }

private:
    std::vector<BgJob> jobs_;
};

struct SideTaskResult {
    long id = 0;
    std::string kind, label, output;
    double duration_ms = 0;
};

// In-process counterpart to ProcessSupervisor: side requests get a short
// foreground grace period, then report back between model steps. Every worker
// owns a cancellation flag and is joined before its session-owned supervisor.
class SideTaskSupervisor {
public:
    using Work = std::function<std::string(const std::atomic<bool>&)>;

    ~SideTaskSupervisor() { cancel_all(); }
    SideTaskSupervisor() = default;
    SideTaskSupervisor(const SideTaskSupervisor&) = delete;
    SideTaskSupervisor& operator=(const SideTaskSupervisor&) = delete;

    long start(std::string kind, std::string label, Work work, long limit,
               bool join_before_final = true) {
        auto job = std::make_shared<Job>();
        std::lock_guard<std::mutex> lock(mutex_);
        if (static_cast<long>(jobs_.size()) >= std::max(1L, limit)) return 0;
        job->id = next_id_++;
        job->kind = std::move(kind);
        job->label = std::move(label);
        job->join_before_final = join_before_final;
        job->cancel = std::make_shared<std::atomic<bool>>(false);
        auto started = std::chrono::steady_clock::now();
        long id = job->id;
        std::string result_kind = job->kind, result_label = job->label;
        auto cancel = job->cancel;
        job->future = std::async(
            std::launch::async,
            [id, kind = std::move(result_kind), label = std::move(result_label),
             cancel, started, work = std::move(work)]() mutable {
                SideTaskResult result;
                result.id = id;
                result.kind = std::move(kind);
                result.label = std::move(label);
                try {
                    result.output = work(*cancel);
                } catch (const std::exception& e) {
                    result.output = "error: side task failed: " + std::string(e.what());
                } catch (...) {
                    result.output = "error: side task failed";
                }
                result.duration_ms = elapsed_ms(started);
                return result;
            });
        jobs_.push_back(std::move(job));
        return id;
    }

    std::optional<SideTaskResult> wait(long id, std::chrono::milliseconds grace) {
        std::shared_ptr<Job> job = find(id);
        if (!job || job->future.wait_for(grace) != std::future_status::ready)
            return std::nullopt;
        return take(id);
    }

    std::vector<SideTaskResult> take_completed() {
        std::vector<std::shared_ptr<Job>> ready;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = jobs_.begin(); it != jobs_.end();) {
                if ((*it)->future.wait_for(std::chrono::milliseconds(0)) ==
                    std::future_status::ready) {
                    ready.push_back(*it);
                    it = jobs_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        std::vector<SideTaskResult> results;
        for (auto& job : ready) results.push_back(job->future.get());
        return results;
    }

    bool wait_for_one(std::chrono::milliseconds timeout) const {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (jobs_.empty()) return false;
                for (const auto& job : jobs_)
                    if (job->future.wait_for(std::chrono::milliseconds(0)) ==
                        std::future_status::ready)
                        return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while (true);
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return jobs_.size();
    }
    bool empty() const { return size() == 0; }
    bool contains(long id) const { return static_cast<bool>(find(id)); }

    size_t joinable() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<size_t>(std::count_if(
            jobs_.begin(), jobs_.end(),
            [](const auto& job) { return job->join_before_final; }));
    }

    size_t cancel_all() {
        std::vector<std::shared_ptr<Job>> jobs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs.swap(jobs_);
        }
        for (auto& job : jobs) job->cancel->store(true);
        for (auto& job : jobs)
            if (job->future.valid()) job->future.wait();
        return jobs.size();
    }

private:
    struct Job {
        long id = 0;
        std::string kind, label;
        bool join_before_final = true;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::future<SideTaskResult> future;
    };

    std::shared_ptr<Job> find(long id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& job : jobs_)
            if (job->id == id) return job;
        return {};
    }

    std::optional<SideTaskResult> take(long id) {
        std::shared_ptr<Job> job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = std::find_if(jobs_.begin(), jobs_.end(),
                                   [&](const auto& candidate) {
                                       return candidate->id == id;
                                   });
            if (it == jobs_.end() ||
                (*it)->future.wait_for(std::chrono::milliseconds(0)) !=
                    std::future_status::ready)
                return std::nullopt;
            job = *it;
            jobs_.erase(it);
        }
        return job->future.get();
    }

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Job>> jobs_;
    long next_id_ = 1;
};

inline void bg_track_signal(pid_t pid, bool add) {
    for (int i = 0; i < BG_MAX; ++i) {
        if (!add && g_bg_pids[i] == pid) {
            g_bg_pids[i] = 0;
            return;
        }
        if (add && g_bg_pids[i] == 0) {
            g_bg_pids[i] = pid;
            return;
        }
    }
}

// "[exit code N]" / "[killed by signal N]" suffix; "" for a clean exit unless show_ok
inline std::string fmt_exit(int status, bool show_ok) {
    if (WIFEXITED(status))
        return (WEXITSTATUS(status) != 0 || show_ok)
                   ? "\n[exit code " + std::to_string(WEXITSTATUS(status)) + "]" : "";
    if (WIFSIGNALED(status))
        return "\n[killed by signal " + std::to_string(WTERMSIG(status)) + "]";
    return "";
}

inline std::string read_log_tail(const std::string& path, long cap) {
    auto tail = [](const std::string& file, long bytes) {
        std::ifstream f(file, std::ios::binary | std::ios::ate);
        if (!f || bytes == 0) return std::pair<std::string, long>{"", 0};
        long size = static_cast<long>(f.tellg());
        long start = bytes > 0 && size > bytes ? size - bytes : 0;
        f.seekg(start);
        return std::pair<std::string, long>{
            std::string(std::istreambuf_iterator<char>(f),
                        std::istreambuf_iterator<char>()),
            start};
    };
    auto [current, current_start] = tail(path, cap);
    long remaining = cap > 0 ? std::max(0L, cap - static_cast<long>(current.size())) : -1;
    auto [previous, previous_start] = tail(path + ".1", remaining);
    if (current.empty() && previous.empty() &&
        !std::filesystem::exists(path) && !std::filesystem::exists(path + ".1"))
        return "(no output captured: " + path + " missing)";
    std::string s = previous + current;
    if (previous_start > 0 || current_start > 0 ||
        (!previous.empty() && std::filesystem::exists(path + ".1")))
        s = "[rotating log tail]\n" + s;
    return s.empty() ? "(no output)" : s;
}

// Hidden subprocess mode used by detached shells. Two half-size segments keep
// server logs bounded without sending SIGXFSZ/SIGPIPE to the server itself.
inline int tool_log_pump(const std::string& path, long max_bytes) {
    long segment = std::max(512L, max_bytes / 2);
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 1;
    std::array<char, 64 * 1024> buffer{};
    long written = 0;
    for (;;) {
        ssize_t count = read(STDIN_FILENO, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        size_t offset = 0;
        while (offset < static_cast<size_t>(count)) {
            if (written >= segment) {
                close(fd);
                unlink((path + ".1").c_str());
                rename(path.c_str(), (path + ".1").c_str());
                fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (fd < 0) return 1;
                written = 0;
            }
            size_t chunk = std::min(static_cast<size_t>(segment - written),
                                    static_cast<size_t>(count) - offset);
            ssize_t n = write(fd, buffer.data() + offset, chunk);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                close(fd);
                return 1;
            }
            offset += static_cast<size_t>(n);
            written += n;
        }
    }
    close(fd);
    return 0;
}

inline bool process_alive(pid_t pid) {
    return pid > 0 && kill(pid, 0) == 0 && getpgid(pid) == pid;
}

inline std::string detached_record_path(pid_t pid) {
    return uagent_dir("terminals") + "/" + std::to_string(pid) + ".json";
}

inline std::vector<json> detached_records() {
    namespace fs = std::filesystem;
    std::vector<json> records;
    auto cutoff = fs::file_time_type::clock::now() -
                  std::chrono::hours(24 * std::max(
                      0L, env_long("UAGENT_TERMINAL_DAYS", 7)));
    std::error_code ec;
    for (fs::directory_iterator it(uagent_dir("terminals"), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        std::ifstream input(it->path());
        json record = json::parse(input, nullptr, false);
        if (record.is_discarded()) {
            fs::remove(it->path(), ec);
            continue;
        }
        record["_alive"] = process_alive(record.value("pid", 0));
        std::error_code time_error;
        auto modified = fs::last_write_time(it->path(), time_error);
        if (!record["_alive"].get<bool>() && !time_error && modified < cutoff) {
            fs::remove(record.value("log", ""), ec);
            fs::remove(record.value("log", "") + ".1", ec);
            fs::remove(it->path(), ec);
            continue;
        }
        records.push_back(std::move(record));
    }
    std::sort(records.begin(), records.end(), [](const json& a, const json& b) {
        return a.value("started_at", "") > b.value("started_at", "");
    });
    return records;
}

inline std::string save_detached_record(pid_t pid, const std::string& log,
                                        const std::string& cmd) {
    std::error_code ec;
    std::string cwd = std::filesystem::current_path(ec).string();
    json record = {{"pid", pid},
                   {"log", log},
                   {"command", cmd},
                   {"cwd", ec ? "" : cwd},
                   {"started_at", utc_stamp()}};
    return tool_write_private_file(detached_record_path(pid), record.dump(2) + "\n");
}

inline std::string tool_terminal_output(long pid) {
    std::vector<json> records = detached_records();
    if (pid <= 0) {
        if (records.empty()) return "(no detached terminals)";
        std::string out;
        for (const json& record : records) {
            pid_t record_pid = record.value("pid", 0);
            out += (record.value("_alive", false) ? "[running] " : "[exited] ");
            out += "pid " + std::to_string(record_pid) + " · " +
                   record.value("cwd", "") + " · " +
                   one_line(record.value("command", ""), 120) + " · " +
                   record.value("log", "") + "\n";
        }
        long cap = tool_result_cap();
        if (cap > 0 && out.size() > static_cast<size_t>(cap)) {
            out = utf8_trunc(std::move(out), static_cast<size_t>(cap));
            out += "\n[terminal list truncated]";
        }
        return out;
    }

    auto found = std::find_if(records.begin(), records.end(), [&](const json& record) {
        return record.value("pid", 0L) == pid;
    });
    if (found == records.end())
        return "error: pid " + std::to_string(pid) +
               " is not a uagent detached terminal";
    const json& record = *found;
    std::string status = record.value("_alive", false) ? "running" : "exited";
    return "[" + status + " · pid " + std::to_string(pid) + " · " +
           record.value("cwd", "") + " · log " + record.value("log", "") + "]\n" +
           read_log_tail(record.value("log", ""), tool_result_cap());
}

// Drain finished bg jobs: return notification strings for any completed pids.
// Called at step boundaries and in the REPL loop — no threads needed.
inline std::vector<std::string> bg_take_completed(ProcessSupervisor& supervisor) {
    auto& jobs = supervisor.jobs();
    std::vector<std::string> notes;
    for (auto it = jobs.begin(); it != jobs.end();) {
        int st;
        if (waitpid(it->pid, &st, WNOHANG) == it->pid) {
            if (!it->detached) bg_track_signal(it->pid, false);
            std::string out = read_log_tail(it->log, tool_result_cap());
            if (!it->detached) unlink(it->log.c_str());
            std::string note = "[" + std::string(it->detached ? "Detached" : "Background") +
                               " result: pid " + std::to_string(it->pid) +
                               " `" + one_line(it->cmd, 80) + "`]\n" + out +
                               fmt_exit(st, /*show_ok=*/true);
            notes.push_back(std::move(note));
            it = jobs.erase(it);
        } else {
            ++it;
        }
    }
    return notes;
}

inline void bg_shutdown_all(ProcessSupervisor& supervisor) {
    auto& jobs = supervisor.jobs();
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [](const BgJob& job) {
                   if (!job.detached) return false;
                   int status = 0;
                   waitpid(job.pid, &status, WNOHANG);
                   return true;
               }),
               jobs.end());
    for (const BgJob& job : jobs)
        if (kill(-job.pid, SIGTERM) != 0) kill(job.pid, SIGTERM);
    for (int attempt = 0; attempt < 10 && !jobs.empty(); ++attempt) {
        for (auto it = jobs.begin(); it != jobs.end();) {
            int status = 0;
            if (waitpid(it->pid, &status, WNOHANG) == it->pid) {
                bg_track_signal(it->pid, false);
                unlink(it->log.c_str());
                it = jobs.erase(it);
            } else {
                ++it;
            }
        }
        if (!jobs.empty()) usleep(50 * 1000);
    }
    for (const BgJob& job : jobs) {
        kill(-job.pid, SIGKILL);
        kill(job.pid, SIGKILL);
        int status = 0;
        waitpid(job.pid, &status, 0);
        bg_track_signal(job.pid, false);
        unlink(job.log.c_str());
    }
    jobs.clear();
}

inline ProcessSupervisor::~ProcessSupervisor() { bg_shutdown_all(*this); }

// window_s: -1 = default poll window (~3 s), 0 = wait until it finishes,
// N = wait up to N seconds — then background instead of killing.
inline std::string tool_run_bash(ProcessSupervisor& supervisor, const std::string& cmd,
                                 long window_s, bool join_before_final = false,
                                 const ToolContext& context = {},
                                 bool allow_background = true, bool detach = false,
                                 std::string shell = "bash") {
    if (shell.empty() || shell.find('\0') != std::string::npos)
        return "error: shell must be a non-empty executable name or path";
    auto& jobs = supervisor.jobs();
    long max_jobs = std::max(1L, env_long("UAGENT_MAX_BACKGROUND_JOBS", 8));
    if (static_cast<long>(supervisor.pending_count()) >= max_jobs)
        return "error: background job limit reached (" + std::to_string(max_jobs) + ")";
    long window = detach ? 0
                         : (window_s < 0 ? env_long("UAGENT_BASH_POLL", 3)
                                         : (window_s == 0 ? (1L << 30) : window_s));
    if (!detach) window = context.remaining_seconds(window);
    const char* log_kind = detach ? "terminals" : "bg";
    std::string pattern = uagent_dir(log_kind) + "/pending-" +
                          std::to_string(getpid()) + "-XXXXXX";
    std::vector<char> temp(pattern.begin(), pattern.end());
    temp.push_back('\0');
    int lfd = mkstemp(temp.data());
    std::string log = temp.data();
    if (lfd < 0) return "error: cannot create log file " + log;
    fchmod(lfd, 0600);
    long log_bytes = std::max(1024L, env_long("UAGENT_BASH_LOG_BYTES", 64 * 1024 * 1024));
    // Foreground commands may be stopped at the cap. Detached servers instead
    // stream through this binary's tiny rotating log pump and keep running.
    std::string bounded_cmd =
        detach
            ? "(" + cmd + ") 2>&1 | " + shell_quote(g_argv0) + " --log-pump " +
                  shell_quote(log) + " " + std::to_string(log_bytes)
            : "ulimit -f " + std::to_string((log_bytes + 1023) / 1024) +
                  "; " + cmd;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, lfd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, lfd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, lfd);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    short group_flag = POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_SETSID
    if (detach) group_flag = POSIX_SPAWN_SETSID;
#endif
    if (group_flag == POSIX_SPAWN_SETPGROUP)
        posix_spawnattr_setpgroup(&attributes, 0);
    sigset_t defaults, mask;
    sigemptyset(&defaults);
    for (int signal_number : {SIGINT, SIGTERM, SIGHUP, SIGPIPE})
        sigaddset(&defaults, signal_number);
    sigemptyset(&mask);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setsigmask(&attributes, &mask);
    posix_spawnattr_setflags(
        &attributes,
        static_cast<short>(group_flag | POSIX_SPAWN_SETSIGDEF |
                           POSIX_SPAWN_SETSIGMASK));
    pid_t pid = -1;
    auto spawn_shell = [&](const std::string& executable) {
        char* const argv[] = {const_cast<char*>(executable.c_str()),
                              const_cast<char*>("-c"),
                              bounded_cmd.data(), nullptr};
        return posix_spawnp(&pid, executable.c_str(), &actions, &attributes, argv,
                            environ);
    };
    int spawn_error = spawn_shell(shell);
    if (spawn_error != 0 && shell == "bash") spawn_error = spawn_shell("/bin/bash");
    if (spawn_error != 0 && shell == "bash") spawn_error = spawn_shell("/bin/sh");
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_error != 0) {
        close(lfd);
        unlink(log.c_str());
        return "error: cannot spawn shell: " + std::string(strerror(spawn_error));
    }
    close(lfd);
    if (!detach) {
        std::string named = uagent_dir(log_kind) + "/" + std::to_string(pid) + ".log";
        if (rename(log.c_str(), named.c_str()) == 0)
            log = named;  // child's fd stays valid
    }
    if (detach) {
        std::string saved = save_detached_record(pid, log, cmd);
        if (saved.rfind("error:", 0) == 0) {
            if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
            int status = 0;
            waitpid(pid, &status, 0);
            unlink(log.c_str());
            unlink((log + ".1").c_str());
            return saved;
        }
    }

    g_child_pgid = pid;  // Ctrl+C during the window is a hard stop: kill it too
    int status = 0;
    bool exited = false;
    bool cancelled = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(window);
    while (std::chrono::steady_clock::now() < deadline) {
        if (waitpid(pid, &status, WNOHANG) == pid) { exited = true; break; }
        if (abort_requested()) {
            if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            exited = cancelled = true;
            break;
        }
        usleep(100 * 1000);
    }
    g_child_pgid = 0;

    if (exited) {
        std::string out = read_log_tail(log, tool_result_cap());
        unlink(log.c_str());
        unlink((log + ".1").c_str());
        if (detach) unlink(detached_record_path(pid).c_str());
        if (cancelled) return "error: command cancelled by user";
        return out + fmt_exit(status, /*show_ok=*/false);
    }
    if (!allow_background) {
        if (kill(-pid, SIGKILL) != 0) kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        unlink(log.c_str());
        unlink((log + ".1").c_str());
        if (detach) unlink(detached_record_path(pid).c_str());
        return "error: search exceeded its execution deadline";
    }
    if (detach) {
        jobs.push_back({pid, log, cmd, false, true});
        return "[detached] pid " + std::to_string(pid) + ", log: " + log +
               " — read with terminal_output(pid=" + std::to_string(pid) + ")";
    }
    jobs.push_back({pid, log, cmd, join_before_final, false});
    bg_track_signal(pid, true);
    return "[backgrounded] pid " + std::to_string(pid) + ", log: " + log +
           " — peek with read_file, or wait_background(pid=" + std::to_string(pid) + ")";
}

inline std::string tool_run_python(ProcessSupervisor& supervisor,
                                   const std::string& code, const json& packages,
                                   long window_s,
                                   const ToolContext& context = {}) {
    if (!executable_on_path("uv"))
        return "error: uv is not on PATH. Install it from "
               "https://docs.astral.sh/uv/getting-started/installation/ "
               "(macOS: brew install uv), then retry";
    if (code.empty()) return "error: Python code must not be empty";
    if (code.size() > 128 * 1024 || code.find('\0') != std::string::npos)
        return "error: Python code exceeds the 128 KiB limit or contains NUL";
    if (!packages.is_array()) return "error: packages must be an array";
    if (packages.size() > 12) return "error: packages is limited to 12 entries";

    std::string command =
        "UV_NO_PROGRESS=1 MPLBACKEND=Agg uv run --quiet --isolated --no-project";
    for (const json& value : packages) {
        if (!value.is_string()) return "error: package entries must be strings";
        std::string package = value.get<std::string>();
        if (package.empty() || package.size() > 256 ||
            package.find_first_of("\r\n") != std::string::npos ||
            package.find('\0') != std::string::npos)
            return "error: invalid package entry";
        command += " --with " + shell_quote(package);
    }
    command += " -- python -c " + shell_quote(code);
    std::string result =
        tool_run_bash(supervisor, command, window_s, /*join_before_final=*/true, context);
    if (result.find("\n[exit code ") != std::string::npos ||
        result.find("\n[killed by signal ") != std::string::npos) {
        std::string hint;
        if (result.find("No module named") != std::string::npos)
            hint = " Declare every third-party dependency in run_python.packages; "
                   "do not install it with pip or run.";
        return "error: Python execution failed." + hint + "\n" + result;
    }
    return result;
}

inline std::string tool_grep(ProcessSupervisor& supervisor, const std::string& pattern,
                             const std::string& path, const std::string& glob,
                             const ToolContext& context = {}) {
    if (pattern.empty()) return "error: search pattern must not be empty";
    std::string target = path.empty() ? "." : path;
    std::error_code path_error;
    auto status = std::filesystem::status(target, path_error);
    if (path_error || (!std::filesystem::is_regular_file(status) &&
                       !std::filesystem::is_directory(status)))
        return "error: search path is not a readable file or directory: " + target;
    long max_results = std::max(1L, env_long("UAGENT_GREP_RESULTS", 200));
    long bytes = std::max(1024L, env_long("UAGENT_GREP_BYTES", tool_result_cap()));
    if (tool_result_cap() > 0) bytes = std::min(bytes, tool_result_cap());
    bool ripgrep = executable_on_path("rg");
    std::string command;
    if (ripgrep) {
        command = "rg --line-number --column --no-heading --color=never";
        if (!glob.empty()) command += " --glob " + shell_quote(glob);
    } else {
        command = "grep -r -E -n -H -I --exclude-dir=.git";
        if (!glob.empty()) command += " --include=" + shell_quote(glob);
    }
    command = "set -o pipefail; " + command +
               " -- " + shell_quote(pattern) + " " + shell_quote(target) +
               " 2>&1 | head -n " + std::to_string(max_results + 1) +
               " | head -c " + std::to_string(bytes);
    std::string output =
        tool_run_bash(supervisor, command, 0, false, context, false);
    auto exit_suffix = [&](int code) {
        return "\n[exit code " + std::to_string(code) + "]";
    };
    auto has_exit = [&](int code) {
        std::string suffix = exit_suffix(code);
        return output.size() >= suffix.size() &&
               output.compare(output.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (has_exit(1)) return "(no matches)";
    if (has_exit(141))
        output.resize(output.size() - exit_suffix(141).size());
    else if (output.find("\n[exit code ") != std::string::npos)
        return "error: search command failed:\n" + output;
    if (output == "(no output)") return "(no matches)";
    if (output.rfind("error:", 0) == 0) return output;

    bool byte_limited = static_cast<long>(output.size()) >= bytes;
    size_t scan = 0, cut = std::string::npos;
    long lines = 0;
    while (lines < max_results) {
        size_t newline = output.find('\n', scan);
        if (newline == std::string::npos) break;
        ++lines;
        scan = newline + 1;
        if (lines == max_results) cut = scan;
    }
    if (lines < max_results && scan < output.size()) ++lines;
    bool more_results = cut != std::string::npos && cut < output.size();
    if (more_results) output.resize(cut);
    std::string header = "[" + std::string(ripgrep ? "ripgrep" : "grep") +
                         " · " + std::to_string(std::min(lines, max_results)) +
                         (more_results ? "+ matches; more available" : " matches") + "]\n";
    if (byte_limited) header += "[output byte limit reached]\n";
    return header + output;
}

inline std::string tool_wait_background(ProcessSupervisor& supervisor, long pid,
                                        long timeout_s,
                                        const ToolContext& context = {}) {
    auto& jobs = supervisor.jobs();
    auto registered = std::find_if(jobs.begin(), jobs.end(),
                                   [&](const BgJob& job) { return job.pid == (pid_t)pid; });
    if (pid <= 0 || registered == jobs.end())
        return "error: pid " + std::to_string(pid) + " is not a live uagent background job";
    if (registered->detached)
        return "error: detached job " + std::to_string(pid) +
               " is persistent; inspect it with terminal_output";
    std::string log = registered->log;
    int status = 0;
    bool reaped = false;
    std::string note;
    long bounded_timeout = context.remaining_seconds(
        timeout_s > 0 ? timeout_s : (1L << 30));
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(bounded_timeout);
    run_cancellable([&] {  // Ctrl+C cancels the wait, never the process
        while (true) {
            pid_t r = waitpid((pid_t)pid, &status, WNOHANG);
            if (r == (pid_t)pid) { reaped = true; break; }
            if (r < 0 && kill((pid_t)pid, 0) != 0) break;  // reaped earlier
            if (abort_requested()) {
                note = "[wait cancelled — process still running]\n";
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                note = "[wait timed out after " + std::to_string(timeout_s) +
                       "s — process still running]\n";
                break;
            }
            usleep(250 * 1000);
        }
    });
    if (note.empty())  // finished: forget the job
        bg_track_signal(static_cast<pid_t>(pid), false);
    if (note.empty())
        jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                                  [&](const BgJob& j) { return j.pid == (pid_t)pid; }),
                   jobs.end());
    std::string out = note + read_log_tail(log, tool_result_cap());
    if (reaped) out += fmt_exit(status, /*show_ok=*/true);  // confirm completion even on 0
    return out;
}

inline std::string tool_wait_side_task(SideTaskSupervisor& supervisor, long id,
                                       long timeout_s,
                                       const ToolContext& context = {}) {
    if (id <= 0 || !supervisor.contains(id))
        return "error: id " + std::to_string(id) +
               " is not a live uagent background job";
    long bounded_timeout = context.remaining_seconds(
        timeout_s > 0 ? timeout_s : (1L << 30));
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(bounded_timeout);
    while (!abort_requested() && std::chrono::steady_clock::now() < deadline) {
        if (auto result = supervisor.wait(id, std::chrono::milliseconds(0)))
            return "[Background result: " + result->kind + " `" +
                   one_line(result->label, 80) + "`]\n" + result->output;
        supervisor.wait_for_one(std::chrono::milliseconds(100));
    }
    return std::string(abort_requested() ? "[wait cancelled" : "[wait timed out") +
           " — background job still running]";
}

// --- registry ---------------------------------------------------------------

inline std::vector<Tool> builtin_tools(
    ProcessSupervisor& supervisor,
    const std::filesystem::path& workspace = canonical_access_path("."),
    bool inline_images = false, SideTaskSupervisor* side_tasks = nullptr) {
    auto schema = [](const char* s) { return json::parse(s); };
    std::vector<Tool> tools;
    auto path_tool = [&](Tool tool, const char* fallback) -> Tool& {
        tool.needs_approval = [workspace, fallback](const json& args) {
            return !path_within(
                canonical_access_path(args.value("path", fallback)), workspace);
        };
        return add_tool(tools, std::move(tool));
    };

    Tool& read = path_tool(
        make_tool("read_file", "Read a line range.",
                  schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},
                    "offset":{"type":"integer","description":"first line (default 1)"},
                    "limit":{"type":"integer","description":"line count (default 200)"}},
                    "required":["path"]})json"),
                  [](const json& a, const ToolContext&) {
                      return tool_read_file(a.value("path", ""), a.value("offset", 1L),
                                            a.value("limit", 0L));
                  }),
        "");
    read.parallel_safe = true;

    Tool& write = path_tool(
        make_tool("write_file", "Write (overwrite) a file",
                  schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},"content":{"type":"string"}},
                    "required":["path","content"]})json"),
                  [](const json& a, const ToolContext&) {
                      return tool_write_file(a.value("path", ""), a.value("content", ""));
                  }),
        "");
    write.mutating = true;
    write.summary = [](const json& a) {
        return a.value("path", "") + " (" +
               std::to_string(a.value("content", std::string()).size()) + " bytes)";
    };

    Tool& edit = path_tool(
        make_tool("edit_file", "Replace one exact occurrence in a file.",
                  schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},"old":{"type":"string"},
                    "new":{"type":"string"}},"required":["path","old","new"]})json"),
                  [](const json& a, const ToolContext&) {
                      return tool_edit_file(a.value("path", ""), a.value("old", ""),
                                            a.value("new", ""));
                  }),
        "");
    edit.mutating = true;

    Tool& list = path_tool(
        make_tool("list_dir", "List a directory",
                  schema(R"json({"type":"object","properties":{
                    "path":{"type":"string"},"offset":{"type":"integer"},
                    "limit":{"type":"integer"}}})json"),
                  [](const json& a, const ToolContext&) {
                      return tool_list_dir(a.value("path", "."), a.value("offset", 0L),
                                           a.value("limit", 0L));
                  }),
        ".");
    list.parallel_safe = true;

    Tool& grep = path_tool(
        make_tool("grep", "Search project files by regex; optional path/glob.",
                  schema(R"json({"type":"object","properties":{
                    "pattern":{"type":"string"},"path":{"type":"string"},
                    "glob":{"type":"string"}},"required":["pattern"]})json"),
                  [&supervisor](const json& a, const ToolContext& context) {
                      return tool_grep(supervisor, a.value("pattern", ""),
                                       a.value("path", "."), a.value("glob", ""), context);
                  }),
        ".");
    grep.summary = [](const json& a) {
        return a.value("pattern", "") + " in " + a.value("path", ".");
    };

    if (inline_images)
        path_tool(make_tool("view_image",
                            "Display a local image inline in this terminal.",
                            schema(R"json({"type":"object","properties":{
                              "path":{"type":"string"}},"required":["path"]})json"),
                            [](const json& a, const ToolContext&) {
                                return tool_view_image(a.value("path", ""));
                            }),
                  "");

    Tool& run = add_tool(
        tools,
        make_tool("run",
                  "Run a command with bash by default or another requested shell. "
                  "Slow jobs return a pid for wait_background; "
                  "detach=true returns immediately, keeps a long-lived server across sessions, "
                  "and makes its log readable with terminal_output; otherwise timeout=0 waits "
                  "forever.",
                  schema(R"json({"type":"object","properties":{
                    "command":{"type":"string"},
                    "shell":{"type":"string","description":"shell executable (default bash)"},
                    "detach":{"type":"boolean","description":"persist process and log across sessions"}},
                    "required":["command"]})json"),
                  [&supervisor](const json& a, const ToolContext& context) {
                      return tool_run_bash(supervisor, a.value("command", ""),
                                           context.timeout_s, false, context, true,
                                           a.value("detach", false),
                                           a.value("shell", "bash"));
                  }));
    run.mutating = true;
    run.summary = [](const json& a) { return a.value("command", ""); };
    run.timeout_s = 3;
    run.full_terminal_output = true;

    Tool& python = add_tool(
        tools,
        make_tool("run_python",
                  "Run isolated uv Python; list every third-party import in packages. "
                  "Environments cache; pip/venv do not persist. Save plots for view_image.",
                  schema(R"json({"type":"object","properties":{
                    "code":{"type":"string"},
                    "packages":{"type":"array","items":{"type":"string"},"maxItems":12,
                      "description":"all third-party PEP 508 requirements; omit for stdlib"}},
                    "required":["code"]})json"),
                  [&supervisor](const json& a, const ToolContext& context) {
                      return tool_run_python(supervisor, a.value("code", ""),
                                             a.value("packages", json::array()),
                                             context.timeout_s, context);
                  }));
    python.mutating = true;
    python.summary = [](const json& a) {
        std::string summary = a.value("code", "");
        size_t packages = a.value("packages", json::array()).size();
        return packages ? summary + "\n[packages: " + a["packages"].dump() + "]"
                        : summary;
    };
    python.timeout_s = 3;
    python.full_terminal_output = true;

    Tool& wait = add_tool(
        tools,
        make_tool("wait_background",
                  "Wait for a live background job from any tool.",
                  schema(R"json({"type":"object","properties":{
                    "id":{"type":"integer","minimum":1,
                      "description":"job id; process jobs use their OS pid"},
                    "pid":{"type":"integer","minimum":1,
                      "description":"legacy alias for id"}}})json"),
                  [&supervisor, side_tasks](const json& a, const ToolContext& context) {
                      long id = a.value("id", a.value("pid", 0L));
                      if (side_tasks && side_tasks->contains(id))
                          return tool_wait_side_task(*side_tasks, id, context.timeout_s,
                                                     context);
                      return tool_wait_background(supervisor, id, context.timeout_s,
                                                  context);
                  }));
    wait.summary = [](const json& a) {
        return "job " + std::to_string(a.value("id", a.value("pid", 0L)));
    };
    wait.timeout_s = 0;

    Tool& terminal = add_tool(
        tools,
        make_tool("terminal_output",
                  "List uagent-detached terminals, or read the latest output for one pid. "
                  "Only run(detach=true) processes are available.",
                  schema(R"json({"type":"object","properties":{
                    "pid":{"type":"integer","minimum":1}}})json"),
                  [](const json& a, const ToolContext&) {
                      return tool_terminal_output(a.value("pid", 0L));
                  }));
    terminal.parallel_safe = true;
    terminal.result_chars = 6000;
    terminal.summary = [](const json& a) {
        long pid = a.value("pid", 0L);
        return pid ? "pid " + std::to_string(pid) : "list";
    };

    Tool& checkpoint = add_tool(
        tools,
        make_tool("checkpoint",
                  "After a checkpoint suggestion, store durable facts and validation; never "
                  "store commands, permissions, or plans.",
                  schema(R"json({"type":"object","properties":{
                    "state":{"type":"string","description":"standalone durable state"},
                    "verbatim":{"type":"array","items":{"type":"string","maxLength":256},
                      "maxItems":8,"description":"exact literals"},
                    "keep_paths":{"type":"array","items":{"type":"string"},
                      "description":"relevant non-secret files"},
                    "keep_last_n_results":{"type":"integer","minimum":0,"maximum":3,
                      "description":"recent results (default 0)"}},"required":["state"]})json"),
                  [](const json&, const ToolContext&) {
                      return "error: checkpoint must be handled by the agent runtime";
                  }));
    checkpoint.summary = [](const json& a) { return one_line(a.value("state", ""), 100); };
    return tools;
}
