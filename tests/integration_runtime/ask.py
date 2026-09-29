import base64
import json
import re

from integration_support import (
    Server,
    assert_true,
    base_env,
    event,
    function_names,
    run,
    run_pty,
    tool_call,
    tool_results,
    wait_until,
)

QUESTIONS = [
    {
        "question": "Which database?",
        "header": "Database",
        "options": [
            {"label": "SQLite", "description": "one file"},
            {"label": "Postgres", "description": "a server"},
        ],
    },
    {
        "question": "Which extras?",
        "header": "Extras",
        "multi_select": True,
        "options": [{"label": "Metrics"}, {"label": "Tracing"}, {"label": "Backups"}],
    },
]
HINT = b"i image"
# A 1x1 PNG, enough for the attachment checks to recognise an image.
PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII="
)


def _ask_then_answer(answer="ask-ok"):
    def route(_, body):
        if not tool_results(body["messages"]):
            return tool_call("ask", {"questions": QUESTIONS})
        return event({"content": answer})

    return route


def test_ask_is_answered_in_the_terminal(root, home, *, binary):
    with Server([_ask_then_answer()]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"pick\n", HINT),
                # First question: the first option.
                (b"\r", HINT),
                # Second question: toggle Tracing and Backups, then done.
                ([b"2", b" ", b"3", b" ", b"\r"], b"ask-ok"),
                b"/q\n",
            ],
            binary=binary,
            timeout=20,
        )
        assert_true(code == 0, output[-2000:])
        result = tool_results(server.requests[-1][1]["messages"])[0]
        assert_true("Which database?\n→ SQLite" in result, result)
        assert_true("Which extras?\n→ Tracing, Backups" in result, result)


def test_ask_takes_their_own_words_and_an_image(root, home, *, binary):
    image = root / "sketch.png"
    image.write_bytes(PNG)
    with Server([_ask_then_answer()]) as server:
        code, output = run_pty(
            root,
            base_env(home, server.url),
            [
                (b"pick\n", HINT),
                # Other on the first question, in their words.
                ([b"3", b"\r"], b"In your words"),
                (b"DuckDB please\r", HINT),
                # An image and a choice on the second.
                (b"i", b"Image path"),
                (str(image).encode() + b"\r", b"image: "),
                (b"\r", b"ask-ok"),
                b"/q\n",
            ],
            binary=binary,
            timeout=20,
        )
        assert_true(code == 0, output[-2000:])
        final = server.requests[-1][1]
        result = tool_results(final["messages"])[0]
        assert_true("in their words: DuckDB please" in result, result)
        assert_true("Metrics; an image" in result, result)
        # The image itself reaches the model with the next request.
        assert_true("image_url" in json.dumps(final["messages"]), json.dumps(final)[-600:])


def test_ask_is_absent_where_no_one_can_answer(root, home, *, binary):
    with Server([event({"content": "plain-ok"})]) as server:
        result = run(root, base_env(home, server.url), "-p", "hi", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        names = function_names(server.requests[0][1])
        assert_true(names and "ask" not in names, names)


def test_a_threads_question_goes_to_its_coordinator(root, home, *, binary):
    answered = {}

    def route(_, body):
        text = json.dumps(body["messages"])
        results = tool_results(body["messages"])
        if "Objective: pick a database" in text:
            if not results:
                return tool_call("ask", {"questions": QUESTIONS[:1]})
            answered["result"] = results[0]
            return event({"content": "thread-done"})
        if "[question, not a user message]" in text and "sent answer" not in json.dumps(results):
            match = re.search(r"Thread ([0-9a-f]{16}) .*?interaction ([^)]+)\)", text)
            return tool_call(
                "decide",
                {
                    "session_id": match.group(1),
                    "interaction_id": match.group(2),
                    "decision": "answer",
                    "answers": [{"choices": ["Postgres"], "other": ""}],
                    "reason": "the brief names a server",
                },
            )
        if "[question" in text or "[thread event" in text:
            return event({"content": "coordinator-ack"})
        if not results:
            return tool_call(
                "thread",
                {
                    "action": "spawn",
                    "title": "Database",
                    "objective": "pick a database",
                    "environment": "local",
                },
            )
        return event({"content": "spawned-ok"})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "coord", "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        wait_until(lambda: "result" in answered, "the thread never heard its answer")
        assert_true("Which database?\n→ Postgres" in answered["result"], answered)
