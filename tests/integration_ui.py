import json
import re
import signal
import sys
import termios
import threading
import time
import unicodedata

from integration_support import (
    Server,
    assert_true,
    base_env,
    budget,
    event,
    run,
    run_dialog,
    run_pty,
    save_settings,
    saved_settings,
    session_files,
    timeout_setting,
    tool_call,
    tool_calls,
    tool_results,
    wait_for_echo,
    wait_until_stopped,
    write_mcp_server,
    write_session,
    write_sse_sequence,
)
from memory_fixture import project_memory_dir


def test_yolo_toggle_refreshes_approval_state(root, home, *, binary):
    def route(_, body):
        messages = body["messages"]
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
        expected_mode = "yolo" if on_turn else "ask"
        expected_env = "env-on" if on_turn else "env-off"
        # The approval mode is a runtime fact: the latest environment note.
        environment = [
            str(message.get("content", ""))
            for message in messages
            if str(message.get("content", "")).startswith("[environment:")
        ]
        assert_true(environment and f"approval {expected_mode}]" in environment[-1], environment)
        if results:
            assert_true(expected_env in results[-1], results)
            return event({"content": f"{expected_env}-ok"})
        env_value = "yolo" if on_turn else "ask"
        return tool_call(
            "run",
            {"command": (f'test "$UAGENT_APPROVAL" = {env_value} && printf {expected_env}')},
        )

    with Server([route]) as server:
        result = run_dialog(
            root,
            base_env(home, server.url),
            "/yolo\ncheck-on\n/yolo\ncheck-off\ny\n/q\n",
            binary=binary,
        )
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true("env-on-ok" in result.stdout, result.stdout)
        assert_true("env-off-ok" in result.stdout, result.stdout)
        assert_true(result.stdout.count("Allow run?") == 1, result.stdout)
        assert_true(len(server.requests) == 4, server.requests)


def test_plan_toggles_plan_mode_and_asks_to_approve_the_plan(root, home, *, binary):
    """/plan enters plan mode; the plan comes back as a prompt with its
    choices, and approving one leaves plan mode for that mode."""

    def route(_, body):
        text = json.dumps(body["messages"])
        if "The plan is approved" in text:
            return event({"content": "CARRIED-OUT"})
        if tool_results(body["messages"]):
            return event({"content": "PLANNED"})
        plan = re.findall(r"write the plan \(markdown\) to (\S+?);", text)[-1]
        return tool_call("write_file", {"path": plan, "content": "1. Step one\n"})

    with Server([route]) as server:
        result = run_dialog(
            root, base_env(home, server.url), "/plan\nPlan it\nc\n/q\n", binary=binary
        )
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        for shown in (
            "plan mode on",
            "1. Step one",
            "Approve the plan?  [y] Approve and run in auto  [c] Approve and ask before changes",
            "CARRIED-OUT",
        ):
            assert_true(shown in result.stdout, (shown, result.stdout))
        approved = json.dumps(server.requests[-1][1]["messages"])
        assert_true(re.findall(r"; approval (\w+)", approved)[-1] == "ask", approved)


def test_a_start_without_a_provider_says_why(root, home, *, binary):
    # The runtime fails before a terminal has connected; its reason still
    # reaches the terminal instead of only its exit status.
    env = base_env(home, "")
    for name in ("UAGENT_BASE_URL", "UAGENT_MODEL", "OPENROUTER_API_KEY"):
        env.pop(name, None)
    result = run_dialog(root, env, "", binary=binary)
    # Before a client connects it reaches stderr, after one does the screen.
    output = result.stdout + result.stderr
    assert_true(result.returncode == 1, output)
    assert_true("no provider configured — run uagent setup" in output, output)
    assert_true("ended while starting" not in output, output)


def test_command_help(root, home, *, binary):
    with Server([event({"content": "unused"})]) as server:
        result = run_dialog(
            root,
            base_env(home, server.url),
            "/model\n/wat\n/recap\n/modle\n/help\n/exit\n",
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        # Every quit alias detaches locally instead of reaching the runtime.
        assert_true("use conversation controls" not in result.stdout, result.stdout)
        assert_true("unknown command /wat; use /help" in result.stdout, result.stdout)
        assert_true("unknown command /recap; use /help" in result.stdout, result.stdout)
        # A near miss names the command it most likely meant.
        assert_true("unknown command /modle; did you mean /model?" in result.stdout, result.stdout)
        assert_true("commands\n" in result.stdout, result.stdout)
        assert_true("  /attach PATH" in result.stdout, result.stdout)
        assert_true("attach a file to the next turn" in result.stdout, result.stdout)
        assert_true("  /help" in result.stdout, result.stdout)
        assert_true("show this help" in result.stdout, result.stdout)
        assert_true("commands:" not in result.stdout, result.stdout)
        assert_true("use /model QUERY" in result.stdout, result.stdout)
        assert_true(not server.get_requests, server.get_requests)


def test_reasoning_modes_render_consistently(root, home, *, binary):
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
                event({"reasoning": "**Planning phases**\n"}, finish=None),
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
                # Quit once the turn has settled, however slow the build.
                (b"go\n", b"Final answer", b"Ready", None),
                b"/q\n",
            ],
            timeout=10,
            args=("--detail", "full"),
            binary=binary,
        )
        assert_true(code == 0, verbose)
        assert_true(b"\xc2\xb7 Thinking" in verbose, verbose)
        reasoning_style = b"\x1b[0m\x1b[39m\x1b[49m\x1b[90m\x1b[3m"
        assert_true(reasoning_style + b"latest line" in verbose, verbose)
        assert_true(verbose.find(b"first line") < verbose.find(b"Final answer"), verbose)

        # Compact mode selects a complete heading and removes decoration. The
        # heading is short: the row gives the label what its counters leave.
        code, compact = run_pty(
            root,
            env,
            [
                (b"go\n", b"Thinking \xc2\xb7 Planning phases"),
                (b"", b"Final answer", b"Ready", None),
                b"/q\n",
            ],
            timeout=10,
            binary=binary,
        )
        assert_true(code == 0, compact)
        assert_true(b"Thinking \xc2\xb7 Planning phases" in compact, compact)
        assert_true(b"****" not in compact, compact)
        assert_true(b"\xc2\xb7 Thinking" not in compact, compact)


def test_no_color_keeps_bold_italic_and_dim(root, home, *, binary):
    # NO_COLOR removes colour, and only colour: bold, italic and dim are
    # attributes, and inline code keeps the backticks that mark it.
    with Server([event({"content": "use `make` and **strong** *slant*"})]) as server:
        env = base_env(home, server.url)
        env["NO_COLOR"] = "1"
        code, output = run_pty(
            root, env, [(b"go\n", b"slant"), b"/q\n"], startup_marker=b"Ready", binary=binary
        )
    assert_true(code == 0, output[-2000:])
    assert_true(b"`make`" in output, output[-2000:])
    assert_true(b"\x1b[1mstrong" in output and b"\x1b[3mslant" in output, output[-2000:])
    assert_true(b"\x1b[2mReady" in output, output[-2000:])
    assert_true(re.search(rb"\x1b\[(3[0-8]|9[0-7]|4[0-8])m", output) is None, output[-2000:])


def test_non_utf8_locale_draws_only_ascii(root, home, *, binary):
    # Every glyph this program draws has an ASCII fallback: spinner, notices,
    # bullets, tool rows and command replies. The model's text here is ASCII.
    replies = [
        tool_call("run", {"command": "printf 'a\\nb\\nc\\n'"}),
        event({"content": "- item one\n- item two\n\nascii-ok"}),
    ]
    with Server(replies) as server:
        env = base_env(home, server.url)
        env["LC_ALL"] = "C"
        code, output = run_pty(
            root,
            env,
            [
                # The reply's own row, not the echo of what was typed.
                (b"/detail full\n", b"- detail full"),
                (b"/detail default\n", b"- detail default"),
                (b"go\n", b"ascii-ok", b"Ready", None),
                (b"/cost\n", b"total"),
                b"/q\n",
            ],
            args=("--yolo",),
            binary=binary,
        )
    assert_true(code == 0, output[-2000:])
    assert_true(b"* item one" in output, output[-2000:])
    assert_true(re.search(rb"[\x80-\xff]", output) is None, output[-2000:])


def test_keys_typed_ahead_survive_a_redraw(root, home, *, binary):
    # /detail shows the conversation again at the new level. A line that
    # arrived with the command is still sent afterwards.
    with Server([event({"content": "typed-ahead-ok"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"/detail full\ngo\n", b"typed-ahead-ok", b"Ready", None), b"/q\n"],
            binary=binary,
        )
    assert_true(code == 0, output[-2000:])


def test_plain_mode_writes_labelled_lines_without_cursor_control(root, home, *, binary):
    # --plain is for screen readers: every row opens with a spoken label and
    # nothing moves the cursor, erases, animates or leaves ASCII.
    replies = [
        tool_call("run", {"command": "printf plain-ran"}),
        event({"content": "plain-ok"}),
    ]
    with Server(replies) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"go\n", b"approval needed:"), (b"y\n", b"plain-ok"), (b"/q\n", None)],
            args=("--plain",),
            startup_marker=b"",
            binary=binary,
        )
    assert_true(code == 0, output[-2000:])
    text = re.sub(rb"\x1b\[[0-9;]*m", b"", output)
    for label in (
        b"Welcome to uAgent. Try",
        b"you: go",
        b"Risk: runs commands\r\n",
        b"result: Ran ",
        b"uagent:\r\nplain-ok",
    ):
        assert_true(label in text, (label, output[-2000:]))
    assert_true(re.search(rb"\x1b(\[[0-9;?]*[A-Za-ln-z]|\])", output) is None, output[-2000:])
    assert_true(re.search(rb"[\x80-\xff]", output) is None, output[-2000:])


def test_undo_puts_back_what_the_last_turn_changed(root, home, *, binary):
    # /changes lists the turn's files and /undo restores them whole: an edit
    # reverts, a created file goes, a deleted one comes back. A file changed
    # since is kept, and says why.
    (root / "edited.txt").write_text("one\n")
    (root / "gone.txt").write_text("keep me\n")
    (root / "later.txt").write_text("first\n")
    calls = [
        ("call-1", "edit_file", {"path": "edited.txt", "edits": [{"old": "one", "new": "two"}]}),
        ("call-2", "write_file", {"path": "fresh.txt", "content": "new\n"}),
        ("call-3", "delete_file", {"path": "gone.txt"}),
        ("call-4", "write_file", {"path": "later.txt", "content": "second\n", "overwrite": True}),
    ]

    def finish(_, _body):
        # Someone edits later.txt after the tool wrote it.
        (root / "later.txt").write_text("user\n")
        return event({"content": "changed-ok"})

    told = []

    def after(_, body):
        told.append(json.dumps(body["messages"]))
        return event({"content": "after-ok"})

    with Server([tool_calls(calls), finish, after]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"go\n", b"changed-ok"),
                (b"/changes\n", b"not tracked"),
                (b"/undo\n", b"changed since"),
                (b"again\n", b"after-ok"),
                (b"/q\n", None),
            ],
            args=("--yolo", "--plain"),
            startup_marker=b"",
            binary=binary,
        )
    assert_true(code == 0, output[-2000:])
    assert_true(b"edited.txt (+1 -1)" in output, output[-2000:])
    assert_true((root / "edited.txt").read_text() == "one\n", output[-2000:])
    assert_true(not (root / "fresh.txt").exists(), output[-2000:])
    assert_true((root / "gone.txt").read_text() == "keep me\n", output[-2000:])
    assert_true((root / "later.txt").read_text() == "user\n", output[-2000:])
    assert_true(b"kept later.txt: changed since" in output, output[-2000:])
    # The model hears once, at its next step, which files to re-read.
    assert_true("user reverted: edited.txt, fresh.txt, gone.txt;" in told[0], told)


def test_long_diff_reads_the_same_live_and_on_resume(root, home, *, binary):
    content = "".join(f"line {index:04d} of a long file\n" for index in range(2000))
    write = tool_call("write_file", {"path": "long.txt", "content": content + "LAST_DIFF_LINE\n"})
    with Server([write, event({"content": "wrote-ok"})]) as server:
        # Room for the call's arguments, so the turn does not compact.
        env = base_env(home, server.url) | {"UAGENT_CONTEXT": "200000"}
        full = ("--plain", "--detail", "full")
        for args, payload in (
            (("--yolo", *full), [(b"go\n", b"wrote-ok"), (b"/q\n", None)]),
            (("-c", *full), [(b"/q\n", None)]),
        ):
            code, output = run_pty(root, env, payload, args=args, startup_marker=b"", binary=binary)
            assert_true(code == 0, output[-2000:])
            # The headline and the facts' words; this terminal draws ASCII.
            assert_true(b"result: Wrote long.txt - +2001 -0" in output, output[-2000:])
            # Opened, the diff's opening ends where it was cut, and says so.
            assert_true(b"+line 0" in output and b"| ...\r\n" in output, output[-2000:])
            assert_true(b"LAST_DIFF_LINE" not in output, output[-2000:])


def test_multiline_bracketed_paste(root, home, *, binary):
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
            [(paste, b"second"), (b"\n", b"multiline-paste-ok"), b"\x04"],
            columns=24,
            # Paste once the worker is ready, so its status line is drawn
            # before the turn replaces it, however slow the build.
            startup_marker=b"Ready",
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"multiline-paste-ok" in output, output)
        # A 24-column status keeps state, route and approval mode.
        assert_true(b"Ready \xc2\xb7 test \xc2\xb7 Ask\x1b[K" in output, output)
        assert_true(b"\x1b[?2004h" in output and b"\x1b[?2004l" in output, output)
        # The echoed turn is banded to the right edge on every row it spans,
        # and the band is always closed again.
        band = b"\x1b[7m"
        assert_true(output.count(band) >= 2, output)
        assert_true(output.rfind(b"\x1b[0m\x1b[39m\x1b[49m") > output.rfind(band), output)
        assert_true(output.count(band + b"first line\x1b[K\r\n") == 1, output)
        assert_true(output.count(band + b"third line\x1b[K") == 1, output)
        assert_true(b"\x1b[1m> \x1b[0m\x1b[39m\x1b[49m" in output, output)
        assert_true(len(server.requests) == 1, len(server.requests))


def test_enter_arriving_with_paste_does_not_submit(root, home, *, binary):
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
                (b"\n", b"paste-enter-ok"),
                b"\x04",
            ],
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"paste-enter-ok" in output, output)
        assert_true(len(server.requests) == 1, server.requests)


def test_oversized_paste_says_why_it_was_not_inserted(root, home, *, binary):
    with Server([event({"content": "after-paste-ok"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"\x1b[200~" + b"x" * (70 * 1024) + b"\x1b[201~", b"paste not inserted"),
                # The next key clears the note; the draft stayed usable.
                (b"hi\n", b"after-paste-ok"),
                b"\x04",
            ],
            startup_marker=b"Ready",
            binary=binary,
        )
        assert_true(code == 0, output[-2000:])
        assert_true(b"\a" in output, output[-2000:])
        assert_true(b"xxxx" not in output, output[-2000:])


def test_resume_picker_accepts_enter_when_icrnl_was_disabled(root, home, *, binary):
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
            [(b"1\r", b"Ready"), b"/q\r"],
            args=("--resume",),
            startup_marker=b"resume #: ",
            configure_terminal=disable_icrnl,
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"Ready" in output, output)
        assert_true(b"ctx 1.9M" not in output, output)
        assert_true(b"^M" not in output, output)
        assert_true(len(server.requests) == 0, server.requests)


def test_model_picker_lists_the_models_before_asking(root, home, *, binary):
    models = {"data": [{"id": "alpha-one"}, {"id": "alpha-two"}]}
    with Server([event({"content": "unused"})], get_response=models) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"/model alpha\r", b"model # "), (b"2\r", b"model alpha-two"), b"/q\r"],
            startup_marker=b"Ready",
            binary=binary,
        )
        assert_true(code == 0, output)
        # The command is still running when it asks: the list it asks about
        # comes with the question, not with the command's result.
        asked = output.index(b"model # ")
        assert_true(b"[2]   alpha-two" in output[:asked], output)


def test_input_redraw_focus_switch_preserves_multiline_draft(root, home, *, binary):
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
            [(input_fragments, b"third"), (b"\n", b"focus-draft-ok"), b"\x04"],
            columns=24,
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"focus-draft-ok" in output, output)
        assert_true(b"[Ifirst" not in output and b"[Ofirst" not in output, output)
        assert_true(b"response interrupted" not in output, output)
        assert_true(len(server.requests) == 1, len(server.requests))


def test_input_redraw_bare_escape_still_clears_idle_draft(root, home, *, binary):
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
                (b"kept\n", b"bare-escape-ok"),
                b"\x04",
            ],
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"bare-escape-ok" in output, output)
        assert_true(len(server.requests) == 1, len(server.requests))


def test_input_redraw_history_restores_current_draft(root, home, *, binary):
    def verify_draft(_, body):
        user = body["messages"][-1].get("content")
        return event({"content": "history-draft-ok" if user == "draft" else "history-draft-bad"})

    with Server([event({"content": "first-ok"}), verify_draft]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"first\n", b"first-ok", b"Ready", None),
                (b"draft", b"draft"),
                (b"\x1b[A", b"first"),
                (b"\x1b[B\n", b"history-draft-ok"),
                b"\x04",
            ],
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"history-draft-ok" in output, output)
        assert_true(len(server.requests) == 2, server.requests)


def test_input_readline_control_keys(root, home, *, binary):
    """Ctrl+W deletes a word, Ctrl+P/Ctrl+N walk history, Ctrl+F moves right."""

    def echo(_, body):
        return event({"content": "got<" + body["messages"][-1].get("content") + ">"})

    with Server([echo]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"alpha beta\x17gamma\n", b"got<", b"Ready", None),
                (b"\x10\x01\x06X\n", b"got<aX", b"Ready", None),
                (b"draft\x10\x0e!\n", b"got<draft!>", b"Ready", None),
                b"\x04",
            ],
            binary=binary,
        )
        assert_true(code == 0, output[-2000:])
        sent = [body["messages"][-1].get("content") for _, body in server.requests]
        assert_true(sent == ["alpha gamma", "aXlpha gamma", "draft!"], sent)


def test_input_redraw_approval_does_not_pollute_history(root, home, *, binary):
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
                (b"go\n", b"Allow run?"),
                # The idle status carries the route in schema form, then the
                # approval mode, and the context window beside what is used.
                # The answer may arrive in the same frame.
                (b"y\n", b"Ready \xc2\xb7 test \xc2\xb7 Ask \xc2\xb7 ", b"est. ctx", None),
                (b"probe", b"probe"),  # input broker is accepting drafts
                b"\x7f" * 5,
                (b"\x1b[A", b"go"),
                (b"\n", b"approval-history-ok"),
                b"\x04",
            ],
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"approval-done" in output, output)
        assert_true(b"approval-history-ok" in output, output)
        assert_true(b"/16.4k" in output, output)  # used/window, not used alone
        assert_true(len(server.requests) == 3, server.requests)


def test_multiline_run_reads_as_its_first_line(root, home, *, binary):
    # Large enough to cross stdio write boundaries: a closed call is one
    # line, its command's first.
    lines = [f"# color-segment-{index:03d}-" + "x" * 24 for index in range(90)]
    lines.append("printf 'done\\n'")
    command = "\n".join(lines)
    with Server([tool_call("run", {"command": command}), event({"content": "color-ok"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"go\n", b"color-ok"), b"/q\n"],
            args=("--yolo",),
            binary=binary,
        )
        # Empty SIGCHLD wake slots must not write their marker byte to PTY fd 0.
        assert_true(b"\x01" not in output, output)
        first = b"\xc2\xb7 Ran # color-segment-000-" + b"x" * 24 + b" \xc2\xb7 "
        assert_true(code == 0 and first in output, output)
        assert_true(b"color-segment-089" not in output, output)


def test_tool_output_drops_its_own_colours(root, home, *, binary):
    # A command's colouring is dropped, not spelled out as \x1b[31m text.
    command = "printf '\\033[31mred-first\\033[0m\\nplain\\n\\033[1;32mgreen-last\\033[m\\n'"
    with Server(
        [tool_call("run", {"command": command}), event({"content": "colour-ok"})] * 2
    ) as server:
        for verbose in (False, True):
            code, output = run_pty(
                root,
                base_env(home, server.url),
                [(b"go\n", b"colour-ok", b"Ready", None), b"/q\n"],
                args=("--yolo", *(("--detail", "full") * verbose)),
                binary=binary,
            )
            assert_true(code == 0, output[-2000:])
            assert_true(b"red-first" in output and b"green-last" in output, output[-2000:])
            assert_true(b"\\x1b" not in output, output[-2000:])


def test_rejected_call_reads_as_its_summary(root, home, *, binary):
    bad = {
        "path": "visible-target",
        "content": "replacement",
        "edits": [{"old": "before", "new": "after", "replace_all": False}],
    }
    with Server([tool_call("edit_file", bad), event({"content": "rejected-ok"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"go\n", b"rejected-ok"), b"/q\n"],
            args=("--yolo",),
            binary=binary,
        )
        assert_true(code == 0 and b"rejected-ok" in output, output)
        # A refused call reads as what it would have done, not as raw JSON;
        # the error beneath says why.
        assert_true(b"visible-target" in output, output)
        assert_true(b'"path":"visible-target"' not in output, output)
        assert_true(b"unknown argument" in output, output)


def test_input_redraw_enter_then_escape_same_packet_interrupts_turn(root, home, *, binary):
    finished = threading.Event()

    def delayed(_, __):
        finished.wait(budget(8))
        return event({"content": "too-late"})

    with Server([delayed]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"work\n\x1b", b"\xc2\xb7 interrupted"), b"/q\n"],
            timeout=8,
            binary=binary,
        )
        finished.set()
        assert_true(code == 0, output)
        assert_true(b"\xc2\xb7 interrupted" in output, output)
        assert_true(b"too-late" not in output, output)


def test_input_redraw_streaming_tail_survives_resize(root, home, *, binary):
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
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"TAIL-BEGIN-TAIL-END" in output, output)
        assert_true(b"\x1eUAGENT\x1f" not in output, output)
        assert_true(re.search(rb"\x1b\[\d+A\x1b\[J", output) is not None, output)


class Screen:
    """Just enough VT100 to replay the client: autowrap with the deferred wrap
    at the last column, wide glyphs, cursor motion, erase, bold, scrollback."""

    def __init__(self, columns, rows=24):
        self.columns, self.rows = columns, rows
        self.lines = [[] for _ in range(rows)]  # scrollback, then the screen
        self.wrapped = set()  # rows that continue on the next one
        self.y = self.x = 0  # x == columns is the deferred wrap
        self.bold = False

    def feed(self, data):
        text = data.decode(errors="replace")
        at = 0
        while at < len(text):
            char = text[at]
            at += 1
            if char == "\x1b":
                match = re.compile(r"\[([?\d;]*)([@-~])|[^\[]").match(text, at)
                at = match.end() if match else at
                if match and match.group(2):
                    self.control(match.group(1), match.group(2))
            elif char == "\r":
                self.x = 0
            elif char == "\n":
                self.linefeed()
            elif char >= " ":
                self.put(char, 2 if unicodedata.east_asian_width(char) in "WF" else 1)

    def linefeed(self):
        self.y += 1
        if self.y == len(self.lines):
            self.lines.append([])

    def put(self, char, width):
        if self.x + width > self.columns:
            self.wrapped.add(self.y)
            self.x = 0
            self.linefeed()
        line = self.lines[self.y]
        line.extend([(" ", False)] * (self.x + width - len(line)))
        line[self.x : self.x + width] = [(char, self.bold)] + [("", self.bold)] * (width - 1)
        self.x += width

    def control(self, params, final):
        count = int(params) if params.isdigit() else 1
        top = len(self.lines) - self.rows
        if final == "A":
            self.y = max(top, self.y - count)
        elif final == "B":
            self.y = min(len(self.lines) - 1, self.y + count)
        elif final == "C":
            self.x = self.x + count
        elif final == "H":
            self.y, self.x = top, 0
        elif final in "JK" and params in ("", "0", "2"):
            if params == "2":
                self.lines[self.y] = []
            del self.lines[self.y][self.x :]
            last = len(self.lines) if final == "J" else self.y + 1
            first = top if final == "J" and params == "2" else self.y
            for row in range(first, last):
                self.wrapped.discard(row)
                if row != self.y:
                    self.lines[row] = []
        elif final == "m":
            for code in (params or "0").split(";"):
                if code in ("1", "22", "0"):
                    self.bold = code == "1"
        self.x = min(self.x, self.columns - 1) if final in "ABC" else self.x

    def text(self):
        return "".join(
            "".join(char for char, _ in line) + ("" if row in self.wrapped else "\n")
            for row, line in enumerate(self.lines)
        )


def test_streaming_paragraph_taller_than_the_screen(root, home, *, binary):
    """A paragraph taller than the screen streams without duplicating itself.

    Each repaint used to erase and rewrite the whole unfinished paragraph:
    bytes per token grew with its length, and once it outgrew the screen the
    erase could not reach its top, so scrollback filled with copies. Rows the
    live region cannot hold go to scrollback once; bold a row opened carries
    over to the next.
    """
    words = [f"w{index:03d}abc" + ("界" if index % 7 == 3 else "") for index in range(250)]
    bold = range(100, 130)
    tokens = [
        ("**" if index == bold.start else "")
        + word
        + ("**" if index == bold.stop - 1 else "")
        + " "
        for index, word in enumerate(words)
    ]

    def streamed(handler, _):
        write_sse_sequence(
            handler,
            [event({"content": token}, finish=None) for token in tokens] + [event({})],
            delay=0.015,
        )

    with Server([streamed]) as server:
        env = base_env(home, server.url)
        # One request streams for about four seconds, near the usual budget.
        env["UAGENT_REQUEST_TIMEOUT"] = timeout_setting(60)
        code, output = run_pty(
            root,
            env,
            [(b"go\n", b"w249", b"Ready", None), b"/q\n"],
            timeout=20,
            binary=binary,
        )
    assert_true(code == 0, output[-2000:])
    screen = Screen(80)
    screen.feed(output)
    shown = screen.text().replace("\n", "")  # rows break mid-word
    for word in words:
        assert_true(len(re.findall(word + r"(?!\d)", shown)) == 1, (word, shown))
    for line in screen.lines:
        row = "".join(char for char, _ in line)
        for match in re.finditer(r"w(\d{3})", row):
            number = int(match.group(1))
            assert_true(line[match.start()][1] == (number in bold), (number, row))
    streamed_bytes = output.rfind(b"w249") - output.find(b"w000")
    per_token = streamed_bytes / len(tokens)
    print(f"  {per_token:.0f} bytes per token", flush=True)
    assert_true(per_token < 400, per_token)


def test_streaming_code_spans_cost_what_plain_text_costs(root, home, *, binary):
    """Inline code streams for about the bytes plain text does.

    A redrawn row replayed every style the paragraph had used before it, so
    bytes per token grew with the number of code spans streamed so far.
    """
    tokens = [(f"`c{index:03d}` " if index % 3 == 0 else f"w{index:03d} ") for index in range(600)]

    def streamed(handler, _):
        write_sse_sequence(
            handler,
            [event({"content": token}, finish=None) for token in tokens] + [event({})],
            delay=0.005,
        )

    with Server([streamed]) as server:
        env = base_env(home, server.url)
        env["UAGENT_REQUEST_TIMEOUT"] = timeout_setting(60)
        code, output = run_pty(
            root,
            env,
            [(b"go\n", b"w599", b"Ready", None), b"/q\n"],
            timeout=20,
            binary=binary,
        )
    assert_true(code == 0, output[-2000:])
    streamed_bytes = output.rfind(b"w599") - output.find(b"c000")
    per_token = streamed_bytes / len(tokens)
    print(f"  {per_token:.0f} bytes per token", flush=True)
    assert_true(per_token < 400, per_token)


def test_streaming_paints_at_most_once_a_frame(root, home, *, binary):
    """However fast tokens arrive, the terminal repaints at a frame rate."""
    tokens = [f"t{index:04d} " for index in range(2000)]
    span = []

    def streamed(handler, _):
        started = time.monotonic()
        write_sse_sequence(
            handler,
            [event({"content": token}, finish=None) for token in tokens] + [event({})],
            delay=0.001,
        )
        span.append(time.monotonic() - started)

    with Server([streamed]) as server:
        env = base_env(home, server.url)
        env["UAGENT_REQUEST_TIMEOUT"] = timeout_setting(60)
        code, output = run_pty(
            root,
            env,
            [(b"go\n", b"t1999", b"Ready", None), b"/q\n"],
            timeout=30,
            binary=binary,
        )
    assert_true(code == 0, output[-2000:])
    streaming = output[output.find(b"t0000") : output.rfind(b"t1999")]
    paints = streaming.count(b"\x1b[?2026h")
    # A frame is 16 ms: at most 62.5 paints a second, plus the edges.
    print(f"  {paints} paints in {span[0]:.1f} s", flush=True)
    assert_true(paints < 64 * span[0] + 8, (paints, span[0]))


def test_live_region_repaints_in_one_synchronized_write(root, home, *, binary):
    # Every walk back up the live region, and every erase, is inside one
    # synchronized update: a terminal never shows an erase without its redraw.
    def streamed(handler, _):
        write_sse_sequence(
            handler,
            [event({"content": f"part{index} "}, finish=None) for index in range(40)]
            + [event({"content": "\n\nsync-ok"})],
            delay=0.01,
        )

    with Server([streamed]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"go\n", b"sync-ok", b"Ready", None), (b"", b"", 50), b"/q\n"],
            binary=binary,
        )
    assert_true(code == 0, output[-2000:])
    frames = re.findall(rb"\x1b\[\?2026h(.*?)\x1b\[\?2026l", output, re.S)
    assert_true(len(frames) > 10, len(frames))
    outside = re.sub(rb"\x1b\[\?2026h.*?\x1b\[\?2026l", b"", output, flags=re.S)
    assert_true(re.search(rb"\x1b\[\d*A|\x1b\[J", outside) is None, outside[-2000:])


def test_row_exactly_as_wide_as_the_terminal(root, home, *, binary):
    # A row that fills the last column leaves the cursor waiting to wrap; the
    # rows after it must still land on their own rows.
    row = "x" * 79 + "|"
    with Server([event({"content": row + " next-row-ok"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"go\n", b"next-row-ok", b"Ready", None), b"/q\n"],
            binary=binary,
        )
    assert_true(code == 0, output[-2000:])
    screen = Screen(80)
    screen.feed(output)
    lines = ["".join(char for char, _ in line).rstrip() for line in screen.lines]
    at = lines.index(row)
    assert_true(lines[at + 1].strip().startswith("next-row-ok"), lines[at - 2 : at + 4])


def test_resize_while_the_ask_picker_is_open(root, home, *, binary):
    question = {
        "question": "Which database?",
        "header": "Database",
        "options": [{"label": "SQLite"}, {"label": "Postgres"}],
    }

    def route(_, body):
        if not any(message.get("role") == "tool" for message in body["messages"]):
            return tool_call("ask", {"questions": [question]})
        return event({"content": "picked-ok"})

    with Server([route]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"pick\n", b"Esc cancel"),
                # The picker is drawn again at the new width, not appended.
                (b"", b"Which database?", 40),
                (b"\r", b"picked-ok"),
                b"/q\n",
            ],
            binary=binary,
            timeout=20,
        )
    assert_true(code == 0, output[-2000:])
    # The whole stream: a byte window can start inside an escape sequence
    # and miss the answer when a platform repaints more.
    screen = Screen(40)
    screen.feed(output)
    shown = screen.text()
    assert_true(shown.count("Which database? → SQLite") == 1, shown)


def test_rows_written_over_the_composer_survive_the_next_repaint(root, home, *, binary):
    """A picker's answer and a decision's prompt stay in scrollback.

    Both replace the composer's rows for a while; once the composer is back,
    no repaint may treat their rows as its own.
    """
    question = {
        "question": "Which database?",
        "header": "Database",
        "options": [{"label": "SQLite"}, {"label": "Postgres"}],
    }

    def asked(handler, _):
        write_sse_sequence(
            handler,
            [
                event({"content": "asking-now"}, finish=None),
                tool_call("ask", {"questions": [question]}),
            ],
        )

    def streamed(handler, _):
        write_sse_sequence(
            handler,
            [event({"content": f"more{index} "}, finish=None) for index in range(5)]
            + [event({"content": "after-ok"})],
            delay=0.05,
        )

    with Server(
        [asked, streamed, tool_call("run", {"command": "printf ran"}), event({"content": "ran-ok"})]
    ) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"pick\n", b"Esc cancel"),
                (b"\r", b"after-ok", b"Ready", None),
                (b"run\n", b"Allow run?"),
                (b"y\n", b"ran-ok", b"Ready", None),
                b"/q\n",
            ],
            binary=binary,
            timeout=20,
        )
    assert_true(code == 0, output[-2000:])
    screen = Screen(80)
    screen.feed(output)
    shown = screen.text()
    assert_true(shown.count("Which database? → SQLite") == 1, shown)
    assert_true(shown.count("Allow run?") == 1, shown)


def test_input_redraw_status_animation_does_not_repaint_draft(root, home, *, binary):
    def delayed(_, __):
        time.sleep(0.7)
        return event({"content": "status-redraw-ok"})

    with Server([delayed]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                # Not "Working": a label held for less than a frame is
                # never drawn.
                (b"work\n", b"Waiting for model"),
                (b"pending draft", b"status-redraw-ok"),
                b"\x15/q\n",
            ],
            startup_marker=b"Ready",
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(output.count(b"status-redraw-ok") == 1, output)
        # Real response output starts with its actor header; its pipe write
        # may arrive separately from the text and legitimately repaint input.
        # The welcome names the program too: the header is the one after
        # the draft.
        response_at = output.index(b"uagent", output.index(b"pending draft"))
        assert_true(output[:response_at].count(b"pending draft") == 1, output)


def test_suspend_restores_and_rearms_terminal(root, home, *, binary):
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
            # presses; the second one detaches with the signal exit status.
            [
                (lambda process: process.send_signal(signal.SIGINT), b"Ctrl+C again to detach"),
                lambda process: process.send_signal(signal.SIGINT),
            ],
            configure_terminal=cooked,
            suspend=suspend,
            # run_pty runs one deadline across the whole interaction, and the
            # suspend handshake here spends it before a key is typed:
            # wait_until_stopped plus two wait_for_echo calls each scale to the
            # default on their own. Under a loaded sanitizer run the payload
            # phase then ran out and the child was killed, which reads as a
            # wrong exit code rather than the timeout it is.
            timeout=30,
            binary=binary,
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


def test_signal_exit_restores_terminal(root, home, *, binary):
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
                (lambda process: process.send_signal(signal.SIGINT), b"Ctrl+C again to detach"),
                lambda process: process.send_signal(signal.SIGINT),
            ],
            configure_terminal=cooked,
            after_exit=capture,
            binary=binary,
        )
        restore = b"\x1b[0m\x1b[39m\x1b[49m"
        assert_true(code == 130, (code, output))
        assert_true(output.rfind(restore) > output.rfind(b"\x1b[48;5;"), output)
        # The composer really did take the terminal (bracketed paste on).
        assert_true(b"\x1b[?2004h" in output, output)
        assert_true(final["lflag"] & termios.ECHO, oct(final["lflag"]))
        assert_true(final["lflag"] & termios.ICANON, oct(final["lflag"]))
        # An idle SIGINT is half a gesture; neither press reaches the model.
        assert_true(not server.get_requests, server.get_requests)


def test_input_redraw_survives_terminal_resize_and_delete(root, home, *, binary):
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
                (b"\x7f" * 10 + b"XYZ\n", b"resize-ok"),
                b"/q\n",
            ],
            columns=80,
            binary=binary,
        )
        text = output.decode(errors="replace")
        assert_true(code == 0, text)
        assert_true("resize-ok" in text, text)
        assert_true(len(server.requests) == 1, server.requests)


def test_input_redraw_backspaces_across_a_wide_glyph_wrap(root, home, *, binary):
    """A wrap boundary that falls inside double-width text.

    The composer maps the cursor to a row by subtracting each row's display
    width, so a row that ends one column short of the terminal -- which is
    what a two-column glyph that will not fit forces -- is where that
    arithmetic and the byte offsets disagree. WrapLines is unit-tested on
    wide text; this is the caret walking back over it.
    """
    # 74 columns of ASCII plus three wide glyphs, against 77 usable columns:
    # the first row takes one glyph and stops at 76, the rest wrap.
    original = "a" * 74 + "\u65e5\u672c\u8a9e"
    expected = "a" * 74 + "\u65e5" + "ok"

    def answer(_, body):
        user = next(
            message.get("content")
            for message in reversed(body["messages"])
            if message.get("role") == "user"
        )
        return event({"content": "wide-ok" if user == expected else f"wide-bad:{user!r}"})

    with Server([answer]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [original.encode(), (b"\x7f" * 2 + b"ok\n", b"wide-ok"), b"/q\n"],
            columns=80,
            binary=binary,
        )
        text = output.decode(errors="replace")
        assert_true(code == 0, text)
        assert_true("wide-ok" in text, text)


def test_resize_replaces_the_status_row_instead_of_appending(root, home, *, binary):
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
                (b"first\n", b"RESIZE-MARKER", b"Ready", None),
                (b"", b"", 40),
                (b"", b"", 80),
                (b"", b"", 40),
                b"/q\n",
            ],
            columns=80,
            timeout=15,
            binary=binary,
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


def test_context_command_shows_memory_and_skills(root, home, *, binary):
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
    save_settings(
        home,
        binary=binary,
        OPENROUTER_API_KEY="context-secret-sentinel",
        UAGENT_BASE_URL="https://user:pass@review.example/v1",
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
            base_env(home, server.url) | {"UAGENT_OTHER_AGENTS": "claude,codex"},
            [
                (b"/context\n", b"context-skill-description-sentinel", b"\x1b[1m> \x1b[0m", None),
                (b"/memory\n", b"project/browser", b"\x1b[1m> \x1b[0m", None),
                b"/q\n",
            ],
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"model request" in output and b'"tools"' in output, output)
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
        assert_true(b"context-secret-sentinel" not in output, output)
        assert_true(b"user:pass" not in output, output)
        assert_true(b'"enabled": true' in output, output)
        assert_true(not server.requests, server.requests)


def test_input_slash_suggestions_and_tab_completion(root, home, *, binary):
    """Typing a command shows what it could still become; Tab commits it.

    The rows hang below the draft inside the composer's own block, so they are
    erased with it and never reach scrollback.
    """
    with Server([event({"content": "unused"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"/con", b"/context"),
                # Unchanged rows are not redrawn: wait for the draft's own.
                (b"\t", b"\x1b[49m/con"),
                b"\x15/q\n",
            ],
            binary=binary,
        )
        assert_true(code == 0, output)
        # Both candidates are offered, with the same description /help prints.
        assert_true(b"/config  show changed settings" in output, output)
        assert_true(b"/context  show current model request" in output, output)
        # Tab commits the shared prefix.
        assert_true(b"/con" in output, output)
        assert_true(not server.get_requests, server.get_requests)


def test_input_slash_menu_moves_with_arrows(root, home, *, binary):
    """Arrows walk the command menu; Tab and Enter take the highlighted row.

    Scattered letters still find a command, and Enter on a partial name only
    completes it, so a half-typed command never reaches the runtime.
    """
    with Server([event({"content": "unused"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"/con", b"> /context  show"),
                (b"\x1b[B", b"> /config  show"),
                (b"\x1b[A", b"> /context  show"),
                (b"\x1b[B\r", b"\x1b[49m/config "),
                (b"\x15/cnfg", b"> /config  show"),
                (b"\t", b"\x1b[49m/config "),
                b"\x15/q\n",
            ],
            binary=binary,
        )
        assert_true(code == 0, output)
        # Neither Enter nor Tab submitted the completed command.
        assert_true(not server.get_requests, server.get_requests)


def test_status_row_names_mode_and_turn_keys(root, home, *, binary):
    """The idle row names the approval mode; a running turn names its keys."""

    shown = threading.Event()

    def delayed(_, __):
        shown.wait(budget(8))
        return event({"content": "status-keys-ok"})

    with Server([delayed]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"work\n", b"Esc stop \xc2\xb7 Ctrl+B background"),
                (lambda _: shown.set(), b"status-keys-ok", b"Ready", None),
                b"/q\n",
            ],
            columns=160,
            startup_marker=b"Ready",
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(re.search(rb"Ready \xc2\xb7 \S+ \xc2\xb7 Ask", output), output)


def test_input_at_path_suggestions_and_tab_completion(root, home, *, binary):
    """`@` completes a path a segment at a time, the way a shell does.

    Naming a file costs the draft a few keystrokes; describing one costs the
    agent a grep and a read round to find what was meant. The rows are the same
    block the slash suggestions use, so they are erased with the draft.
    """
    (root / "alpha").mkdir()
    (root / "alpha" / "inner.txt").write_text("x", encoding="utf-8")
    (root / "alphabet.txt").write_text("x", encoding="utf-8")
    (root / "beta.txt").write_text("x", encoding="utf-8")
    (root / ".hidden.txt").write_text("x", encoding="utf-8")

    def answer(_, body):
        user = next(
            message.get("content")
            for message in reversed(body["messages"])
            if message.get("role") == "user"
        )
        return event(
            {"content": "path-ok" if user == "read @alpha/inner.txt" else f"path-bad:{user!r}"}
        )

    with Server([answer]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                # Two entries share the prefix, so Tab commits only "alpha".
                (b"read @alph", b"@alphabet.txt"),
                (b"\t", b"@alpha"),
                # Descending into the directory re-lists from inside it.
                (b"/", b"@alpha/inner.txt"),
                (b"\t\n", b"path-ok"),
                b"/q\n",
            ],
            binary=binary,
        )
        assert_true(code == 0, output)
        # Both candidates are offered; a dotfile nobody asked for is not.
        assert_true(b"@alpha/" in output, output)
        assert_true(b"@alphabet.txt" in output, output)
        assert_true(b".hidden.txt" not in output, output)
        # An unrelated sibling is never a candidate for this prefix.
        assert_true(b"beta.txt" not in output, output)
        assert_true(b"path-ok" in output, output)


def test_input_ctrl_x_ctrl_e_round_trips_through_an_editor(root, home, *, binary):
    """Ctrl+X Ctrl+E hands the draft to $EDITOR and takes back what it saved.

    The terminal has to come back cooked for the editor and raw for the
    composer afterwards, which is the part that strands a session when it is
    wrong -- so the case keeps typing after the round trip.
    """
    editor = root / "fake-editor.sh"
    # Appends rather than replaces, so the assertion proves both directions:
    # the draft reached the file and the file came back.
    editor.write_text('#!/bin/sh\nprintf \'%s\' "$(cat "$1")-edited" > "$1"\n', encoding="utf-8")
    editor.chmod(0o755)

    def answer(_, body):
        user = next(
            message.get("content")
            for message in reversed(body["messages"])
            if message.get("role") == "user"
        )
        return event(
            {"content": "editor-ok" if user == "draft-edited!" else f"editor-bad:{user!r}"}
        )

    with Server([answer]) as server:
        env = base_env(home, server.url)
        env["EDITOR"] = str(editor)
        code, output = run_pty(
            root,
            env,
            # Ctrl+X Ctrl+E, then keep typing: the composer must be raw again.
            [(b"draft\x18\x05", b"draft-edited"), (b"!\n", b"editor-ok"), b"/q\n"],
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"editor-ok" in output, output)


def test_input_shift_enter_keeps_the_draft_open(root, home, *, binary):
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
            binary=binary,
        )
        assert_true(code == 0, output)
        # The draft newline renders as the glyph the echo also uses.
        assert_true(b"first line\xe2\x86\xb5second line" in output, output)
        assert_true(b"multiline-ok" in output, output)


def test_cli_permissions_config_http_and_fork(root, home, *, binary):
    with Server([lambda _, _body: event({"content": "CLI capture proof"})]) as server:
        result = run_dialog(
            root,
            base_env(home, server.url),
            "/permissions yolo\n/config user UAGENT_MAX_STEPS=23\nFirst CLI turn\n/http\n/http 1 response\n/fork CLI fork\nSecond CLI turn\n/q\n",
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(
            '"effective": "yolo"' in result.stdout and '"forked": true' in result.stdout,
            result.stdout,
        )
        assert_true("data: [DONE]" in result.stdout and '"tools"' in result.stdout, result.stdout)
        assert_true(len(server.requests) == 2, server.requests)
        assert_true("First CLI turn" in json.dumps(server.requests[1][1]), "fork lost CLI history")
        files = session_files(home)
        assert_true(len(files) == 2, files)
        original = next(path for path in files if not path.name.startswith("fork-"))
        fork = next(path for path in files if path.name.startswith("fork-"))
        assert_true(
            "Second CLI turn" not in original.read_text() and "Second CLI turn" in fork.read_text(),
            "fork modified source",
        )
        assert_true(
            saved_settings(home).get("limits.maxSteps") == 23,
            "CLI config not persisted",
        )


def test_cli_btw_opens_tool_capable_conversation_with_followups(root, home, *, binary):
    (root / "btw-cli.txt").write_text("CLI-SIDE-PROOF")

    def answer(_, body):
        messages = body["messages"]
        last = messages[-1]
        if last.get("content") == "CLI parent":
            return event({"content": "CLI-PARENT-DONE"})
        if last.get("content") == "CLI side followup":
            assert_true(any(m.get("content") == "CLI-SIDE-DONE" for m in messages), messages)
            return event({"content": "CLI-FOLLOWUP-DONE"})
        if last["role"] == "tool":
            assert_true("CLI-SIDE-PROOF" in str(last.get("content")), last)
            return event({"content": "CLI-SIDE-DONE"})
        return tool_call("read_path", {"path": "btw-cli.txt"}, call_id="cli-side-read")

    with Server([answer]) as server:
        result = run_dialog(
            root,
            base_env(home, server.url),
            "CLI parent\n/btw CLI side question\nCLI side followup\n/q\n",
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("CLI-FOLLOWUP-DONE" in result.stdout, result.stdout)
        files = session_files(home)
        original = next(path for path in files if not path.name.startswith("fork-"))
        side = next(path for path in files if path.name.startswith("fork-"))
        assert_true("CLI-SIDE-DONE" not in original.read_text(), original)
        assert_true(
            "CLI-SIDE-DONE" in side.read_text() and "CLI side followup" in side.read_text(), side
        )


def test_cli_fork_does_not_inherit_remembered_approvals(root, home, *, binary):
    def answer(_, body):
        messages = body["messages"]
        start = max(
            index for index, message in enumerate(messages) if message.get("role") == "user"
        )
        if any(message.get("role") == "tool" for message in messages[start:]):
            return event({"content": "Approval handled"})
        name = "first.txt" if messages[start]["content"] == "First approval" else "second.txt"
        return tool_call("write_file", {"path": name, "content": "Scoped approval"}, call_id=name)

    with Server([answer]) as server:
        result = run_dialog(
            root,
            base_env(home, server.url),
            "First approval\ns\n/fork\nSecond approval\nn\n/q\n",
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.count("Allow write_file?") == 2, result.stdout)
        assert_true(
            (root / "first.txt").exists() and not (root / "second.txt").exists(),
            "fork inherited an approval grant",
        )


def test_instructions_editor_reaches_new_sessions(root, home, *, binary):
    editor = root / "instructions-editor.sh"
    editor.write_text('#!/bin/sh\nprintf "Only editor behavior.\\nPreserve newlines.\\n" > "$1"\n')
    editor.chmod(0o755)
    env = base_env(home, "http://127.0.0.1:9/v1")
    env["VISUAL"] = str(editor)
    code, output = run_pty(
        root,
        env,
        [(b"/instructions edit user\n", b"restarted sessions read it"), b"/q\n"],
        binary=binary,
    )
    assert_true(code == 0, output)
    saved = (home / ".uagent" / "AGENTS.md").read_text()
    assert_true(saved == "Only editor behavior.\nPreserve newlines.\n", saved)

    def answer(_, body):
        prompt = body["messages"][0]["content"]
        # Additive: the base stays, the instructions follow it.
        assert_true("Read only what the task needs" in prompt, prompt)
        assert_true("Only editor behavior.\nPreserve newlines." in prompt, prompt)
        return event({"content": "instructions-ok"})

    with Server([answer]) as server:
        result = run(root, base_env(home, server.url), "-p", "reply", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "instructions-ok", result.stdout)


def test_cli_mcp_config_and_restart(root, home, *, binary):
    fake = home / "fake_mcp.py"
    write_mcp_server(
        fake,
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], 'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'echo', 'inputSchema': {'type': 'object'}}]}\n"
        "    else:\n"
        "        result = {}\n",
    )
    (home / ".mcp.json").write_text(
        json.dumps({"mcpServers": {"probe": {"command": sys.executable, "args": [str(fake)]}}})
    )
    try:
        with Server(
            [
                # A limit set a moment ago already bounds this request.
                lambda _, body: event({"content": f"Before restart, cap {body.get('max_tokens')}"}),
                lambda _, body: event(
                    {"content": "history kept" if "Before restart" in json.dumps(body) else "lost"}
                ),
            ]
        ) as server:
            result = run_dialog(
                root,
                base_env(home, server.url),
                "/mcp\n/mcp off probe\n/mcp on probe\n"
                "/config user UAGENT_MCP_TIMEOUT=100\n"
                "/config user UAGENT_MAX_TOKENS=321\n/config\n"
                "First turn\n/restart\nSecond turn\n"
                "/config user reset\n/rename " + "x" * 300 + "\n/rename Parity check\n"
                "/permissions rules\n/q\n",
                binary=binary,
            )
        output = result.stdout
        assert_true(result.returncode == 0, result.stderr)
        assert_true(re.search(r"probe .*global .*1 tools", output), output)
        assert_true("disabled" in output, output)
        assert_true("UAGENT_MCP_TIMEOUT: needs a restart" in output, output)
        assert_true("/restart applies it here" in output, output)
        assert_true("UAGENT_MAX_TOKENS: active at the next user turn" in output, output)
        assert_true("Before restart, cap 321" in output, output)
        assert_true(re.search(r"UAGENT_MCP_TIMEOUT = .*100 .*All conversations", output), output)
        assert_true("history kept" in output, output)
        assert_true("mcp.timeout" not in saved_settings(home), "reset")
        assert_true("no rules for this repository" in output, output)
        # A typed rename keeps to the rule a title has everywhere else.
        assert_true("title must be 1" in output, output)
        assert_true(
            any("Parity check" in path.read_text() for path in session_files(home)),
            "rename not saved",
        )
    finally:
        (home / ".mcp.json").unlink(missing_ok=True)
