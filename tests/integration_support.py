#!/usr/bin/env python3
import errno
import fcntl
import json
import os
import pathlib
import pty
import re
import select
import shlex
import signal
import struct
import subprocess
import sys
import termios
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Enough of a PNG for the attachment inspector to accept it.
SMALL_PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 32


def _binary_from_argv():
    """The suite is invoked as `integration.py BINARY ...`.

    Importing this module for its HTTP/SSE fixture alone — the eval harness
    reuses `Server`, `sse` and `event` rather than keeping a second copy — must
    not require that argument.
    """
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        return pathlib.Path(sys.argv[1]).resolve()
    return None


BINARY = _binary_from_argv()
# A sanitized or coverage-instrumented binary starts and renders several times
# slower than a plain one, which turns every wall-clock budget below into a
# coin flip on a shared runner. Those jobs raise the multiplier instead of each
# deadline being retuned by hand.
TIMEOUT_SCALE = float(os.environ.get("UAGENT_TEST_TIMEOUT_SCALE", "1"))


def budget(seconds):
    """Scale a test-side deadline for a slow build."""
    return seconds * TIMEOUT_SCALE


def event(delta=None, finish="stop", usage=None):
    choice = {"delta": delta or {}, "finish_reason": finish}
    payload = {"choices": [choice]}
    if usage is not None:
        payload["usage"] = usage
    return payload


def sse(payload):
    return ("data: " + json.dumps(payload) + "\n\ndata: [DONE]\n\n").encode()


def write_sse_sequence(handler, payloads, delay=0):
    handler.send_response(200)
    handler.send_header("Content-Type", "text/event-stream")
    handler.send_header("Connection", "close")
    handler.end_headers()
    handler.close_connection = True
    try:
        for payload in payloads:
            handler.wfile.write(("data: " + json.dumps(payload) + "\n\n").encode())
            handler.wfile.flush()
            if delay:
                time.sleep(delay)
        handler.wfile.write(b"data: [DONE]\n\n")
        handler.wfile.flush()
    except (BrokenPipeError, ConnectionResetError):
        pass


def write_http_response(handler, data, content_type="application/json", status=200):
    handler.send_response(status)
    handler.send_header("Content-Type", content_type)
    handler.send_header("Content-Length", str(len(data)))
    handler.end_headers()
    try:
        handler.wfile.write(data)
    except (BrokenPipeError, ConnectionResetError):
        pass


def write_json_response(handler, payload, status=200):
    write_http_response(handler, json.dumps(payload).encode(), status=status)


def two_route_providers(first_url, second_url):
    return {
        "first": {
            "base_url": first_url,
            "api_key": "key-a",
            "context": 4096,
            "models": {"main": {"id": "model-a", "effort": "low"}},
        },
        "second": {
            "base_url": second_url,
            "api_key": "key-b",
            "models": {"fast": {"id": "model-b", "effort": "medium"}},
        },
    }


class Server:
    def __init__(self, responders, get_response=None):
        self.responders = list(responders)
        self.requests = []
        self.get_requests = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                owner.get_requests.append(self.path)
                if get_response is None:
                    self.send_error(404)
                    return
                write_json_response(self, get_response)

            def do_POST(self):
                size = int(self.headers.get("Content-Length", "0"))
                body = json.loads(self.rfile.read(size))
                owner.requests.append((dict(self.headers), body))
                index = len(owner.requests) - 1
                response = owner.responders[min(index, len(owner.responders) - 1)]
                if callable(response):
                    response = response(self, body)
                    if response is None:
                        return
                streaming = body.get("stream", True)
                data = sse(response) if streaming else json.dumps(response).encode()
                write_http_response(
                    self,
                    data,
                    "text/event-stream" if streaming else "application/json",
                )

            def log_message(self, *_):
                pass

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.thread = threading.Thread(
            target=self.httpd.serve_forever,
            kwargs={"poll_interval": 0.01},
            daemon=True,
        )
        self.thread.start()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.httpd.server_port}/v1"

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=2)


def base_env(home, url):
    env = {key: value for key, value in os.environ.items() if not key.startswith("UAGENT_")}
    env.update(
        {
            "HOME": str(home),
            "UAGENT_BASE_URL": url,
            "UAGENT_MODEL": "test",
            "UAGENT_CONTEXT": "16384",
            "UAGENT_REQUEST_TIMEOUT": "5",
            "UAGENT_FIRST_EVENT_TIMEOUT": "2",
            "UAGENT_STREAM_IDLE_TIMEOUT": "2",
        }
    )
    return env


def provider_env(home, url, providers, model=None):
    env = base_env(home, url)
    env["UAGENT_PROVIDERS"] = json.dumps(providers)
    if model is None:
        env.pop("UAGENT_MODEL")
    else:
        env["UAGENT_MODEL"] = model
    return env


def run(cwd, env, *args, timeout=10):
    return subprocess.run(
        [str(BINARY), *args],
        cwd=cwd,
        env=env,
        stdin=subprocess.DEVNULL,
        text=True,
        capture_output=True,
        timeout=budget(timeout),
    )


def run_dialog(cwd, env, text, *args, timeout=10):
    return subprocess.run(
        [str(BINARY), *args],
        cwd=cwd,
        env=env,
        input=text,
        text=True,
        capture_output=True,
        timeout=budget(timeout),
    )


def run_pty(
    cwd,
    env,
    payload=b"",
    timeout=10,
    columns=80,
    args=(),
    startup_marker=None,
    configure_terminal=None,
    before_payload=None,
    after_exit=None,
    suspend=None,
):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, columns, 0, 0))
    if configure_terminal is not None:
        configure_terminal(slave)
    # A session of its own isolates the child from the runner's signals, but it
    # also orphans its process group, and the kernel discards stop signals sent
    # to an orphaned group. A suspend case therefore needs a plain process
    # group inside this session, the way a shell's job control provides one.
    placement = {"process_group": 0} if suspend else {"start_new_session": True}
    process = subprocess.Popen(
        [str(BINARY), *args],
        cwd=cwd,
        env=env,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        **placement,
    )
    os.close(slave)
    output = bytearray()
    deadline = time.monotonic() + budget(timeout)
    last_match_end = 0

    def read_until(marker=None, start=0, following=None):
        nonlocal last_match_end
        while time.monotonic() < deadline:
            if marker is not None:
                marker_at = output.find(marker, start)
                if marker_at >= 0:
                    after_marker = marker_at + len(marker)
                    if following is None or following in output[after_marker:]:
                        last_match_end = after_marker
                        return True
            if select.select([master], [], [], 0.1)[0]:
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    return False
                if not chunk:
                    return False
                output.extend(chunk)
            elif process.poll() is not None:
                return False
        return marker is None

    def read_prompt(start=0):
        read_until(
            b"\x1b[36m> \x1b[0m\x1b[39m\x1b[49m",
            min(start, last_match_end),
        )
        time.sleep(0.05)  # the composer finishes raw-mode setup after drawing

    def write_fragment(fragment):
        offset = 0
        while offset < len(fragment):
            readable, writable, _ = select.select([master], [master], [], 0.1)
            if readable:
                chunk = os.read(master, 65536)
                if not chunk:
                    return
                output.extend(chunk)
            if process.poll() is not None:
                return
            if writable:
                offset += os.write(master, fragment[offset : offset + 4096])

    if startup_marker is None:
        read_prompt()
    else:
        read_until(startup_marker)
        time.sleep(0.05)
    if before_payload is not None:
        try:
            before_payload()
        except BaseException:
            # Callback assertions must not strand an interactive child. A
            # leaked raw-mode process spins after its PTY owner disappears and
            # can starve every sanitizer case that follows.
            process.kill()
            process.wait()
            os.close(master)
            raise
    if suspend is not None:
        suspend(process, master)
    payloads = [payload] if isinstance(payload, bytes) else payload
    for index, item in enumerate(payloads):
        marker = None
        resized_columns = None
        if isinstance(item, tuple):
            if len(item) == 3:
                item, marker, resized_columns = item
            else:
                item, marker = item
                resized_columns = None
        start = len(output)
        try:
            # A callable acts on the child instead of typing at it: this
            # PTY is not its controlling terminal, so ^C cannot be typed.
            if callable(item):
                item(process)
            else:
                fragments = item if isinstance(item, list) else [item]
                for fragment in fragments:
                    write_fragment(fragment)
                    if len(fragments) > 1:
                        time.sleep(0.01)
        except OSError as error:
            if error.errno != errno.EIO:
                raise
            read_until()
            break
        if resized_columns:
            fcntl.ioctl(
                master,
                termios.TIOCSWINSZ,
                struct.pack("HHHH", 24, resized_columns, 0, 0),
            )
            # This PTY is not the child's controlling terminal, so mirror
            # the SIGWINCH a real terminal sends to its foreground group.
            process.send_signal(signal.SIGWINCH)
            time.sleep(0.05)
        if index + 1 < len(payloads):
            if marker is not None and not read_until(marker, start):
                break
            if marker is None:
                read_prompt(start)
    read_until()
    if process.poll() is None:
        process.kill()
    process.wait()
    # Inspect the PTY the child left behind, line discipline included, while
    # the master is still open.
    if after_exit is not None:
        after_exit(master)
    os.close(master)
    return process.returncode, bytes(output)


def assert_true(value, message):
    if not value:
        raise AssertionError(message)


def tool_results(messages):
    """Every tool-role message content, in order."""
    return [str(m.get("content", "")) for m in messages if m.get("role") == "tool"]


def has_message(messages, role, content):
    return any(
        message.get("role") == role and message.get("content") == content for message in messages
    )


def tool_calls(calls, usage=None):
    return event(
        {
            "tool_calls": [
                {
                    "index": index,
                    "id": call_id,
                    "function": {"name": name, "arguments": json.dumps(arguments)},
                }
                for index, (call_id, name, arguments) in enumerate(calls)
            ]
        },
        finish="tool_calls",
        usage=usage,
    )


def tool_call(name, arguments, *, call_id="call-1", usage=None):
    return tool_calls([(call_id, name, arguments)], usage)


def detached_pid(body):
    result = next(
        message.get("content", "")
        for message in reversed(body["messages"])
        if message.get("role") == "tool" and "[detached] pid " in message.get("content", "")
    )
    match = re.search(r"\[detached\] pid (\d+)", result)
    assert_true(match is not None, result)
    return int(match.group(1))


def signal_process_group(pid, signal_number=signal.SIGTERM):
    if pid is None:
        return
    try:
        os.killpg(pid, signal_number)
    except ProcessLookupError:
        pass


def live_process_states(pids):
    """Return process states for PIDs that still represent live processes."""
    states = {}
    for pid in pids:
        result = subprocess.run(
            ["ps", "-o", "stat=", "-p", str(pid)],
            text=True,
            capture_output=True,
            check=False,
        )
        state = result.stdout.strip()
        if result.returncode == 0 and state and not state.startswith("Z"):
            states[pid] = state
    return states


def wait_for_processes_stopped(pids, timeout=2):
    """Allow process-group teardown and orphan reaping to settle."""
    deadline = time.monotonic() + budget(timeout)
    states = live_process_states(pids)
    while states and time.monotonic() < deadline:
        time.sleep(0.02)
        states = live_process_states(states)
    return states


def descendant_pids(root_pid):
    """Every process below root_pid, so a leaked grandchild is still visible."""
    children = {}
    for line in subprocess.check_output(["ps", "-axo", "pid=,ppid="], text=True).splitlines():
        child, parent = map(int, line.split())
        children.setdefault(parent, []).append(child)
    found, pending = set(), [root_pid]
    while pending:
        for child in children.get(pending.pop(), []):
            if child not in found:
                found.add(child)
                pending.append(child)
    return found


def wait_until(predicate, message, timeout=30, interval=0.02):
    """Poll until predicate() holds, or fail the test with message."""
    deadline = time.monotonic() + budget(timeout)
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(interval)
    raise AssertionError(message)


def write_session(home, name, messages, *, cwd, kinds=None, context_tokens=0, **header):
    """Write one saved session the way the agent persists it.

    The format-3 envelope lives here alone: a schema change is one edit, and a
    fixture cannot drift into a shape the agent would never write.
    """
    fields = {
        "format": 3,
        "cwd": str(pathlib.Path(cwd).resolve()),
        "model": "test",
        "session_id": name,
        "turns": len(messages),
        "title": name,
    }
    fields.update(header)
    payload = {
        "messages": messages,
        "message_kinds": [message["role"] for message in messages] if kinds is None else kinds,
        "archive": [],
        "archive_dropped_segments": 0,
        "context_tokens": context_tokens,
        "usage": {},
        "route_usage": {},
    }
    history = home / ".uagent" / "history"
    history.mkdir(parents=True, exist_ok=True)
    session = history / f"{name}.json"
    session.write_text(json.dumps(fields) + "\n" + json.dumps(payload), encoding="utf-8")
    return session


def function_tools(body):
    return [tool["function"] for tool in body.get("tools", []) if tool.get("type") == "function"]


def function_names(body):
    return {tool["name"] for tool in function_tools(body)}


def function_tool(body, name):
    return next(tool for tool in function_tools(body) if tool["name"] == name)


def large_json_command():
    payload = {
        "sentinel": "HEAD-ONLY",
        "padding": "x" * 12000,
        "tail": "FULL-END",
    }
    script = (
        "import json;"
        "print(json.dumps({'sentinel':'HEAD-ONLY','padding':'x'*12000,"
        "'tail':'FULL-END'},separators=(',',':')))"
    )
    command = f"{shlex.quote(sys.executable)} -c {shlex.quote(script)}"
    return command, len((json.dumps(payload, separators=(",", ":")) + "\n").encode())


def captured_log_path(result):
    prefix = "[captured log: "
    start = result.find(prefix)
    end = result.find(" (", start + len(prefix))
    assert_true(start >= 0 and end > start, result)
    return pathlib.Path(result[start + len(prefix) : end])


def json_sentinel_command(path):
    query = "import json,sys;print(json.load(open(sys.argv[1], encoding='utf-8'))['sentinel'])"
    return f"{shlex.quote(sys.executable)} -c {shlex.quote(query)} {shlex.quote(str(path))}"


def midturn_compaction_env(home, url):
    env = base_env(home, url)
    env.update(
        {
            "UAGENT_CONTEXT": "8000",
            "UAGENT_MAX_TOKENS": "512",
            "UAGENT_AUTO_COMPACT_PCT": "40",
            "UAGENT_TOOL_RESULT_CHARS": "8000",
        }
    )
    return env


def wait_until_stopped(pid, timeout=10):
    """WUNTRACED reports a job-control stop without reaping the child."""
    deadline = time.monotonic() + budget(timeout)
    while time.monotonic() < deadline:
        waited, status = os.waitpid(pid, os.WUNTRACED | os.WNOHANG)
        if waited == pid and os.WIFSTOPPED(status):
            return os.WSTOPSIG(status)
        time.sleep(0.05)
    return 0


def wait_for_echo(master, wanted, timeout=10):
    """Wait for the line discipline to reach a state, rather than sleeping.

    A fixed sleep is what makes a terminal test flaky on a loaded machine.
    """
    deadline = time.monotonic() + budget(timeout)
    while time.monotonic() < deadline:
        lflag = termios.tcgetattr(master)[3]
        if bool(lflag & termios.ECHO) == wanted:
            return lflag
        time.sleep(0.05)
    return termios.tcgetattr(master)[3]


TEST_ORDER = (
    "test_plain_turn",
    "test_adaptive_system_revises_replaces_and_clears",
    "test_stream_error_is_not_an_empty_response",
    "test_empty_response_after_tools_recovers",
    "test_foreign_tool_markup_recovers_as_prose",
    "test_transient_stream_errors_retry_before_progress",
    "test_command_help",
    "test_reasoning_modes_render_consistently",
    "test_config_reload_applies_only_at_turn_boundaries",
    "test_project_instructions_precede_first_turn",
    "test_prompt_overlay_replaces_base_sections",
    "test_attach_tool_puts_bytes_in_context",
    "test_full_run_and_python_terminal_trace",
    "test_large_run_output_is_recoverable",
    "test_multiline_bracketed_paste",
    "test_resume_picker_accepts_enter_when_icrnl_was_disabled",
    "test_session_journal_records_digests_not_argument_values",
    "test_session_title_replaces_initial_greeting",
    "test_input_redraw_focus_switch_preserves_multiline_draft",
    "test_input_redraw_bare_escape_still_clears_idle_draft",
    "test_input_redraw_history_restores_current_draft",
    "test_input_redraw_approval_does_not_pollute_history",
    "test_multiline_run_keeps_action_color",
    "test_multiline_rejected_call_shows_arguments",
    "test_input_redraw_enter_then_escape_same_packet_interrupts_turn",
    "test_input_steering_yields_activity_wait",
    "test_input_idle_background_completion_is_observational",
    "test_input_redraw_streaming_tail_survives_resize",
    "test_input_redraw_status_animation_does_not_repaint_draft",
    "test_input_slash_suggestions_and_tab_completion",
    "test_input_shift_enter_keeps_the_draft_open",
    "test_input_ctrl_c_asks_once_then_quits",
    "test_suspend_restores_and_rearms_terminal",
    "test_signal_exit_restores_terminal",
    "test_input_redraw_survives_terminal_resize_and_delete",
    "test_resize_replaces_the_status_row_instead_of_appending",
    "test_run_rejects_python_and_sudo_before_execution",
    "test_self_configuration_requires_a_person",
    "test_self_configuration_asks_even_under_yolo",
    "test_approval_remembers_always_and_forwards_a_refusal",
    "test_self_configuration_commits_after_approval",
    "test_process_hardening_scrubs_loader_variables",
    "test_streamed_search_citations",
    "test_openrouter_named_search_contract_and_errors",
    "test_openrouter_reasoning_details_survive_tool_step",
    "test_provider_context_overflow_compacts_once",
    "test_provider_background_completion_does_not_trigger_model_turns",
    "test_provider_text_protocol_preserves_reasoning_and_trace",
    "test_provider_responses_native_search_and_function_replay",
    "test_provider_anthropic_native_search_pause_turn_replay",
    "test_self_info_reports_live_configuration",
    "test_effort_and_variant_persist_like_model",
    "test_headless_json_envelope_contains_trace_usage_and_exit",
    "test_headless_json_stream_emits_lifecycle_events",
    "test_session_budget_stops_before_the_next_call",
    "test_turn_cost_is_unlimited_by_default",
    "test_tool_policy_scopes_schema_and_runtime",
    "test_delete_file_removes_and_receipts",
    "test_grep_tool_round_trip",
    "test_project_agent_config_trust",
    "test_memory_reaches_context_by_scope",
    "test_configured_redaction_keywords_apply",
    "test_context_command_shows_memory_and_skills",
    "test_memory_background_extractor_is_bounded",
    "test_memory_background_extractor_releases_failed_claims",
    "test_no_memory_hides_index_and_tool",
    "test_skill_tool_offers_and_opens",
    "test_mcp_image_reaches_the_model",
    "test_invalid_mcp_config_not_executed",
    "test_mcp_tool_round_trip",
    "test_model_route_switch",
    "test_openrouter_variant_is_scoped_to_openrouter",
    "test_dynamic_provider_catalog_and_model",
    "test_model_preference_survives_restart",
    "test_first_event_timeout",
    "test_midturn_compaction_preserves_progress_and_usage",
    "test_absolute_compaction_ceiling",
    "test_tool_trace_repeated_rounds_are_telemetry_only",
    "test_invalid_tool_rejection_loop_stops_before_fourth_round",
    "test_activity_progress_polls_do_not_trip_identical_call_guard",
    "test_activity_no_change_polls_are_steered_then_stopped",
    "test_activity_poll_in_productive_batches_does_not_form_a_loop",
    "test_detached_terminal_materialized_wait_does_not_bypass_repeat_guard",
    "test_tool_call_budget_is_unlimited_by_default",
    "test_subagent_auto_join_continues_turn",
    "test_subagent_foreground_returns_result_without_wait_round",
    "test_subagent_reports_the_limit_that_stopped_the_child",
    "test_subagent_foreground_outlives_the_per_call_budget",
    "test_subagent_clamps_are_reported_not_silent",
    "test_parallel_subagents_auto_join",
    "test_subagent_interrupt_reaps_child",
    "test_activity_wait_outlives_the_per_call_budget",
    "test_parallel_run_overlaps",
    "test_subagent_uses_selected_model_route",
    "test_subagent_failure_reports_route_stage_and_bounded_diagnostics",
    "test_image_fallback_reaches_another_provider",
    "test_subagent_recursion_is_depth_bounded",
    "test_headless_reaps_timed_out_process",
    "test_detached_terminal_survives_and_is_readable",
    "test_detached_terminal_tracks_group_after_wrapper_exit",
)
