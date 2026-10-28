import json
import os
import pathlib
import re
import shlex
import signal
import subprocess
import sys
import threading
import time

from integration_support import (
    OPENROUTER_SUPPORTS,
    Concurrency,
    Server,
    assert_token_budget_stop,
    assert_true,
    base_env,
    budget,
    descendant_pids,
    event,
    fnv1a64,
    function_names,
    has_message,
    run,
    run_dialog,
    saved_json_files,
    session_files,
    tool_call,
    tool_calls,
    tool_results,
    user_prompt,
    user_texts,
    wait_for_processes_stopped,
    wait_until,
    write_http_response,
    write_mcp_server,
)


def child_sessions(home):
    return saved_json_files(home / ".uagent" / "history", "*/agent-*.json")


STARTED = "[started] subagent "


def launched(texts):
    """Whether a background child was launched, by its receipt."""
    return any(STARTED in text for text in texts)


def wait_round():
    """The parent's round that waits for every child it started."""
    return tool_call("activity", {"operation": "wait", "wait_ms": 30000})


def collaborator(text):
    """The child id a resume note names."""
    match = re.search(r"\[collaborator (agent-[^;\]]+)|\[started\] subagent (agent-\S+) ", text)
    assert_true(match is not None, text)
    return match.group(1) or match.group(2)


def subagent_batch(prompts, usage=None, **arguments):
    """One parent round that starts a child per prompt, in one batch."""
    return tool_calls(
        [
            (f"task-{index}", "subagent", {"prompt": prompt, **arguments})
            for index, prompt in enumerate(prompts)
        ],
        usage,
    )


def test_subagent_usage_counts_toward_parent_token_budget(root, home, *, binary):
    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child-budget"):
            return event(
                {"content": "child-result"},
                usage={"prompt_tokens": 2, "completion_tokens": 4},
            )
        return tool_call("subagent", {"prompt": "child-budget", "background": False})

    with Server([route]) as server:
        envelope = assert_token_budget_stop(
            root, home, server, "--yolo", "--token-budget", "3", "-p", "delegate", binary=binary
        )
        assert_true(envelope["stop"]["session_generated_tokens"] == 4, envelope)
        assert_true(len(server.requests) == 2, server.requests)


def test_completed_parent_answer_survives_late_child_budget_usage(root, home, *, binary):
    child_requested = threading.Event()

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "late-budget-child"):
            child_requested.set()
            return event(
                {"content": "too-expensive-child"},
                usage={"prompt_tokens": 1, "completion_tokens": 4},
            )
        results = tool_results(messages)
        if launched(results):
            assert_true(child_requested.wait(budget(2)), "child request did not arrive")
            time.sleep(0.3)
            return event({"content": "parent-answer"})
        return tool_call("subagent", {"prompt": "late-budget-child", "background": True})

    with Server([route]) as server:
        result = run(
            root,
            base_env(home, server.url),
            "--yolo",
            "--token-budget",
            "3",
            "--json",
            "-p",
            "delegate",
            timeout=10,
            binary=binary,
        )
        envelope = json.loads(result.stdout)
        assert_true(result.returncode == 0, envelope)
        assert_true(envelope["answer"] == "parent-answer", envelope)
        assert_true(envelope["stop"]["reason"] == "completed", envelope)
        assert_true(envelope["stop"]["session_generated_tokens"] == 4, envelope)


def test_background_child_spend_after_its_turn_is_journaled(root, home, *, binary):
    """The turn record is written once, before a background child finishes;
    what the child spends afterwards is recorded beside it, not lost."""

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "slow-child"):
            time.sleep(0.5)
            return event(
                {"content": "child-done"},
                usage={"prompt_tokens": 1, "completion_tokens": 4},
            )
        if launched(tool_results(messages)):
            return event({"content": "parent-answer"})
        return tool_call("subagent", {"prompt": "slow-child", "background": True})

    trace = root / "late-child.jsonl"
    with Server([route]) as server:
        result = run(
            root,
            base_env(home, server.url),
            "--yolo",
            f"--debug={trace}",
            "-p",
            "delegate",
            timeout=10,
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        records = [json.loads(line) for line in trace.read_text().splitlines()]
        amended = [r["data"] for r in records if r["event"] == "turn_usage_amended"]
        assert_true(len(amended) == 1, amended)
        assert_true(amended[0]["turn"] == 1, amended)
        assert_true(amended[0]["usage"]["output"] == 4, amended)
        assert_true(amended[0]["statistics"]["model_calls"] == 1, amended)


def test_subagent_inherits_only_remaining_session_token_budget(root, home, *, binary):
    def route(_, body):
        messages = body["messages"]
        spent = {"prompt_tokens": 1, "completion_tokens": 3}
        if has_message(messages, "user", "child-remainder"):
            return tool_call("read_path", {"path": "."}, usage=spent)
        return tool_call(
            "subagent", {"prompt": "child-remainder", "background": False}, usage=spent
        )

    with Server([route]) as server:
        assert_token_budget_stop(
            root, home, server, "--yolo", "--token-budget", "5", "-p", "delegate", binary=binary
        )
        # The child inherited the two-token remainder and stopped before
        # executing its call or requesting a second model round.
        assert_true(len(server.requests) == 2, server.requests)


def test_a_batch_never_hands_a_child_an_unlimited_token_share(root, home, *, binary):
    """A remainder too small to split still bounds each child: a share is never
    0, which would mean no limit at all."""

    def route(_, body):
        messages = body["messages"]
        spent = {"prompt_tokens": 1, "completion_tokens": 3}
        if user_prompt(messages, ("share-a", "share-b")):
            return tool_call("read_path", {"path": "."}, usage=spent)
        return subagent_batch(("share-a", "share-b"), usage=spent)

    with Server([route]) as server:
        assert_token_budget_stop(
            root, home, server, "--yolo", "--token-budget", "4", "-p", "delegate", binary=binary
        )
        # Each child stopped at its one-token share after its first round.
        children = [
            b for _, b in server.requests if user_prompt(b["messages"], ("share-a", "share-b"))
        ]
        assert_true(len(children) == 2, len(children))


def test_subagent_auto_join_continues_turn(root, home, *, binary):
    waiting = threading.Event()

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            # Answers only once the parent waits for it, so the result joins.
            waiting.wait(budget(4))
            return event({"content": "child-result"})
        has_result = any(
            isinstance(message.get("content"), str) and "[Activity result:" in message["content"]
            for message in messages
        )
        if has_result:
            return event({"content": "late-task-ok"})
        if launched(str(message.get("content", "")) for message in messages):
            waiting.set()
            return wait_round()
        return tool_call("subagent", {"prompt": "child", "background": True})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_STREAM_TIMEOUT"] = "4"
        result = run(root, env, "--yolo", "-p", "delegate", timeout=8, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "late-task-ok", result.stdout)
        request_summary = [
            [
                (message.get("role"), str(message.get("content", ""))[:80])
                for message in body.get("messages", [])
            ]
            for _, body in server.requests
        ]
        assert_true(len(server.requests) == 4, request_summary)


def test_subagent_foreground_returns_result_without_wait_round(root, home, *, binary):
    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            time.sleep(0.3)
            return event({"content": "foreground-child-result"})
        results = tool_results(messages)
        if any("foreground-child-result" in result for result in results):
            direct = not launched(results)
            return event({"content": "foreground-task-ok" if direct else "foreground-task-bad"})
        return tool_call("subagent", {"prompt": "child", "background": False})

    with Server([route]) as server:
        env = base_env(home, server.url)
        args = ("--yolo", "--model", "parent-model", "-p", "delegate")
        result = run(root, env, *args, timeout=8, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "foreground-task-ok", result.stdout)
        # A child given no model runs on the one its parent is on.
        child_models = {
            body.get("model")
            for _, body in server.requests
            if has_message(body["messages"], "user", "child")
        }
        assert_true(child_models == {"parent-model"}, child_models)
        parent_requests = [
            body for _, body in server.requests if has_message(body["messages"], "user", "delegate")
        ]
        assert_true(len(parent_requests) == 2, len(parent_requests))


def test_lean_subagent_does_not_clone_parent_mcp_fleet(root, home, *, binary):
    marker = root / "mcp-starts"
    fake = root / "fake_mcp.py"
    write_mcp_server(
        fake,
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], "
        "'capabilities': {'tools': {}}}\n"
        "    else:\n"
        "        result = {'tools': []} if method == 'tools/list' else {}\n",
        extra_imports=("os", "pathlib"),
        setup=f"marker = pathlib.Path({str(marker)!r})\n"
        "with marker.open('a', encoding='utf-8') as output:\n"
        "    output.write(os.environ.get('UAGENT_INTERNAL_DEPTH', '0') + '\\n')\n",
    )
    (home / ".mcp.json").write_text(
        json.dumps(
            {"mcpServers": {"parent-only": {"command": sys.executable, "args": [str(fake)]}}}
        ),
        encoding="utf-8",
    )

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            return event({"content": "lean-child-result"})
        if any("lean-child-result" in result for result in tool_results(messages)):
            return event({"content": "lean-mcp-ok"})
        return tool_call("subagent", {"prompt": "child", "background": False})

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "delegate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "lean-mcp-ok", result.stdout)
        starts = marker.read_text(encoding="utf-8").splitlines()
        assert_true(starts == ["0"], starts)


def test_subagent_followup_resumes_durable_conversation(root, home, *, binary):
    def route(_, body):
        messages = body["messages"]
        child_prompts = user_texts(messages)
        if child_prompts and child_prompts[-1].endswith("directive kept?"):
            kept = child_prompts[-1].startswith("[collaborator directive]\nalways mention beta")
            return event({"content": "directive-kept" if kept else "directive-lost"})
        if any(prompt.endswith("remember alpha") for prompt in child_prompts):
            initial_directive = any(
                prompt == "[collaborator directive]\nalways mention beta\n\nremember alpha"
                for prompt in child_prompts
            )
            directed_followup = any(
                prompt
                == "[collaborator directive]\nalways mention beta\n\nwhat did I ask you to remember?"
                for prompt in child_prompts
            )
            if directed_followup:
                return event(
                    {
                        "content": "alpha-from-history-with-directive"
                        if initial_directive
                        else "initial-directive-missing"
                    }
                )
            return event({"content": "stored alpha" if initial_directive else "bad seed"})
        results = "\n".join(tool_results(messages))
        if "directive-kept" in results:
            return event({"content": "followup-ok"})
        if "alpha-from-history-with-directive" in results:
            return tool_call(
                "subagent",
                {
                    "operation": "followup",
                    "agent_id": collaborator(results),
                    # An empty value is one that was not given.
                    "prompt": "directive kept?",
                    "directive": "",
                    "background": False,
                },
            )
        if "stored alpha" in results:
            return tool_call(
                "subagent",
                {
                    "operation": "followup",
                    "agent_id": collaborator(results),
                    "prompt": "what did I ask you to remember?",
                    "background": False,
                },
            )
        return tool_call(
            "subagent",
            {
                "prompt": "remember alpha",
                "directive": "always mention beta",
                "background": False,
            },
        )

    with Server([route]) as server:
        result = run(root, base_env(home, server.url), "--yolo", "-p", "collaborate", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "followup-ok", result.stdout)
        # The child is an ordinary saved session whose header carries its role.
        sessions = child_sessions(home)
        assert_true(len(sessions) == 1, sessions)
        text = sessions[0].read_text(encoding="utf-8")
        role = json.loads(text.split("\n", 1)[0])["delegation"]
        assert_true(role["directive"] == "always mention beta", role)
        assert_true(role["label"] == "directive kept?", role)
        # A child has to know it is one: guidance can arrive mid-run as an
        # ordinary user message, which it cannot infer from its own prompt.
        assert_true("[collaborator:" in text, sessions[0])


def test_message_to_finished_child_runs_it_once(root, home, *, binary):
    """A message to a finished child runs it again on that message, once: a
    later followup, failed or not, never repeats it."""
    collaborator_id = {"value": ""}

    def route(_, body):
        messages = body["messages"]
        users = user_texts(messages)
        panel = next(
            (
                index
                for index in range(len(messages) - 1, -1, -1)
                if messages[index].get("role") == "user"
                and str(messages[index].get("content", "")).endswith("panel")
            ),
            None,
        )
        parent_prompt = messages[panel]["content"] if panel is not None else ""
        results = tool_results(messages[panel + 1 :]) if panel is not None else []

        if users and users[-1] == "seed child":
            return event({"content": "seeded"})
        if users and users[-1] == "queued once":
            return event({"content": "heard"})
        if users and "fail child" in users[-1]:
            return event({}, finish="content_filter")
        if users and users[-1] == "retry child":
            valid = users.count("queued once") == 1
            return event({"content": "mailbox-cleared" if valid else "mailbox-repeated"})

        if parent_prompt == "spawn panel":
            if results:
                collaborator_id["value"] = collaborator(results[-1])
                return event({"content": "spawned"})
            return tool_call("subagent", {"prompt": "seed child", "background": False})
        if parent_prompt == "queue panel":
            if results and "heard" in results[-1]:
                return event({"content": "queued"})
            if results:
                return wait_round()
            return tool_call(
                "subagent",
                {
                    "operation": "message",
                    "agent_id": collaborator_id["value"],
                    "prompt": "queued once",
                },
            )
        if parent_prompt == "fail panel":
            if results:
                return event({"content": "failure-observed"})
            return tool_call(
                "subagent",
                {
                    "operation": "followup",
                    "agent_id": collaborator_id["value"],
                    "prompt": "fail child",
                    "background": False,
                },
            )
        if parent_prompt == "retry panel":
            if results:
                valid = "mailbox-cleared" in results[-1]
                return event({"content": "mailbox-ok" if valid else "mailbox-bad"})
            return tool_call(
                "subagent",
                {
                    "operation": "followup",
                    "agent_id": collaborator_id["value"],
                    "prompt": "retry child",
                    "background": False,
                },
            )
        return event({"content": "unexpected-route"})

    with Server([route]) as server:
        env = base_env(home, server.url)
        # All operations belong to one panel conversation. An unrelated
        # session in the same folder must not control its retained children.
        result = run_dialog(
            root,
            env,
            "spawn panel\nqueue panel\nfail panel\nretry panel\n/q\n",
            "--yolo",
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(
            "mailbox-ok" in result.stdout and "mailbox-bad" not in result.stdout, result.stdout
        )
        # Guidance lives in the child's mailbox until its delivery is saved,
        # so a message that reached the child has to leave nothing behind.
        left = list((home / ".uagent" / "mail").glob("*/*/*.json"))
        assert_true(not left, left)


def test_message_reaches_running_child(root, home, *, binary):
    """A message to a running child is delivered without waiting for a followup."""
    scale = max(1, int(float(os.environ.get("UAGENT_TEST_TIMEOUT_SCALE", "1"))))

    def route(_, body):
        messages = body["messages"]
        users = user_texts(messages)
        if "wait-for-guidance" in users:
            if any("[parent guidance]" in user for user in users):
                return event({"content": "guidance-received"})
            if len(messages) > 30 * scale:
                return event({"content": "guidance-never-arrived"})
            # Idle in short steps: the drain runs between them, so the child
            # only has to still be alive when the message lands.
            return tool_call("run", {"command": "sleep 0.2"})
        combined = "\n".join(str(message.get("content", "")) for message in messages)
        if "guidance-received" in combined:
            return event({"content": "live-message-ok"})
        if "sent to agent" in combined:
            return wait_round()
        if STARTED in combined:
            return tool_call(
                "subagent",
                {
                    "operation": "message",
                    "agent_id": collaborator(combined),
                    "prompt": "say banana",
                },
            )
        return tool_call("subagent", {"prompt": "wait-for-guidance", "background": True})

    with Server([route]) as server:
        result = run(
            root,
            base_env(home, server.url),
            "--yolo",
            "-p",
            "coordinate",
            timeout=60,
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "live-message-ok", result.stdout)
        sessions = child_sessions(home)
        assert_true(len(sessions) == 1, sessions)
        # The guidance became an ordinary user message in the child's own
        # conversation, which is what makes it steering rather than a note.
        # The record is JSON, so the wrapper's newline is escaped in the file.
        transcript = sessions[0].read_text(encoding="utf-8")
        assert_true("[parent guidance]\\nsay banana" in transcript, transcript[:2000])
        left = list((home / ".uagent" / "mail").glob("*/*/*.json"))
        assert_true(not left, f"delivered mail was left behind: {left}")


def test_agents_command_lists_a_running_child(root, home, *, binary):
    """/agents answers from the records and the supervisor, without a turn."""

    def route(_, body):
        messages = body["messages"]
        users = user_texts(messages)
        if "stay-busy" in users:
            # Bounded: the panel quits long before this, and a child that
            # outlived the dialog would keep the test's process group alive.
            if len(messages) > 8:
                return event({"content": "child-done"})
            return tool_call("run", {"command": "sleep 1"})
        combined = "\n".join(str(message.get("content", "")) for message in messages)
        if STARTED in combined:
            return event({"content": "delegated"})
        return tool_call("subagent", {"prompt": "stay-busy", "background": True})

    with Server([route]) as server:
        result = run_dialog(
            root,
            base_env(home, server.url),
            "delegate\n/agents\n/q\n",
            "--yolo",
            timeout=30,
            binary=binary,
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true("subagents" in result.stdout, result.stdout)
        # The row joins the record on disk with the live job: the id and the
        # mode come from the record, "running" only from the supervisor.
        listing = result.stdout.split("subagents", 1)[1]
        assert_true(re.search(r"agent-[0-9a-f]{8}\s+running · lean", listing), listing)


def test_parallel_subagents_auto_join(root, home, *, binary):
    running = Concurrency()

    def route(_, body):
        messages = body["messages"]
        if child := user_prompt(messages, ("child-a", "child-b")):
            with running:
                time.sleep(1)
                return event({"content": f"{child}-result"})
        combined = "\n".join(str(message.get("content", "")) for message in messages)
        if "child-a-result" in combined and "child-b-result" in combined:
            return event({"content": "parallel-task-ok"})
        if STARTED in combined:
            return wait_round()
        return subagent_batch(("child-a", "child-b"), background=True)

    with Server([route]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "--yolo", "-p", "delegate twice", timeout=8, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "parallel-task-ok", result.stdout)
        assert_true(running.most == 2, running.most)
        parent_requests = [
            body
            for _, body in server.requests
            if has_message(body["messages"], "user", "delegate twice")
        ]
        assert_true(3 <= len(parent_requests) <= 4, len(parent_requests))
        for request in parent_requests:
            assert_true("subagent" in function_names(request), request)


def test_a_batch_of_children_runs_together_and_shares_the_budget(root, home, *, binary):
    """Children the turn waits for run side by side; under a session budget
    each takes an equal share of what is left."""
    running = Concurrency()

    def route(_, body):
        messages = body["messages"]
        if child := user_prompt(messages, ("child-a", "child-b")):
            with running:
                time.sleep(1)
                return event({"content": f"{child}-result"})
        results = tool_results(messages)
        if results:
            shared = all("limits.cost to $0.5000, its share" in result for result in results)
            return event({"content": "shared-ok" if shared else " | ".join(results)})
        return subagent_batch(("child-a", "child-b"), limits={"cost": 0.9})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_SESSION_BUDGET"] = "1"
        result = run(root, env, "--yolo", "-p", "delegate twice", timeout=8, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "shared-ok", result.stdout)
        assert_true(running.most == 2, running.most)


def test_a_batch_of_six_children_runs_at_once(root, home, *, binary):
    """A child holds its tool worker for its whole run, so a batch gets a
    worker per child up to the slots children may hold, not the four other
    parallel calls share."""
    running = Concurrency()
    prompts = tuple(f"child-{index}" for index in range(6))

    def route(_, body):
        messages = body["messages"]
        if child := user_prompt(messages, prompts):
            with running:
                time.sleep(1)
                return event({"content": f"{child}-result"})
        if tool_results(messages):
            return event({"content": "six-ok"})
        return subagent_batch(prompts)

    with Server([route]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "--yolo", "-p", "delegate six", timeout=8, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "six-ok", result.stdout)
        assert_true(running.most == 6, running.most)


def test_a_child_answer_is_held_to_the_schema_it_was_asked_for(root, home, *, binary):
    """A child given a schema is told the format in its brief. An answer that
    does not fit goes back to it with the reason, and the one that fits
    reaches the parent. The plan step it works for is kept in its role."""
    schema = {
        "type": "object",
        "required": ["claims"],
        "properties": {"claims": {"type": "array"}},
    }
    briefs = []

    def route(_, body):
        messages = body["messages"]
        brief = next((text for text in user_texts(messages) if text.startswith("child")), None)
        if brief is not None:
            briefs.append(brief)
            said = "\n".join(str(message.get("content", "")) for message in messages)
            if "does not match the schema: `claims` is required" in said:
                return event({"content": '```json\n{"claims": ["fits"]}\n```'})
            return event({"content": '{"findings": []}'})
        if results := tool_results(messages):
            return event({"content": results[0]})
        return tool_call("subagent", {"prompt": "child", "step": "s1", "schema": schema})

    with Server([route]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "--yolo", "-p", "delegate", timeout=8, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true('{"claims": ["fits"]}' in result.stdout, result.stdout)
        assert_true(len(briefs) == 2, briefs)
        assert_true("[answer format]" in briefs[0] and '"claims"' in briefs[0], briefs[0])
        [child] = child_sessions(home)
        role = json.loads(child.read_text().split("\n", 1)[0])["delegation"]
        assert_true(role["step"] == "s1" and role["schema"] == schema, role)


def test_a_fanout_keeps_only_what_its_refuters_cannot_refute(root, home, *, binary):
    """One finder per item runs at once; each finding gets a refuter; the parent
    receives the verified findings and the refuted ones with their reason."""
    running = Concurrency()
    finding = {"type": "object", "required": ["claim"], "properties": {"claim": {"type": "string"}}}
    found = {"find in a": ["a-1", "a-2"], "find in b": ["b-1"]}

    def route(_, body):
        messages = body["messages"]
        brief = next(
            (t for t in user_texts(messages) if t.startswith(("find in", "Try to refute"))), None
        )
        if brief and brief.startswith("find in"):
            with running:
                time.sleep(0.5)
                claims = found[brief.split("\n")[0]]
                return event({"content": json.dumps({"findings": [{"claim": c} for c in claims]})})
        if brief:
            refuted = "a-2" in brief.split("The task that produced it")[0]
            verdict = "refuted" if refuted else "holds"
            return event({"content": json.dumps({"verdict": verdict, "reason": f"{verdict}-why"})})
        if results := tool_results(messages):
            return event({"content": results[0]})
        return tool_call(
            "subagent_fanout",
            {
                "items": ["a", "b"],
                "prompt": "find in {item}",
                "finding": finding,
                "step": "s1",
            },
        )

    with Server([route]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "--yolo", "-p", "survey", timeout=15, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        report = json.loads(result.stdout[result.stdout.index("{") : result.stdout.rindex("}") + 1])
        assert_true(sorted(f["claim"] for f in report["verified"]) == ["a-1", "b-1"], report)
        assert_true(
            report["refuted"] == [{"finding": {"claim": "a-2"}, "reason": "refuted-why"}], report
        )
        assert_true(report["children"] == 5 and not report["failed_items"], report)
        assert_true(running.most == 2, running.most)
        roles = [
            json.loads(path.read_text().split("\n", 1)[0])["delegation"]
            for path in child_sessions(home)
        ]
        assert_true(len(roles) == 5 and all(role["step"] == "s1" for role in roles), roles)


def test_a_fanout_without_refuters_verifies_nothing(root, home, *, binary):
    """With refuters 0 nobody checks a finding, so each comes back unchecked."""
    finding = {"type": "object", "required": ["claim"], "properties": {"claim": {"type": "string"}}}

    def route(_, body):
        messages = body["messages"]
        if any(text.startswith("find in") for text in user_texts(messages)):
            return event({"content": json.dumps({"findings": [{"claim": "x"}]})})
        if results := tool_results(messages):
            return event({"content": results[0]})
        return tool_call(
            "subagent_fanout",
            {
                "items": ["a", "b"],
                "prompt": "find in {item}",
                "finding": finding,
                "refuters": 0,
            },
        )

    with Server([route]) as server:
        env = base_env(home, server.url)
        result = run(root, env, "--yolo", "-p", "survey", timeout=15, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        report = json.loads(result.stdout[result.stdout.index("{") : result.stdout.rindex("}") + 1])
        assert_true(report["verified"] == [] and len(report["unchecked"]) == 2, report)
        assert_true(report["children"] == 2, report)


def test_subagent_interrupt_reaps_child(root, home, *, binary):
    """A soft interrupt during the tool batch must not orphan delegation."""
    batch = tool_calls(
        [
            ("call-task", "subagent", {"prompt": "slow-child", "background": True}),
            ("call-run", "run", {"command": "sleep 5"}),
        ]
    )

    stopped = threading.Event()

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "slow-child"):
            stopped.wait(budget(10))
            return event({"content": "too-late"})
        if any(message.get("role") == "tool" for message in messages):
            return event({"content": "unexpected-continuation"})
        return batch

    server = Server([route])
    trace = root / "subagent-interrupt.jsonl"
    process = None
    try:
        process = subprocess.Popen(
            [
                str(binary),
                "--yolo",
                f"--debug={trace}",
                "-p",
                "delegate then run",
            ],
            cwd=root,
            env=base_env(home, server.url),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        child_pids = set()

        def delegated_child_started():
            if not trace.exists():
                return False
            for line in trace.read_text().splitlines():
                event_data = json.loads(line)
                data = event_data.get("data", {})
                if (
                    event_data.get("event") == "tool_result"
                    and data.get("name") == "subagent"
                    and str(data.get("result", "")).startswith(STARTED)
                ):
                    child_pids.update(descendant_pids(process.pid))
                    return True
            return False

        wait_until(delegated_child_started, "delegated child did not start", timeout=4)
        assert_true(child_pids, "delegated child pid was not observable")
        process.send_signal(signal.SIGINT)
        process.communicate(timeout=8)
        survivors = wait_for_processes_stopped(child_pids)
        assert_true(not survivors, f"delegated child processes survived: {survivors}")
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        stopped.set()
        server.close()


def test_subagent_uses_selected_model_route(root, home, *, binary):
    child_output = root / "delegated-by-child.txt"

    def child_action(_, body):
        assert_true(body.get("model") == "child-model", body.get("model"))
        assert_true("reasoning" in body and "stream_options" not in body, body)
        names = function_names(body)
        assert_true({"write_file", "edit_file", "memory"} <= names, names)
        return tool_call(
            "write_file",
            {"path": str(child_output), "content": "delegated"},
        )

    def child_reply(_, body):
        succeeded = any(
            message.get("role") == "tool" and "wrote" in str(message.get("content", ""))
            for message in body["messages"]
        )
        return event({"content": "child-route-ok" if succeeded else "child-route-bad"})

    child = Server([child_action, child_reply])

    def delegate(_, body):
        runtime = "\n".join(user_texts(body["messages"]))
        assert_true("[delegation: parent=" in runtime, runtime)
        return tool_call(
            "subagent",
            {
                "prompt": "child",
                "mode": "full",
                "model": "codex-local/child model",
                "background": True,
            },
        )

    def finish(_, body):
        combined = "\n".join(str(message.get("content", "")) for message in body["messages"])
        return event({"content": "route-ok" if "child-route-ok" in combined else "route-bad"})

    parent = Server(
        [
            delegate,
            lambda *_: wait_round(),
            finish,
        ]
    )
    try:
        env = base_env(home, parent.url)
        env["UAGENT_CONTEXT"] = "32768"
        env["UAGENT_PROVIDERS"] = json.dumps(
            {
                "codex-local": {
                    "base_url": child.url,
                    "api_key": "local-key",
                    "supports": OPENROUTER_SUPPORTS,
                    "models": {"child-model": {"id": "child-model", "effort": "low"}},
                }
            }
        )
        result = run_dialog(root, env, "delegate locally\ny\n/q\n", timeout=20, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true("route-ok" in result.stdout, result.stdout)
        assert_true(
            child_output.read_text(encoding="utf-8") == "delegated",
            child_output.read_text(encoding="utf-8"),
        )
        assert_true(len(child.requests) == 2, len(child.requests))
    finally:
        parent.close()
        child.close()


def test_subagent_model_is_the_route_a_call_that_names_none_runs_on(root, home, *, binary):
    """subagent.model sends a child that names no route to that one, and the
    tool says so to the parent."""
    child = Server([lambda _, body: event({"content": f"ran on {body.get('model')}"})])
    described = []

    def delegate(_, body):
        tool = next(t for t in body["tools"] if t["function"]["name"] == "subagent")
        described.append(tool["function"]["parameters"]["properties"]["model"]["description"])
        return tool_call("subagent", {"prompt": "child"})

    parent = Server(
        [delegate, lambda _, body: event({"content": tool_results(body["messages"])[0]})]
    )
    try:
        env = base_env(home, parent.url)
        env["UAGENT_SUBAGENT_MODEL"] = "codex-local/child model"
        env["UAGENT_PROVIDERS"] = json.dumps(
            {
                "codex-local": {
                    "base_url": child.url,
                    "api_key": "local-key",
                    "supports": OPENROUTER_SUPPORTS,
                    "models": {"child-model": {"id": "child-model"}},
                }
            }
        )
        result = run(root, env, "--yolo", "-p", "delegate", timeout=15, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true("ran on child-model" in result.stdout, result.stdout)
        assert_true("codex-local/child model, the configured default" in described[0], described)
    finally:
        parent.close()
        child.close()


def test_subagent_failure_reports_route_stage_and_bounded_diagnostics(root, home, *, binary):
    def reject_child(handler, body):
        assert_true(body.get("model") == "unsupported-model", body)
        payload = json.dumps(
            {
                "error": {
                    "message": "fixture endpoint rejects unsupported-model",
                    "type": "invalid_request_error",
                    "code": "model_not_found",
                }
            }
        ).encode()
        write_http_response(handler, payload, status=400)

    child = Server([reject_child])

    def route(_, body):
        results = tool_results(body["messages"])
        report = next((result for result in reversed(results) if "configured route:" in result), "")
        if report:
            valid = all(
                marker in report
                for marker in (
                    "configured route: failing/child -> failing/unsupported-model",
                    "@ 127.0.0.1",
                    "failure stage: child execution",
                    "remedy:",
                    "fixture endpoint rejects unsupported-model",
                )
            )
            valid = valid and "child-secret-do-not-print" not in report and len(report) < 4000
            return event({"content": "child-diagnostic-ok" if valid else report})
        if launched(results):
            return wait_round()
        return tool_call(
            "subagent",
            {
                "prompt": "fail on the configured route",
                "model": "failing/child",
                "background": True,
            },
        )

    parent = Server([route])
    try:
        env = base_env(home, parent.url)
        env["UAGENT_PROVIDERS"] = json.dumps(
            {
                "failing": {
                    "base_url": child.url,
                    "api_key": "child-secret-do-not-print",
                    "models": {"child": {"id": "unsupported-model", "effort": "low"}},
                }
            }
        )
        result = run(root, env, "--yolo", "-p", "delegate failure", timeout=12, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "child-diagnostic-ok", result.stdout)
        assert_true(len(child.requests) == 1, len(child.requests))
        assert_true(len(parent.requests) == 3, len(parent.requests))
    finally:
        parent.close()
        child.close()


def test_subagent_recursion_is_depth_bounded(root, home, *, binary):
    """Full agents honor depth; lean workers never expose recursive delegation."""

    def has_task(body):
        return "subagent" in function_names(body)

    for depth, cap, expected in (
        ("0", "2", True),
        ("1", "2", True),
        ("2", "2", False),
        ("0", "0", False),
    ):
        with Server([lambda _, body: event({"content": str(has_task(body))})]) as server:
            env = base_env(home, server.url)
            env["UAGENT_INTERNAL_DEPTH"] = depth
            env["UAGENT_SUBAGENT_DEPTH"] = cap
            result = run(root, env, "-p", "probe", binary=binary)
            assert_true(result.returncode == 0, result.stderr)
            assert_true(
                result.stdout.strip() == str(expected),
                (depth, cap, expected, result.stdout),
            )

    with Server([lambda _, body: event({"content": str(has_task(body))})]) as server:
        env = base_env(home, server.url)
        env["UAGENT_INTERNAL_DEPTH"] = "1"
        env["UAGENT_SUBAGENT_DEPTH"] = "2"
        env["UAGENT_INTERNAL_TOOLSET"] = "lean"
        result = run(root, env, "-p", "probe", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "False", result.stdout)


def test_subagent_reports_the_limit_that_stopped_the_child(root, home, *, binary):
    """A child that hits a ceiling answers from what it has and says which
    ceiling, so the caller can decide."""

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            if "No further tool call will run" in str(messages[-1].get("content")):
                return event({"content": "child-findings-so-far"})
            # Two calls in one round against a ceiling of one: the child
            # refuses the batch and stops on max_tool_calls.
            return tool_calls(
                [(f"child-call-{i}", "run", {"command": "echo child-work"}) for i in range(2)]
            )
        results = tool_results(messages)
        if results:
            report = results[-1]
            assert_true("child-findings-so-far" in report, report)
            assert_true("delegated child failed" not in report, report)
            assert_true("max_tool_calls" in report, report)
            assert_true("child stopped" in report, report)
            # The parent gets the child's answer and reason, not its envelope.
            assert_true("uagent.headless.v1" not in report, report)
            return event({"content": "limit-reported-ok"})
        return tool_call(
            "subagent",
            {"prompt": "child", "background": False, "limits": {"tool_calls": 1}},
        )

    with Server([route]) as server:
        result = run(
            root, base_env(home, server.url), "--yolo", "-p", "delegate", timeout=30, binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "limit-reported-ok", result.stdout)


def test_subagent_answer_survives_a_record_larger_than_the_cap(root, home, *, binary):
    """A child's record is read whole, however much trace it drags behind it."""

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            if tool_results(messages):
                return event({"content": "child-answer-9f3a"})
            # Enough captured output that the envelope this child prints is
            # far larger than the cap its parent reads a tool result through.
            return tool_call("run", {"command": "printf 'x%.0s' $(seq 1 60000)"})
        results = tool_results(messages)
        if results:
            report = results[-1]
            # The answer, not the slice of trace the cap happened to land on.
            assert_true("child-answer-9f3a" in report, report)
            assert_true("no result envelope" not in report, report)
            assert_true("uagent.headless.v1" not in report, report)
            return event({"content": "recovered-ok"})
        return tool_call("subagent", {"prompt": "child", "background": False})

    with Server([route]) as server:
        result = run(
            root, base_env(home, server.url), "--yolo", "-p", "delegate", timeout=60, binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "recovered-ok", result.stdout)


def test_subagent_foreground_outlives_the_per_call_budget(root, home, *, binary):
    """A child the caller waits for is bounded by limits.seconds, not by the
    budget that stops a runaway command.

    The child spends its time working rather than stalling, so this measures
    the parent's per-call budget and not the child's first-event timeout.
    """

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            child_results = tool_results(messages)
            if any("child-slept" in result for result in child_results):
                return event({"content": "slow-child-result"})
            return tool_call("run", {"command": "sleep 2; echo child-slept"})
        results = tool_results(messages)
        if any("slow-child-result" in result for result in results):
            return event({"content": "slow-child-ok"})
        return tool_call("subagent", {"prompt": "child", "background": False})

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_TOOL_TIMEOUT"] = "1"
        result = run(root, env, "--yolo", "-p", "delegate", timeout=40, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "slow-child-ok", result.stdout)


def test_a_background_child_keeps_its_own_time_limit(root, home, *, binary):
    """limits.seconds stops a child nobody is waiting on, and it still answers."""

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            if "No further tool call will run" in str(messages[-1].get("content")):
                return event({"content": "child-out-of-time"})
            return tool_call("run", {"command": "sleep 30"})
        results = tool_results(messages)
        report = next((result for result in results if "child-out-of-time" in result), "")
        if report:
            assert_true("child stopped: turn_deadline" in report, report)
            return event({"content": "time-limit-ok"})
        if launched(results):
            return wait_round()
        return tool_call(
            "subagent", {"prompt": "child", "background": True, "limits": {"seconds": 1}}
        )

    with Server([route]) as server:
        started = time.monotonic()
        result = run(
            root, base_env(home, server.url), "--yolo", "-p", "delegate", timeout=30, binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "time-limit-ok", result.stdout)
        assert_true(time.monotonic() - started < budget(15), "the child outlived its limit")


def test_stopping_a_child_stops_what_it_started(root, home, *, binary):
    """A stopped child takes its own commands and children with it, and what
    they spent still reaches the parent."""
    pids = [root / "command.pid", root / "grandchild-command.pid"]

    spent = {"prompt_tokens": 1, "completion_tokens": 4}

    def sleeper(pid_file):
        command = f"echo $$ > {shlex.quote(str(pid_file))}; sleep 60"
        return tool_call("run", {"command": command}, usage=spent)

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "grandchild"):
            return sleeper(pids[1])
        if has_message(messages, "user", "child"):
            if launched(tool_results(messages)):
                return sleeper(pids[0])
            grandchild = {"prompt": "grandchild", "mode": "full", "background": True}
            return tool_call("subagent", grandchild, usage=spent)
        results = tool_results(messages)
        if any("stopped" in result for result in results[1:]):
            return event({"content": "child-stopped"})
        started = (
            re.search(r"\[started\] subagent agent-\S+ \(activity (\d+)\)", results[0])
            if results
            else None
        )
        if started:
            wait_until(lambda: all(p.exists() for p in pids), "the child tree did not start")
            return tool_call("activity", {"operation": "stop", "id": int(started.group(1))})
        return tool_call("subagent", {"prompt": "child", "mode": "full", "background": True})

    with Server([route]) as server:
        args = ("--yolo", "--json", "-p", "delegate")
        result = run(root, base_env(home, server.url), *args, timeout=30, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        envelope = json.loads(result.stdout)
        assert_true(envelope["answer"] == "child-stopped", envelope)
        # Two rounds of the child and one of its child, four tokens each.
        assert_true(envelope["stop"]["session_generated_tokens"] == 12, envelope["stop"])
        survivors = wait_for_processes_stopped({int(p.read_text()) for p in pids})
        assert_true(not survivors, f"processes of a stopped child survived: {survivors}")


def test_a_follow_up_reports_only_what_it_spent(root, home, *, binary):
    """A resumed child's session holds what its earlier runs used; the parent
    is told what this run added, so nothing is counted twice."""
    spent = {"prompt_tokens": 1, "completion_tokens": 4}

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "again"):
            return event({"content": "second-run"}, usage=spent)
        if has_message(messages, "user", "child"):
            return event({"content": "first-run"}, usage=spent)
        results = "\n".join(tool_results(messages))
        if "second-run" in results:
            return event({"content": "both-ran"})
        if "first-run" in results:
            agent = collaborator(results)
            again = {"operation": "followup", "agent_id": agent, "prompt": "again"}
            return tool_call("subagent", again | {"background": False})
        return tool_call("subagent", {"prompt": "child", "background": False})

    with Server([route]) as server:
        args = ("--yolo", "--json", "-p", "delegate")
        result = run(root, base_env(home, server.url), *args, timeout=30, binary=binary)
        envelope = json.loads(result.stdout)
        assert_true(envelope["answer"] == "both-ran", envelope)
        assert_true(envelope["stop"]["session_generated_tokens"] == 8, envelope["stop"])


def test_a_childs_time_limit_covers_what_it_left_running(root, home, *, binary):
    """A child that answered is not kept alive past limits.seconds by a child
    of its own that is still working."""

    finished = threading.Event()

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "grandchild"):
            finished.wait(budget(20))
            return event({"content": "too-late"})
        if has_message(messages, "user", "child"):
            if tool_results(messages):
                return event({"content": "child-answered"})
            return tool_call("subagent", {"prompt": "grandchild", "background": True})
        report = next((r for r in tool_results(messages) if "child-answered" in r), "")
        if report:
            # It answered, and says that its time ended with work outstanding.
            assert_true("child stopped: turn_deadline" in report, report)
            return event({"content": "limit-held"})
        child = {"prompt": "child", "mode": "full", "background": False}
        return tool_call("subagent", child | {"limits": {"seconds": 2}})

    with Server([route]) as server:
        started = time.monotonic()
        result = run(
            root, base_env(home, server.url), "--yolo", "-p", "delegate", timeout=30, binary=binary
        )
        finished.set()
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "limit-held", result.stdout)
        assert_true(time.monotonic() - started < budget(12), "the child outlived its limit")


def test_subagent_clamps_are_reported_not_silent(root, home, *, binary):
    """A background launch and its completion both retain host clamps."""

    def route(_, body):
        messages = body["messages"]
        if has_message(messages, "user", "child"):
            return event({"content": "clamped-child-result"})
        results = tool_results(messages)
        if any("clamped-child-result" in result for result in results):
            report = next(
                result for result in reversed(results) if "clamped-child-result" in result
            )
            assert_true("clamped limits.seconds to 2" in report, report)
            return event({"content": "clamp-reported-ok"})
        if launched(results):
            receipt = next(result for result in results if STARTED in result)
            assert_true("clamped limits.seconds to 2" in receipt, receipt)
            assert_true("produced no result envelope" not in receipt, receipt)
            return wait_round()
        return tool_call(
            "subagent",
            {"prompt": "child", "background": True, "limits": {"seconds": 600}},
        )

    with Server([route]) as server:
        env = base_env(home, server.url)
        env["UAGENT_SUBAGENT_TIMEOUT"] = "2"
        result = run(root, env, "--yolo", "-p", "delegate", timeout=30, binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "clamp-reported-ok", result.stdout)


def start_peer(root, home, binary, *flags):
    """A saved session in a folder, made to look served by a runtime."""
    workspace = root / "peers"
    workspace.mkdir()
    with Server([event({"content": "b-first"})]) as server:
        result = run(workspace, base_env(home, server.url), *flags, "-p", "b", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
    [peer] = session_files(home)
    runtime = pathlib.Path(f"/tmp/uagent-{os.getuid()}-{fnv1a64(str(home / '.uagent'))}")
    runtime.mkdir(mode=0o700, exist_ok=True)
    (runtime / f"{fnv1a64(str(peer))}.sock").touch()
    return workspace, peer


def message_peer(workspace, home, peer, binary, *args):
    """A second session lists its peers and messages the one it finds;
    returns that last tool result."""
    seen = {}

    def route(_, body):
        results = tool_results(body["messages"])
        if not results:
            return tool_call("session", {})
        if len(results) == 1:
            return tool_call(
                "session_message",
                {
                    "session_id": fnv1a64(str(peer)),
                    "prompt": "ping from a",
                },
                call_id="send",
            )
        seen["result"] = results[-1]
        return event({"content": "a-done"})

    with Server([route]) as server:
        result = run(workspace, base_env(home, server.url), *args, "-p", "a", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
    return seen["result"]


def resume_peer(workspace, home, peer, binary, *args):
    """The peer takes its next turn; returns what its model was sent."""
    later = time.time() + 100
    os.utime(peer, (later, later))
    sent = []

    def route(_, body):
        sent.append(json.dumps(body["messages"]))
        return event({"content": "b-again"})

    with Server([route]) as server:
        result = run(workspace, base_env(home, server.url), *args, "-c", "-p", "b", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
    return "".join(sent)


def test_yolo_sessions_in_a_folder_message_each_other(root, home, *, binary):
    workspace, peer = start_peer(root, home, binary, "--yolo")
    sent = message_peer(workspace, home, peer, binary, "--yolo")
    assert_true("sent to session" in sent, sent)
    seen = resume_peer(workspace, home, peer, binary, "--yolo")
    assert_true("ping from a" in seen and "[session" in seen, seen)
    assert_true(not list((home / ".uagent" / "mail").glob("*/new/*.json")), "mail left")


def test_a_peer_in_ask_mode_does_not_take_a_yolo_session_mail(root, home, *, binary):
    workspace, peer = start_peer(root, home, binary)
    sent = message_peer(workspace, home, peer, binary, "--yolo")
    assert_true("it arrives while that session is in yolo mode" in sent, sent)
    seen = resume_peer(workspace, home, peer, binary)
    assert_true("approval ask" in seen and "ping from a" not in seen, seen)
    assert_true(len(list((home / ".uagent" / "mail").glob("*/new/*.json"))) == 1, "mail kept")


def test_a_delegated_child_in_yolo_reaches_no_peer(root, home, *, binary):
    workspace, peer = start_peer(root, home, binary, "--yolo")
    reached = []

    def route(_, body):
        messages = body["messages"]
        results = tool_results(messages)
        if "child task" in user_texts(messages):
            if not results:
                return tool_call(
                    "session_message",
                    {
                        "session_id": fnv1a64(str(peer)),
                        "prompt": "from the child",
                    },
                )
            reached.append(results[-1])
            return event({"content": "child-done"})
        if results:
            return event({"content": "parent-done"})
        return tool_call("subagent", {"prompt": "child task", "background": False})

    with Server([route]) as server:
        result = run(workspace, base_env(home, server.url), "--yolo", "-p", "a", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
    assert_true(reached and "not linked" in reached[0], reached)
    assert_true(not list((home / ".uagent" / "mail").glob("*/new/*.json")), "mail left")
