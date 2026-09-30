import json
import re
import signal
import sys
import termios
import time
import unicodedata

from integration_support import (
    Server,
    assert_true,
    base_env,
    event,
    run,
    run_dialog,
    run_pty,
    session_files,
    timeout_setting,
    tool_call,
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


def test_command_help(root, home, *, binary):
    with Server([event({"content": "unused"})]) as server:
        result = run_dialog(
            root, base_env(home, server.url), "/models\n/wat\n/recap\n/help\n/exit\n", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        # Every quit alias detaches locally instead of reaching the runtime.
        assert_true("use conversation controls" not in result.stdout, result.stdout)
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
                event(
                    {"reasoning": ("**Planning provider normalization phases**\n")},
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
                (b"/verbose\n", b"verbose on"),
                # Quit once the turn has settled, however slow the build.
                (b"go\n", b"Final answer", b"Ready", None),
                b"/q\n",
            ],
            timeout=10,
            binary=binary,
        )
        assert_true(code == 0, verbose)
        assert_true(b"\xc2\xb7 Thinking" in verbose, verbose)
        reasoning_style = b"\x1b[0m\x1b[39m\x1b[49m\x1b[90m\x1b[3m"
        assert_true(reasoning_style + b"latest line" in verbose, verbose)
        assert_true(verbose.find(b"first line") < verbose.find(b"Final answer"), verbose)

        # Compact mode selects a complete heading and removes decoration.
        code, compact = run_pty(
            root,
            env,
            [
                (b"go\n", b"provider normalization phases"),
                (b"", b"Final answer", b"Ready", None),
                b"/q\n",
            ],
            timeout=10,
            binary=binary,
        )
        assert_true(code == 0, compact)
        assert_true(b"Thinking \xc2\xb7" in compact, compact)
        assert_true(b"provider normalization phases" in compact, compact)
        assert_true(b"Thinking \xc2\xb7 \xe2\x80\xa6" not in compact, compact)
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
                (b"/verbose\n", b"verbose on"),
                (b"/verbose\n", b"verbose off"),
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
        # A 24-column status keeps its never-dropped segments: state and route.
        assert_true(b"Ready \xc2\xb7 test\x1b[K" in output, output)
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
                (b"y\n", b"approval-done"),
                # The idle status carries the route in schema form and the
                # context window beside what is used.
                (b"", b"Ready \xc2\xb7 test \xc2\xb7 est. ctx"),
                (b"probe", b"probe"),  # input broker is accepting drafts
                b"\x7f" * 5,
                (b"\x1b[A", b"go"),
                (b"\n", b"approval-history-ok"),
                b"\x04",
            ],
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(b"approval-history-ok" in output, output)
        assert_true(b"/16.4k" in output, output)  # used/window, not used alone
        assert_true(len(server.requests) == 3, server.requests)


def test_multiline_run_keeps_action_color(root, home, *, binary):
    # Large enough to cross the old 2 KiB call-label cap and stdio write
    # boundaries. Every line carries its own bold SGR so a concurrent composer
    # repaint cannot turn the tail into the terminal default foreground.
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
        first = b"\x1b[1m\xe2\x86\x92 Running\r\n\x1b[1m# color-segment-000"
        assert_true(code == 0 and first in output, output)
        for index in range(90):
            marker = f"\x1b[1m# color-segment-{index:03d}".encode()
            assert_true(marker in output, (index, output))


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
                [(b"/verbose\n", b"verbose on")] * verbose
                + [(b"go\n", b"colour-ok", b"Ready", None), b"/q\n"],
                args=("--yolo",),
                binary=binary,
            )
            assert_true(code == 0, output[-2000:])
            assert_true(b"red-first" in output and b"green-last" in output, output[-2000:])
            assert_true(b"\\x1b" not in output, output[-2000:])


def test_multiline_rejected_call_shows_arguments(root, home, *, binary):
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
        assert_true(b"\xe2\x86\x92 edit_file(" in output, output)
        assert_true(b'"path":"visible-target"' in output, output)
        assert_true(b"unknown argument" in output, output)


def test_input_redraw_enter_then_escape_same_packet_interrupts_turn(root, home, *, binary):
    def delayed(_, __):
        time.sleep(2)
        return event({"content": "too-late"})

    with Server([delayed]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [(b"work\n\x1b", b"\xc2\xb7 interrupted"), b"/q\n"],
            timeout=8,
            binary=binary,
        )
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
                (b"work\n", b"Working"),
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
        response_at = output.index(b"uagent")
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
    global_config = home / ".uagent" / ".config"
    global_config.parent.mkdir(parents=True, exist_ok=True)
    global_config.write_text(
        "OPENROUTER_API_KEY=context-secret-sentinel\n"
        "UAGENT_PERMISSION_URL=https://user:pass@review.example/v1\n",
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
                (b"/mod", b"/models"),
                # Unchanged rows are not redrawn: wait for the draft's own.
                (b"\t", b"\x1b[49m/model"),
                b"\x15/q\n",
            ],
            binary=binary,
        )
        assert_true(code == 0, output)
        # Both candidates are offered, with the same description /help prints.
        assert_true(b"/models  search and select across providers" in output, output)
        assert_true(b"/model  choose what model to use" in output, output)
        # Tab commits the shared prefix and, once one row is left, the space
        # its argument needs.
        assert_true(b"/model " in output, output)
        assert_true(not server.get_requests, server.get_requests)


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


def test_input_ctrl_c_asks_once_then_quits(root, home, *, binary):
    """An idle SIGINT is half a gesture: the row says so, the second one exits."""
    with Server([event({"content": "unused"})]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (lambda process: process.send_signal(signal.SIGINT), b"Ctrl+C again to detach"),
                lambda process: process.send_signal(signal.SIGINT),
            ],
            timeout=10,
            binary=binary,
        )
        # The confirmed press preserves the signal exit status, so the shell
        # still sees the interrupt status it always saw.
        assert_true(code == 130, (code, output))
        assert_true(b"Ctrl+C again to detach" in output, output)
        assert_true(not server.get_requests, server.get_requests)


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
            "UAGENT_MAX_STEPS=23" in (home / ".uagent/.config").read_text(),
            "CLI config not persisted",
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
            "First approval\na\n/fork\nSecond approval\nn\n/q\n",
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
        [(b"/instructions edit sessions user\n", b"restarted sessions read it"), b"/q\n"],
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
                lambda _, _body: event({"content": "Before restart"}),
                lambda _, body: event(
                    {"content": "history kept" if "Before restart" in json.dumps(body) else "lost"}
                ),
            ]
        ) as server:
            result = run_dialog(
                root,
                base_env(home, server.url),
                "/mcp\n/mcp off probe\n/mcp on probe\n"
                "/config user UAGENT_MAX_TOKENS=100\n/config\n"
                "First turn\n/restart\nSecond turn\n"
                "/config user reset\n/rename Parity check\n/permissions rules\n/q\n",
                binary=binary,
            )
        output = result.stdout
        assert_true(result.returncode == 0, result.stderr)
        assert_true(re.search(r"probe .*global .*1 tools", output), output)
        assert_true("disabled" in output, output)
        assert_true("UAGENT_MAX_TOKENS: needs a restart" in output, output)
        assert_true("/restart applies it here" in output, output)
        assert_true(re.search(r"UAGENT_MAX_TOKENS = .*100 .*global-config", output), output)
        assert_true("history kept" in output, output)
        assert_true("UAGENT_MAX_TOKENS" not in (home / ".uagent/.config").read_text(), "reset")
        assert_true("no remembered actions for this repository" in output, output)
        assert_true(
            any("Parity check" in path.read_text() for path in session_files(home)),
            "rename not saved",
        )
    finally:
        (home / ".mcp.json").unlink(missing_ok=True)
