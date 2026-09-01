from integration_support import (
    Server,
    assert_true,
    base_env,
    event,
    json,
    re,
    run_dialog,
    run_pty,
    signal,
    termios,
    time,
    tool_call,
    wait_for_echo,
    wait_until_stopped,
    write_session,
    write_sse_sequence,
)
from memory_fixture import project_memory_dir


def test_yolo_toggle_refreshes_approval_state(root, home):
    def route(_, body):
        messages = body["messages"]
        system = messages[0].get("content", "")
        turns = [
            (index, str(message.get("content", "")))
            for index, message in enumerate(messages)
            if message.get("role") == "user"
            and str(message.get("content", "")) in {"check-on", "check-off"}
        ]
        assert_true(turns, messages)
        turn_start, turn_prompt = turns[-1]
        results = [
            str(message.get("content", ""))
            for message in messages[turn_start + 1 :]
            if message.get("role") == "tool"
        ]
        on_turn = turn_prompt == "check-on"
        expected_mode = "automatic" if on_turn else "prompted"
        expected_env = "env-on" if on_turn else "env-off"
        assert_true(f"approval={expected_mode}" in system, system)
        if results:
            assert_true(expected_env in results[-1], results)
            return event({"content": f"{expected_env}-ok"})
        env_value = "yolo" if on_turn else "prompt"
        return tool_call(
            "run",
            {"command": (f'test "$UAGENT_APPROVAL" = {env_value} && printf {expected_env}')},
        )

    with Server([route]) as server:
        result = run_dialog(
            root,
            base_env(home, server.url),
            "/yolo\ncheck-on\n/yolo\ncheck-off\ny\n/q\n",
        )
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true("env-on-ok" in result.stdout, result.stdout)
        assert_true("env-off-ok" in result.stdout, result.stdout)
        assert_true(result.stdout.count("allow run?") == 1, result.stdout)
        assert_true(len(server.requests) == 4, server.requests)


def test_command_help(root, home):
    with Server([event({"content": "unused"})]) as server:
        result = run_dialog(root, base_env(home, server.url), "/models\n/wat\n/recap\n/help\n/q\n")
        assert_true(result.returncode == 0, result.stderr)
        assert_true("unknown command /wat; use /help" in result.stdout, result.stdout)
        assert_true("unknown command /recap; use /help" in result.stdout, result.stdout)
        assert_true("commands\n" in result.stdout, result.stdout)
        assert_true("  /attach PATH" in result.stdout, result.stdout)
        assert_true("attach a file to the next turn" in result.stdout, result.stdout)
        assert_true("  /help" in result.stdout, result.stdout)
        assert_true("show this help" in result.stdout, result.stdout)
        assert_true("commands:" not in result.stdout, result.stdout)
        assert_true("use /models QUERY" in result.stdout, result.stdout)
        assert_true(not server.get_requests, server.get_requests)


def test_reasoning_modes_render_consistently(root, home):
    def streamed(handler, _):
        write_sse_sequence(
            handler,
            [
                event({"reasoning": "first line\nlatest line"}, finish=None),
                event(
                    {"content": "Final answer"},
                    usage={"prompt_tokens": 2, "completion_tokens": 2},
                ),
            ],
            delay=0.25,
        )

    def styled(handler, _):
        write_sse_sequence(
            handler,
            [
                event(
                    {
                        "reasoning": (
                            "**Map core invariants****Detail implementation and tests****"
                            "Planning provider normalization phases**"
                        )
                    },
                    finish=None,
                ),
                event({"content": "Final answer"}),
            ],
            delay=0.25,
        )

    with Server([streamed, styled]) as server:
        env = base_env(home, server.url)
        env["UAGENT_MEMORY"] = "0"
        code, verbose = run_pty(
            root,
            env,
            [
                (b"/verbose\n", b"verbose ON"),
                (b"go\n", b"Final answer"),
                b"/q\n",
            ],
            timeout=10,
        )
        assert_true(code == 0, verbose)
        assert_true(b"\xc2\xb7 thinking" in verbose, verbose)
        reasoning_style = b"\x1b[0m\x1b[39m\x1b[49m\x1b[90m\x1b[3m"
        assert_true(reasoning_style + b"latest line" in verbose, verbose)
        assert_true(verbose.find(b"first line") < verbose.find(b"Final answer"), verbose)

        # The working row also names the route, so the ticker window is a few
        # columns narrower than the reasoning it scrolls: assert the clause
        # that fits rather than a width budget the row no longer has.
        code, compact = run_pty(
            root,
            env,
            [
                (b"go\n", b"provider normalization phases"),
                (b"", b"Final answer"),
                b"/q\n",
            ],
            timeout=10,
        )
        assert_true(code == 0, compact)
        assert_true(b"thinking \xc2\xb7" in compact, compact)
        assert_true(b"provider normalization phases" in compact, compact)
        assert_true(b"thinking \xc2\xb7 \xe2\x80\xa6" not in compact, compact)
        assert_true(b"****" not in compact, compact)
        assert_true(b"\xc2\xb7 thinking" not in compact, compact)


def test_multiline_bracketed_paste(root, home):
    def verify(_, body):
        pasted = body["messages"][-1].get("content")
        return event(
            {
                "content": "multiline-paste-ok"
                if pasted == "first line\nsecond line\nthird line"
                else "multiline-paste-bad"
            }
        )

    with Server([verify]) as server:
        paste = [
            b"\x1b",
            b"[2",
            b"00~first line\r\nsecond line\rthird line\x1b[20",
            b"1~",
        ]
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(paste, b"second"), b"\n", b"\x04"],
            columns=24,
        )
        assert_true(code == 0, output)
        assert_true(b"multiline-paste-ok" in output, output)
        assert_true(b"ctx " in output, output)
        assert_true(b"\x1b[?2004h" in output and b"\x1b[?2004l" in output, output)
        # The echoed turn is banded to the right edge on every row it spans,
        # and the band is always closed again.
        band = b"\x1b[48;5;250m"
        assert_true(output.count(band) >= 2, output)
        assert_true(output.rfind(b"\x1b[0m\x1b[39m\x1b[49m") > output.rfind(band), output)
        assert_true(output.count(band + b"first line\x1b[K\r\n") == 1, output)
        assert_true(output.count(band + b"third line\x1b[K") == 1, output)
        assert_true(b"\x1b[36m> \x1b[0m\x1b[39m\x1b[49m" in output, output)
        assert_true(len(server.requests) == 1, len(server.requests))


def test_enter_arriving_with_paste_does_not_submit(root, home):
    def verify(_, body):
        pasted = body["messages"][-1].get("content")
        return event(
            {"content": ("paste-enter-ok" if pasted == "safe paste" else "paste-enter-bad")}
        )

    with Server([verify]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"\x1b[200~safe paste\x1b[201~\n", b"safe paste\xe2\x86\xb5"),
                b"\n",
                b"\x04",
            ],
        )
        assert_true(code == 0, output)
        assert_true(b"paste-enter-ok" in output, output)
        assert_true(len(server.requests) == 1, server.requests)


def test_resume_picker_accepts_enter_when_icrnl_was_disabled(root, home):
    write_session(
        home,
        "resume-picker",
        [{"role": "system", "content": "saved system"}],
        cwd=root,
        context_tokens=1_900_000,
        session_id="resume-picker-test",
        turns=0,
        title="saved session",
    )

    def disable_icrnl(slave):
        attributes = termios.tcgetattr(slave)
        attributes[0] &= ~termios.ICRNL
        attributes[3] |= termios.ICANON | termios.ECHO
        termios.tcsetattr(slave, termios.TCSANOW, attributes)

    with Server([event({"content": "unused"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [b"1\r", b"/q\r"],
            args=("--resume",),
            startup_marker=b"resume #: ",
            configure_terminal=disable_icrnl,
        )
        assert_true(code == 0, output)
        assert_true(b"resumed" in output, output)
        assert_true(b"ctx 1.9M" not in output, output)
        assert_true(b"^M" not in output, output)
        assert_true(len(server.requests) == 0, server.requests)


def test_input_redraw_focus_switch_preserves_multiline_draft(root, home):
    def verify(_, body):
        pasted = body["messages"][-1].get("content")
        return event(
            {
                "content": (
                    "focus-draft-ok"
                    if pasted == "first line\nsecond line\nthird line"
                    else "focus-draft-bad"
                )
            }
        )

    with Server([verify]) as server:
        input_fragments = [
            b"\x1b[200~first line\r\nsecond line\x1b[201~",
            b"\x1b",
            b"[",
            b"O",
            b"\x1b[",
            b"I",
            b"\x1b[200~\nthird line\x1b[201~",
        ]
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(input_fragments, b"third"), b"\n", b"\x04"],
            columns=24,
        )
        assert_true(code == 0, output)
        assert_true(b"focus-draft-ok" in output, output)
        assert_true(b"[Ifirst" not in output and b"[Ofirst" not in output, output)
        assert_true(b"response interrupted" not in output, output)
        assert_true(len(server.requests) == 1, len(server.requests))


def test_input_redraw_bare_escape_still_clears_idle_draft(root, home):
    def verify(_, body):
        user = body["messages"][-1].get("content")
        return event({"content": "bare-escape-ok" if user == "kept" else "bare-escape-bad"})

    with Server([verify]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"discard me", b"discard"),
                ([b"\x1b"] + [b""] * 15, b"> "),
                b"kept\n",
                b"\x04",
            ],
        )
        assert_true(code == 0, output)
        assert_true(b"bare-escape-ok" in output, output)
        assert_true(len(server.requests) == 1, len(server.requests))


def test_input_redraw_history_restores_current_draft(root, home):
    def verify_draft(_, body):
        user = body["messages"][-1].get("content")
        return event({"content": "history-draft-ok" if user == "draft" else "history-draft-bad"})

    with Server([event({"content": "first-ok"}), verify_draft]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"first\n", b"first-ok"),
                b"",  # wait for the next prompt, not merely streamed text
                (b"draft", b"draft"),
                (b"\x1b[A", b"first"),
                b"\x1b[B\n",
                b"\x04",
            ],
        )
        assert_true(code == 0, output)
        assert_true(b"history-draft-ok" in output, output)
        assert_true(len(server.requests) == 2, server.requests)


def test_input_redraw_approval_does_not_pollute_history(root, home):
    def verify_recalled(_, body):
        user = body["messages"][-1].get("content")
        return event({"content": "approval-history-ok" if user == "go" else "approval-history-bad"})

    with Server(
        [
            tool_call("run", {"command": "printf approved"}),
            event({"content": "approval-done"}),
            verify_recalled,
        ]
    ) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"go\n", b"allow run?"),
                (b"y\n", b"approval-done"),
                # The idle status carries the route in schema form and the
                # context window beside what is used.
                (b"", b"test @ 127.0.0.1 \xc2\xb7 ctx"),
                (b"probe", b"probe"),  # input broker is accepting drafts
                b"\x7f" * 5,
                (b"\x1b[A", b"go"),
                (b"\n", b"approval-history-ok"),
                b"\x04",
            ],
        )
        assert_true(code == 0, output)
        assert_true(b"approval-history-ok" in output, output)
        assert_true(b"/16.4K" in output, output)  # used/window, not used alone
        assert_true(len(server.requests) == 3, server.requests)


def test_multiline_run_keeps_action_color(root, home):
    # Large enough to cross the old 2 KiB call-label cap and stdio write
    # boundaries. Every line carries its own cyan SGR so a concurrent composer
    # repaint cannot turn the tail into the terminal default foreground.
    lines = [f"# color-segment-{index:03d}-" + "x" * 24 for index in range(90)]
    lines.append("printf 'done\\n'")
    command = "\n".join(lines)
    with Server([tool_call("run", {"command": command}), event({"content": "color-ok"})]) as server:
        code, output = run_pty(
            root, base_env(home, server.url), [b"go\n", b"/q\n"], args=("--yolo",)
        )
        # Empty SIGCHLD wake slots must not write their marker byte to PTY fd 0.
        assert_true(b"\x01" not in output, output)
        first = b"\x1b[36m\xe2\x86\x92 run\r\n\x1b[36m# color-segment-000"
        assert_true(code == 0 and first in output, output)
        for index in range(90):
            marker = f"\x1b[36m# color-segment-{index:03d}".encode()
            assert_true(marker in output, (index, output))


def test_multiline_rejected_call_shows_arguments(root, home):
    bad = {
        "path": "visible-target",
        "content": "replacement",
        "edits": [{"old": "before", "new": "after", "replace_all": False}],
    }
    with Server([tool_call("edit_file", bad), event({"content": "rejected-ok"})]) as server:
        code, output = run_pty(
            root, base_env(home, server.url), [b"go\n", b"/q\n"], args=("--yolo",)
        )
        assert_true(code == 0 and b"rejected-ok" in output, output)
        assert_true(b"\xe2\x86\x92 edit_file(" in output, output)
        assert_true(b'"path":"visible-target"' in output, output)
        assert_true(b"unknown argument" in output, output)


def test_input_redraw_enter_then_escape_same_packet_interrupts_turn(root, home):
    def delayed(_, __):
        time.sleep(2)
        return event({"content": "too-late"})

    with Server([delayed]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"work\n\x1b", b"Interrupting"), b"/q\n"],
            timeout=8,
        )
        assert_true(code == 0, output)
        assert_true(b"Interrupting" in output, output)
        assert_true(b"too-late" not in output, output)


def test_input_redraw_streaming_tail_survives_resize(root, home):
    def streamed(handler, _):
        write_sse_sequence(
            handler,
            [
                event({"content": "TAIL-BEGIN"}, finish=None),
                event({"content": "-TAIL-END"}),
            ],
            delay=0.4,
        )

    with Server([streamed]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"go\n", b"TAIL-BEGIN"),
                (b"", b"TAIL-END", 36),
                b"/q\n",
            ],
            columns=80,
            timeout=10,
        )
        assert_true(code == 0, output)
        assert_true(b"TAIL-BEGIN-TAIL-END" in output, output)
        assert_true(b"\x1eUAGENT\x1f" not in output, output)
        assert_true(re.search(rb"\x1b\[\d+A\x1b\[J", output) is not None, output)


def test_input_redraw_status_animation_does_not_repaint_draft(root, home):
    def delayed(_, __):
        time.sleep(0.7)
        return event({"content": "status-redraw-ok"})

    with Server([delayed]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"work\n", b"Working"),
                (b"pending draft", b"status-redraw-ok"),
                b"\x15/q\n",
            ],
        )
        assert_true(code == 0, output)
        assert_true(output.count(b"status-redraw-ok") == 1, output)
        response_at = output.index(b"status-redraw-ok")
        assert_true(output[:response_at].count(b"pending draft") == 1, output)


def test_suspend_restores_and_rearms_terminal(root, home):
    # Ctrl+Z stops the process, so it cannot restore anything on the way down:
    # the handler has to hand back a cooked line discipline before the stop and
    # re-arm raw mode on SIGCONT, or the resumed session echoes every keypress.
    seen = {}

    def cooked(slave):
        attributes = termios.tcgetattr(slave)
        attributes[3] |= termios.ICANON | termios.ECHO
        termios.tcsetattr(slave, termios.TCSANOW, attributes)

    def suspend(process, master):
        seen["running"] = termios.tcgetattr(master)[3]
        process.send_signal(signal.SIGTSTP)
        seen["stop_signal"] = wait_until_stopped(process.pid)
        # Cooked by the time it is stopped, and raw again once resumed. The
        # repaint itself stays in the PTY for the harness to collect: reading
        # it here would take those bytes out of the asserted transcript.
        seen["stopped"] = wait_for_echo(master, wanted=True)
        process.send_signal(signal.SIGCONT)
        seen["resumed"] = wait_for_echo(master, wanted=False)

    with Server([event({"content": "unused"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            # An idle SIGINT asks first now, so ending the session is two
            # presses; the second one still leaves through the signal path.
            [
                (lambda process: process.send_signal(signal.SIGINT), b"ctrl+c again to quit"),
                lambda process: process.send_signal(signal.SIGINT),
            ],
            configure_terminal=cooked,
            suspend=suspend,
        )
    # It really suspended, and SIGINT still ends the session afterwards.
    assert_true(seen["stop_signal"] == signal.SIGTSTP, seen)
    assert_true(code == 130, (code, output))
    # Raw while running, cooked while stopped, raw again after fg.
    assert_true(not seen["running"] & termios.ECHO, oct(seen["running"]))
    assert_true(seen["stopped"] & termios.ECHO, oct(seen["stopped"]))
    assert_true(seen["stopped"] & termios.ICANON, oct(seen["stopped"]))
    assert_true(not seen["resumed"] & termios.ECHO, oct(seen["resumed"]))
    # Bracketed paste is retired before the stop and re-armed on resume.
    assert_true(output.count(b"\x1b[?2004l") >= 1, output)
    assert_true(output.count(b"\x1b[?2004h") >= 2, output)


def test_signal_exit_restores_terminal(root, home):
    # A signal exit out of the raw-mode composer must hand back a cooked line
    # discipline too, or the surviving shell has no echo until `stty sane`.
    final = {}

    def cooked(slave):
        attributes = termios.tcgetattr(slave)
        attributes[3] |= termios.ICANON | termios.ECHO
        termios.tcsetattr(slave, termios.TCSANOW, attributes)

    def capture(master):
        final["lflag"] = termios.tcgetattr(master)[3]

    with Server([event({"content": "unused"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (lambda process: process.send_signal(signal.SIGINT), b"ctrl+c again to quit"),
                lambda process: process.send_signal(signal.SIGINT),
            ],
            configure_terminal=cooked,
            after_exit=capture,
        )
        restore = b"\x1b[0m\x1b[39m\x1b[49m"
        assert_true(code == 130, (code, output))
        assert_true(output.rfind(restore) > output.rfind(b"\x1b[48;5;"), output)
        # The composer really did take the terminal (bracketed paste on).
        assert_true(b"\x1b[?2004h" in output, output)
        assert_true(final["lflag"] & termios.ECHO, oct(final["lflag"]))
        assert_true(final["lflag"] & termios.ICANON, oct(final["lflag"]))


def test_input_redraw_survives_terminal_resize_and_delete(root, home):
    original = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    expected = original[:-10] + "XYZ"

    def answer(_, body):
        user = next(
            message.get("content")
            for message in reversed(body["messages"])
            if message.get("role") == "user"
        )
        return event({"content": "resize-ok" if user == expected else "resize-bad"})

    with Server([answer]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (original.encode(), b"", 20),
                b"\x7f" * 10 + b"XYZ\n",
                b"/q\n",
            ],
            columns=80,
        )
        text = output.decode(errors="replace")
        assert_true(code == 0, text)
        assert_true("resize-ok" in text, text)
        assert_true(len(server.requests) == 1, server.requests)


def test_resize_replaces_the_status_row_instead_of_appending(root, home):
    """Repeated resizes must not stack status rows down the scrollback.

    A resize repaints the pinned region, and the status row sits *above* the
    cursor. Erasing only downward leaves it on screen, so every repaint adds
    another copy — the failure this pins. The rule the stream can prove is:
    no status row is ever written without erasing the previous region first.
    """

    def answer(_, __):
        return event({"content": "RESIZE-MARKER"})

    with Server([answer]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"first\n", b"RESIZE-MARKER"),
                (b"", b"", 40),
                (b"", b"", 80),
                (b"", b"", 40),
                b"/q\n",
            ],
            columns=80,
            timeout=15,
        )
        text = output.decode(errors="replace")
        assert_true(code == 0, text)
        assert_true(b"RESIZE-MARKER" in output, text)
        # The region is erased only after walking up to its top row. Erasing
        # downward from the cursor (`\r\x1b[J`) leaves the status row above it
        # on screen, so each repaint would append another copy — the failure
        # this pins. Every resize must produce a walk-up-then-erase.
        assert_true(b"\r\x1b[J" not in output, output[-200:])
        walks = re.findall(rb"\x1b\[\d+A\x1b\[J", output)
        assert_true(len(walks) >= 1, len(walks))
        # Resizes arriving inside the settle window coalesce, so the repaint
        # count is bounded by the number of resizes rather than equal to it.
        assert_true(len(walks) <= 8, len(walks))


def test_context_command_shows_memory_and_skills(root, home):
    workspace = root / "context-workspace"
    workspace.mkdir()
    memory_dir = project_memory_dir(home, workspace)
    memory_dir.mkdir(parents=True)
    (memory_dir / "browser.md").write_text("context-memory-body-sentinel", encoding="utf-8")
    codex = home / ".codex" / "memories"
    codex.mkdir(parents=True)
    (codex / "MEMORY.md").write_text("codex-memory-body-sentinel", encoding="utf-8")
    claude_project = re.sub(r"[^A-Za-z0-9]", "-", str(workspace.resolve()))
    claude = home / ".claude" / "projects" / claude_project / "memory"
    claude.mkdir(parents=True)
    (claude / "MEMORY.md").write_text("claude-memory-body-sentinel", encoding="utf-8")
    global_config = home / ".uagent" / ".config"
    global_config.parent.mkdir(parents=True, exist_ok=True)
    global_config.write_text(
        "UAGENT_WEB_SEARCH_API_KEY=context-secret-sentinel\n"
        "UAGENT_WEB_SEARCH_URL=https://user:pass@search.example/v1\n",
        encoding="utf-8",
    )
    skill = workspace / ".uagent" / "skills" / "context-demo"
    skill.mkdir(parents=True)
    (skill / "SKILL.md").write_text(
        "---\nname: context-demo\ndescription: context-skill-description-sentinel\n"
        "---\n\ncontext-skill-body-sentinel\n",
        encoding="utf-8",
    )
    with Server([event({"content": "unused"})]) as server:
        code, output = run_pty(
            workspace,
            base_env(home, server.url),
            [b"/context\n", b"/memory\n", b"/q\n"],
        )
        assert_true(code == 0, output)
        assert_true(b"Context" in output and b"Skills" in output, output)
        assert_true(b"project/browser" in output, output)
        assert_true(b"codex/MEMORY" in output and b"claude/MEMORY" in output, output)
        assert_true(b"context-memory-body-sentinel" not in output, output)
        assert_true(b"codex-memory-body-sentinel" not in output, output)
        assert_true(b"claude-memory-body-sentinel" not in output, output)
        assert_true(b"context-skill-description-sentinel" in output, output)
        assert_true(b"context-skill-body-sentinel" not in output, output)
        assert_true(b'"name": "memory"' in output, output)
        assert_true(b'"name": "skill"' in output, output)
        assert_true(b'"UAGENT_MODEL": "environment"' in output, output)
        assert_true(b'"web_search_api_key": "<set>"' in output, output)
        assert_true(b"context-secret-sentinel" not in output, output)
        assert_true(b"user:pass" not in output, output)
        assert_true(b"memory on" in output, output)


def test_input_slash_suggestions_and_tab_completion(root, home):
    """Typing a command shows what it could still become; Tab commits it.

    The rows hang below the draft inside the composer's own block, so they are
    erased with it and never reach scrollback.
    """
    with Server([event({"content": "unused"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"/mod", b"/models"),
                (b"\t", b"/model "),
                b"\x15/q\n",
            ],
        )
        assert_true(code == 0, output)
        # Both candidates are offered, with the same description /help prints.
        assert_true(b"/models  search and select across providers" in output, output)
        assert_true(b"/model  choose what model to use" in output, output)
        # Tab commits the shared prefix and, once one row is left, the space
        # its argument needs.
        assert_true(b"/model " in output, output)
        assert_true(not server.get_requests, server.get_requests)


def test_input_shift_enter_keeps_the_draft_open(root, home):
    """Shift+Enter is a newline in the draft; Enter is still the submission."""

    def route(_, body):
        text = json.dumps(body["messages"])
        assert_true("first line\\nsecond line" in text, text)
        return event({"content": "multiline-ok"})

    with Server([route]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"first line\x1b\rsecond line", b"\xe2\x86\xb5"),
                (b"\n", b"multiline-ok"),
                b"/q\n",
            ],
        )
        assert_true(code == 0, output)
        # The draft newline renders as the glyph the echo also uses.
        assert_true(b"first line\xe2\x86\xb5second line" in output, output)
        assert_true(b"multiline-ok" in output, output)


def test_input_ctrl_c_asks_once_then_quits(root, home):
    """An idle SIGINT is half a gesture: the row says so, the second one exits."""
    with Server([event({"content": "unused"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (lambda process: process.send_signal(signal.SIGINT), b"ctrl+c again to quit"),
                lambda process: process.send_signal(signal.SIGINT),
            ],
            timeout=10,
        )
        # The confirmed press leaves through the signal path, so the shell
        # still sees the interrupt status it always saw.
        assert_true(code == 130, (code, output))
        assert_true(b"ctrl+c again to quit" in output, output)
        assert_true(not server.get_requests, server.get_requests)
