#!/usr/bin/env python3
"""Compare the footprint of coding-agent CLIs the same way for each.

Measures, for every tool given: size on disk, `--version` start-up, an idle
interactive session in a pseudo-terminal, and one tiny headless turn. Runs are
interleaved round-robin across tools so background load affects all of them.
Processes are attributed by PID only: the started process, its descendants, and new detached
(parent PID 1) processes started from the tool's executable path. The headless turn calls the
tool's real model provider with a one-word prompt. Claude Code and Codex CLI run without MCP
servers, plugins, hooks or helpers from the machine's configuration (see BARE_FLAGS).
"""

import argparse
import contextlib
import fcntl
import json
import os
import pathlib
import platform
import pty
import select
import signal
import statistics
import struct
import subprocess
import sys
import tempfile
import termios
import time

PROMPT = "Reply with the single word: ok"
IDLE_SECONDS = 10
QUIET_SECONDS = 2
SETTLE_LIMIT = 20
HEADLESS_TIMEOUT = 180
FIRST_SCREEN_QUIET = 0.5
SYSTEM_PREFIXES = ("/usr/lib/", "/System/", "/lib/", "/lib64/", "/usr/lib64/")
# Flags that keep each tool's own processes only: no MCP servers, plugins, hooks or helpers
# from the machine's configuration. They apply to the idle and the headless measurement.
# Codex has no such flag for its interactive UI, so these apply there; its headless turn uses
# `exec --ignore-user-config` (auth is kept) because `-c mcp_servers={}` does not drop them.
BARE_FLAGS = {
    "uagent": [],
    "claude": [
        "--strict-mcp-config",
        "--mcp-config",
        '{"mcpServers":{}}',
        "--setting-sources",
        "project,local",
        "--disable-slash-commands",
    ],
    "codex": [
        "-c",
        "mcp_servers={}",
        *[
            flag
            for feature in (
                "plugins",
                "apps",
                "computer_use",
                "browser_use",
                "browser_use_external",
                "in_app_browser",
                "hooks",
                "memories",
            )
            for flag in ("--disable", feature)
        ],
    ],
}
# Keys that ask each tool's interactive UI to quit.
QUIT_KEYS = {"uagent": b"/quit\r", "claude": b"/exit\r", "codex": b"/exit\r"}


def cpu_seconds(value):
    result = 0.0
    for field in value.split(":"):
        result = result * 60 + float(field)
    return result


def process_table():
    rows = {}
    output = subprocess.check_output(["ps", "-axo", "pid=,ppid=,rss=,time=,command="], text=True)
    for line in output.splitlines():
        parts = line.split(None, 4)
        if len(parts) < 5:
            continue
        try:
            rows[int(parts[0])] = {
                "ppid": int(parts[1]),
                "rss_kib": int(parts[2]),
                "cpu": cpu_seconds(parts[3]),
                "command": parts[4],
            }
        except ValueError:
            continue
    return rows


class Attribution:
    """The PIDs one launch is responsible for, found by comparing process tables.

    Every started process is tracked so it can be stopped, but only those running the tool's
    own executables (command line starting with a marker) are measured: an MCP server or plugin
    helper the machine's configuration makes the tool spawn is not the tool.
    """

    def __init__(self, root_pid, markers):
        self.root = root_pid
        self.markers = tuple(markers)
        self.before = set(process_table())
        self.pids = {root_pid}
        self.descendants = {root_pid}
        self.last = {}

    def sample(self):
        table = process_table()
        changed = True
        while changed:
            changed = False
            for pid, row in table.items():
                if pid in self.pids or pid in self.before:
                    continue
                if row["ppid"] in self.descendants:
                    self.descendants.add(pid)
                elif row["ppid"] != 1 or not row["command"].startswith(self.markers):
                    continue
                self.pids.add(pid)
                changed = True
        live = {
            pid: table[pid]
            for pid in self.pids
            if pid in table and table[pid]["command"].startswith(self.markers)
        }
        self.last.update({pid: row["cpu"] for pid, row in live.items()})
        return live


def winsize(fd):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))


def kill_recorded(pids):
    for pid in sorted(pids, reverse=True):
        with contextlib.suppress(ProcessLookupError, PermissionError):
            os.kill(pid, signal.SIGTERM)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and any(alive(pid) for pid in pids):
        time.sleep(0.1)
    for pid in pids:
        if alive(pid):
            with contextlib.suppress(ProcessLookupError, PermissionError):
                os.kill(pid, signal.SIGKILL)
    time.sleep(0.2)
    return [pid for pid in pids if alive(pid)]


def alive(pid):
    with contextlib.suppress(ChildProcessError):
        os.waitpid(pid, os.WNOHANG)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def install_root(executable):
    """The resolved executable and what ships with it, as (files, description)."""
    real = executable.resolve()
    parent = real.parent
    if parent.name == "bin":
        prefix = parent.parent
        if (prefix / "share" / "uagent").is_dir():
            return prefix, "bin/ and share/uagent/"
        if (prefix / "codex-package.json").is_file():
            return prefix, "the cask package (bin, codex-resources, codex-path)"
    return real, "the single executable"


def tree_size(root):
    if root.is_file():
        return root.stat().st_size, 1
    total = count = 0
    for path in root.rglob("*"):
        if path.is_file() and not path.is_symlink():
            total += path.stat().st_size
            count += 1
    return total, count


def shared_libraries(binary):
    command = ["otool", "-L"] if sys.platform == "darwin" else ["ldd"]
    try:
        lines = subprocess.check_output(command + [str(binary)], text=True).splitlines()
    except (OSError, subprocess.CalledProcessError):
        return None
    paths = [line.split()[0] for line in lines[1:] if line.strip()]
    return [path for path in paths if path.startswith("/") and not path.startswith(SYSTEM_PREFIXES)]


def own_markers(tool):
    root = install_root(tool["path"])[0]
    return [str(tool["path"]), str(tool["path"].resolve()), str(root)]


def measure_size(tool):
    real = tool["path"].resolve()
    root, what = install_root(tool["path"])
    total, files = tree_size(root)
    libraries = shared_libraries(real)
    return {
        "executable_bytes": real.stat().st_size,
        "install_bytes": total,
        "install_files": files,
        "install_counts": what,
        "non_system_libraries": None if libraries is None else len(libraries),
    }


def spawn(argv, cwd, **kwargs):
    return subprocess.Popen(argv, cwd=cwd, start_new_session=True, **kwargs)


def measure_version(tool, project):
    started = time.perf_counter()
    process = spawn(
        [str(tool["path"]), "--version"],
        project,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    _, _, usage = os.wait4(process.pid, 0)
    wall = time.perf_counter() - started
    return {"wall_ms": wall * 1000, "cpu_ms": (usage.ru_utime + usage.ru_stime) * 1000}


def drain(master, state, terminal_replies=True):
    """Read what the UI wrote; answer the terminal queries a TUI waits on."""
    ready, _, _ = select.select([master], [], [], 0.05)
    if not ready:
        return
    try:
        data = os.read(master, 65536)
    except OSError:
        return
    if not data:
        return
    now = time.monotonic()
    state["last_output"] = now
    state.setdefault("first_output", now)
    state["tail"] = (state.get("tail", b"") + data)[-4096:]
    if terminal_replies:
        if b"\x1b[6n" in data:
            os.write(master, b"\x1b[1;1R")
        if b"\x1b[c" in data or b"\x1b[0c" in data:
            os.write(master, b"\x1b[?1;2c")
    if now - state.get("burst_end", now) > FIRST_SCREEN_QUIET:
        state.setdefault("first_screen", state["burst_end"])
    state["burst_end"] = now


def measure_idle(tool, project):
    master, slave = pty.openpty()
    winsize(slave)
    env = dict(os.environ, TERM="xterm-256color", COLUMNS="120", LINES="40")
    started = time.monotonic()
    process = spawn(
        [str(tool["path"]), *BARE_FLAGS[tool["name"]]],
        project,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=env,
    )
    os.close(slave)
    attribution = Attribution(process.pid, own_markers(tool))
    state = {}
    result = {}
    try:
        while True:
            now = time.monotonic()
            drain(master, state)
            attribution.sample()
            quiet = now - state.get("last_output", started)
            if process.poll() is not None:
                raise RuntimeError(
                    f"the UI exited before it settled: {state.get('tail', b'')[-200:]!r}"
                )
            if (state and quiet >= QUIET_SECONDS) or now - started >= SETTLE_LIMIT:
                break
        first = state.get("first_screen") or state.get("burst_end")
        result["first_screen_ms"] = None if first is None else (first - started) * 1000
        result["settled"] = bool(state) and now - started < SETTLE_LIMIT
        start_cpu = {pid: row["cpu"] for pid, row in attribution.sample().items()}
        samples = []
        window_end = time.monotonic() + IDLE_SECONDS
        while time.monotonic() < window_end:
            drain(master, state)
            live = attribution.sample()
            samples.append(sum(row["rss_kib"] for row in live.values()))
            time.sleep(0.9)
        end_cpu = dict(attribution.last)
        result["rss_kib"] = statistics.median(samples)
        result["idle_cpu_ms"] = (
            sum(c - start_cpu.get(pid, 0.0) for pid, c in end_cpu.items()) * 1000
        )
        live = attribution.sample()
        result["process_count"] = len(live)
        result["tree"] = [
            ("child " if pid in attribution.descendants else "detached ") + row["command"][:100]
            for pid, row in live.items()
        ]
        with contextlib.suppress(OSError):
            os.write(master, QUIT_KEYS[tool["name"]])
        end = time.monotonic() + 3
        while time.monotonic() < end:
            drain(master, state)
    finally:
        left = kill_recorded(sorted(attribution.pids))
        with contextlib.suppress(OSError):
            os.close(master)
        result["left_running"] = left
    return result


def measure_headless(tool, project):
    process = spawn(
        tool["headless"],
        project,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    attribution = Attribution(process.pid, own_markers(tool))
    started = time.perf_counter()
    peak = 0
    tree = []
    status = usage = None
    output = b""
    os.set_blocking(process.stdout.fileno(), False)
    try:
        while True:
            live = attribution.sample()
            total = sum(row["rss_kib"] for row in live.values())
            if total > peak:
                peak = total
                tree = [
                    ("child " if pid in attribution.descendants else "detached ")
                    + row["command"][:100]
                    for pid, row in live.items()
                ]
            with contextlib.suppress(BlockingIOError):
                output += process.stdout.read() or b""
            done, status, usage = os.wait4(process.pid, os.WNOHANG)
            if done:
                break
            if time.perf_counter() - started > HEADLESS_TIMEOUT:
                raise TimeoutError("headless turn timed out")
            time.sleep(0.05)
        wall = time.perf_counter() - started
        with contextlib.suppress(BlockingIOError):
            output += process.stdout.read() or b""
    finally:
        process.stdout.close()
        left = kill_recorded(sorted(attribution.pids))
    reaped = (usage.ru_utime + usage.ru_stime) * 1000
    # Detached helpers are not in the reaped usage; add the CPU time last seen for them.
    detached = sum(c for pid, c in attribution.last.items() if pid not in attribution.descendants)
    return {
        "wall_ms": wall * 1000,
        "cpu_ms": reaped,
        "detached_cpu_ms_seen": detached * 1000,
        "peak_rss_kib": peak,
        "tree_at_peak": tree,
        "ok": os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0 and b"ok" in output.lower(),
        "left_running": left,
    }


def chip_name():
    command = ["sysctl", "-n", "machdep.cpu.brand_string"]
    try:
        return subprocess.check_output(command, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return platform.machine()


def new_project(root, name):
    project = root / name
    project.mkdir()
    (project / "main.py").write_text("print('hello')\n")
    for command in (
        ["git", "init", "-q"],
        ["git", "add", "."],
        ["git", "-c", "user.name=b", "-c", "user.email=b@b", "commit", "-qm", "init"],
    ):
        subprocess.run(command, cwd=project, check=True, capture_output=True)
    return project


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def tool_version(tool):
    output = subprocess.run(
        [str(tool["path"]), "--version"], capture_output=True, text=True, timeout=30
    )
    return (output.stdout or output.stderr).strip()


def build_tools(args):
    tools = []
    uagent_model = ["--model", args.uagent_model] if args.uagent_model else []
    if args.uagent:
        tools.append(
            {
                "name": "uagent",
                "path": args.uagent,
                "headless": [str(args.uagent), "-p", PROMPT, "--json", *uagent_model],
            }
        )
    if args.claude:
        tools.append(
            {
                "name": "claude",
                "path": args.claude,
                "headless": [str(args.claude), *BARE_FLAGS["claude"], "-p", PROMPT],
            }
        )
    if args.codex:
        tools.append(
            {
                "name": "codex",
                "path": args.codex,
                "headless": [
                    str(args.codex),
                    "exec",
                    "--ignore-user-config",
                    "--ephemeral",
                    PROMPT,
                ],
            }
        )
    if not tools:
        sys.exit("give at least one of --uagent, --claude, --codex")
    return tools


def median(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def mb(value):
    return "n/a" if value is None else f"{value / 1e6:.1f}"


def ms(value):
    return "n/a" if value is None else f"{value:.0f}"


def table(tools, report):
    names = [t["name"] for t in tools]
    rows = []

    def row(label, getter):
        rows.append("| " + label + " | " + " | ".join(getter(n) for n in names) + " |")

    def size(n, key):
        return report[n]["size"][key]

    row("Executable alone (MB)", lambda n: mb(size(n, "executable_bytes")))
    row("Install on disk (MB)", lambda n: mb(size(n, "install_bytes")))
    row("Files in the install", lambda n: str(size(n, "install_files")))
    row("Non-system shared libraries", lambda n: str(size(n, "non_system_libraries")))
    row("`--version` wall, median / p90 (ms)", lambda n: stat_pair(report[n], "version", "wall_ms"))
    row("`--version` CPU, median / p90 (ms)", lambda n: stat_pair(report[n], "version", "cpu_ms"))
    row("Idle session RSS, process tree (MB)", lambda n: field(report[n], "idle", "rss_kib", 1024))
    row("Idle session CPU over 10 s (ms)", lambda n: field(report[n], "idle", "idle_cpu_ms"))
    row("Idle processes counted", lambda n: field(report[n], "idle", "process_count"))
    row("Time to first screen (ms)", lambda n: field(report[n], "idle", "first_screen_ms"))
    row("Headless turn CPU (ms)", lambda n: field(report[n], "headless", "cpu_ms"))
    row("Headless turn peak RSS (MB)", lambda n: field(report[n], "headless", "peak_rss_kib", 1024))
    row("Headless turn wall (ms), model-bound", lambda n: field(report[n], "headless", "wall_ms"))
    header = "| Metric | " + " | ".join(names) + " |\n|---|" + "---|" * len(names)
    return header + "\n" + "\n".join(rows)


DISPLAY_NAMES = {"uagent": "µAgent", "claude": "Claude Code", "codex": "Codex CLI"}
CHART_STYLE = """
  .bg { fill: #ffffff; }
  text { font-family: system-ui, -apple-system, "Segoe UI", sans-serif; fill: #1f2328; }
  .title { font-size: 13px; font-weight: 600; }
  .name { font-size: 12px; }
  .value { font-size: 12px; font-variant-numeric: tabular-nums; }
  .note { font-size: 10px; fill: #656d76; }
  .bar { fill: #b4bac1; }
  .bar.uagent { fill: #0a69da; }
  @media (prefers-color-scheme: dark) {
    .bg { fill: #0d1117; }
    text { fill: #e6edf3; }
    .note { fill: #8b949e; }
    .bar { fill: #4a525c; }
    .bar.uagent { fill: #4493f8; }
  }
"""


def panel_values(tools, report, metric, key, divisor):
    """One median per tool, or None where the tool could not be measured."""
    values = {}
    for tool in tools:
        entry = report[tool["name"]]
        samples = [entry["size"][key]] if metric == "size" else [s.get(key) for s in entry[metric]]
        value = median(samples)
        values[tool["name"]] = None if value is None else value / divisor
    return values


def chart_svg(tools, report):
    panels = (
        ("Executable size, MB", panel_values(tools, report, "size", "executable_bytes", 1e6), 1),
        ("Idle session memory, MB", panel_values(tools, report, "idle", "rss_kib", 1024), 1),
        ("CPU per headless turn, ms", panel_values(tools, report, "headless", "cpu_ms", 1), 0),
        ("Time to first screen, ms", panel_values(tools, report, "idle", "first_screen_ms", 1), 0),
    )
    width, height, panel_width, panel_height = 800, 300, 380, 120
    label_width, bar_width = 84, 230
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
        f'width="{width}" height="{height}" role="img" aria-label="Footprint of the agents">',
        f"<style>{CHART_STYLE}</style>",
        f'<rect class="bg" width="{width}" height="{height}"/>',
    ]
    for index, (title, values, places) in enumerate(panels):
        left = 20 + (index % 2) * (panel_width + 20)
        top = 20 + (index // 2) * (panel_height + 15)
        out.append(f'<text class="title" x="{left}" y="{top + 12}">{title}</text>')
        peak = max([v for v in values.values() if v is not None] or [1]) or 1
        for row, tool in enumerate(tools):
            name = tool["name"]
            y = top + 26 + row * 28
            out.append(f'<text class="name" x="{left}" y="{y + 13}">{DISPLAY_NAMES[name]}</text>')
            value = values[name]
            if value is None:
                out.append(f'<text class="value" x="{left + label_width}" y="{y + 13}">n/a</text>')
                continue
            length = max(1, value / peak * bar_width)
            css = "bar uagent" if name == "uagent" else "bar"
            out.append(
                f'<rect class="{css}" x="{left + label_width}" y="{y}" '
                f'width="{length:.1f}" height="18" rx="2"/>'
            )
            out.append(
                f'<text class="value" x="{left + label_width + length + 6:.1f}" y="{y + 13}">'
                f"{value:.{places}f}</text>"
            )
    machine = report["_machine"]
    out.append(
        f'<text class="note" x="20" y="{height - 8}">medians, {machine["chip"]}, '
        f"{machine['date']}; see benchmarks/README.md</text>"
    )
    out.append("</svg>")
    return "\n".join(out) + "\n"


def stat_pair(entry, metric, key):
    samples = [s[key] for s in entry.get(metric, [])]
    if not samples:
        return "n/a"
    return f"{statistics.median(samples):.0f} / {percentile(samples, 0.9):.0f}"


def field(entry, metric, key, divisor=1):
    value = median([s.get(key) for s in entry.get(metric, [])])
    if value is None:
        return "n/a"
    return f"{value / divisor:.1f}" if divisor != 1 else f"{value:.0f}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uagent", type=pathlib.Path, help="path to the uagent executable")
    parser.add_argument("--claude", type=pathlib.Path, help="path to the claude executable")
    parser.add_argument("--codex", type=pathlib.Path, help="path to the codex executable")
    parser.add_argument("--uagent-model", help="--model value for the uagent headless turn")
    parser.add_argument("--version-runs", type=int, default=30)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--session-runs", type=int, default=5, help="idle and headless runs")
    parser.add_argument("--skip", action="append", default=[], choices=["idle", "headless"])
    parser.add_argument("--chart", type=pathlib.Path, help="also write an SVG bar chart here")
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("footprint.json"))
    args = parser.parse_args()
    tools = build_tools(args)
    for tool in tools:
        tool["path"] = tool["path"].absolute()
    report = {n["name"]: {"idle": [], "headless": [], "version": [], "errors": []} for n in tools}
    with tempfile.TemporaryDirectory(prefix="agents-footprint-") as temporary:
        root = pathlib.Path(temporary)
        projects = {t["name"]: new_project(root, t["name"]) for t in tools}
        for tool in tools:
            entry = report[tool["name"]]
            entry["version_string"] = tool_version(tool)
            entry["size"] = measure_size(tool)
        for _ in range(args.warmups):
            for tool in tools:
                measure_version(tool, projects[tool["name"]])
        for _ in range(args.version_runs):
            for tool in tools:
                report[tool["name"]]["version"].append(
                    measure_version(tool, projects[tool["name"]])
                )
        for metric, function in (("idle", measure_idle), ("headless", measure_headless)):
            if metric in args.skip:
                continue
            for _ in range(args.session_runs):
                for tool in tools:
                    entry = report[tool["name"]]
                    try:
                        entry[metric].append(function(tool, projects[tool["name"]]))
                    except (RuntimeError, TimeoutError, OSError) as error:
                        entry["errors"].append(f"{metric}: {error}")
    report["_machine"] = {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "chip": chip_name(),
        "cpus": os.cpu_count(),
        "date": time.strftime("%Y-%m-%d"),
        "runs": {"version": args.version_runs, "session": args.session_runs},
    }
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(table(tools, report))
    if args.chart:
        args.chart.write_text(chart_svg(tools, report))
    for tool in tools:
        for error in report[tool["name"]]["errors"]:
            print(f"{tool['name']}: {error}", file=sys.stderr)


if __name__ == "__main__":
    main()
