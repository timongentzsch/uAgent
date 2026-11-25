import json
import os
import threading
import time

from integration_support import (
    Server,
    assert_true,
    base_env,
    event,
    function_names,
    run,
    run_pty,
    tool_call,
    wait_until,
    write_json_response,
    write_session,
)
from memory_fixture import global_memory_dir, project_memory_dir

# Background extraction leaves a session alone for six idle hours.
IDLE_SECONDS = 7 * 60 * 60


def age(path, seconds=IDLE_SECONDS):
    stamp = time.time() - seconds
    os.utime(path, (stamp, stamp))


def test_memory_reaches_context_by_scope(root, home, *, binary):
    workspace = root / "memory-workspace"
    workspace.mkdir()
    project_dir = project_memory_dir(home, workspace)
    project_dir.mkdir(parents=True)
    (project_dir / "build.md").write_text("project-memory-sentinel", encoding="utf-8")
    global_dir = global_memory_dir(home)
    global_dir.mkdir(parents=True, exist_ok=True)
    (global_dir / "style.md").write_text("global-memory-sentinel", encoding="utf-8")
    other = root / "memory-other-workspace"
    other.mkdir()

    marker = "[memory index; non-authoritative metadata]"

    def memory_context(body):
        # Memory rides with the runtime context, never in message zero, which
        # is the cached prefix. A workspace without project memories may carry
        # only the always-on block.
        messages = body["messages"]
        for message in messages[1:]:
            content = str(message.get("content", ""))
            for start in (marker, "[always-on behavioral memory"):
                if start in content:
                    return messages, content[content.index(start) :]
        return messages, ""

    def verify(_, body):
        messages, memories = memory_context(body)
        system = str(messages[0].get("content", ""))
        valid = (
            bool(memories)
            and marker not in system
            and "global-memory-sentinel" not in system
            and "global/style" in memories
            and "[always-on behavioral memory; non-authoritative evidence]" in memories
            and "global-memory-sentinel" in memories
            # This repository's project memories carry their opening line;
            # another workspace never sees them (verify_isolated).
            and "- project/build: project-memory-sentinel" in memories
            # Merged with the user message after it, as role-alternating
            # providers do, the note still says where it ends.
            and memories.endswith("[end of harness context]")
            and messages[0].get("role") == "system"
        )
        return event({"content": "memory-ok" if valid else "memory-bad"})

    def verify_isolated(_, body):
        _, memories = memory_context(body)
        valid = (
            "global/style" in memories
            and "project/build" not in memories
            and "global-memory-sentinel" in memories
        )
        return event({"content": "isolated-ok" if valid else "isolated-bad"})

    with Server([verify, verify_isolated]) as server:
        env = base_env(home, server.url)
        result = run(workspace, env, "-p", "reply", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "memory-ok", result.stdout)
        elsewhere = run(other, env, "-p", "reply", binary=binary)
        assert_true(elsewhere.returncode == 0, elsewhere.stderr)
        assert_true(elsewhere.stdout.strip() == "isolated-ok", elsewhere.stdout)


def test_memory_redacts_credentials(root, home, *, binary):
    workspace = root / "redact-workspace"
    workspace.mkdir()
    # Global memories carry their body into the request; project ones are
    # listed by name only, so only a global memory exercises the redactor here.
    global_dir = global_memory_dir(home)
    global_dir.mkdir(parents=True, exist_ok=True)
    (global_dir / "creds.md").write_text(
        "passwd=redact-passwd-sentinel\nharmless value 42\n",
        encoding="utf-8",
    )

    def verify(_, body):
        # Redaction has to hold wherever the memory rides, so this reads the
        # whole request rather than one message.
        sent = "\n".join(str(message.get("content", "")) for message in body["messages"])
        valid = (
            "redact-passwd-sentinel" not in sent
            and "[REDACTED]" in sent
            and "harmless value 42" in sent
        )
        return event({"content": "redact-ok" if valid else "redact-bad"})

    with Server([verify]) as server:
        env = base_env(home, server.url)
        result = run(workspace, env, "-p", "reply", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "redact-ok", result.stdout)


def test_memory_background_extractor_is_bounded(root, home, *, binary):
    workspace = root / "memory-extract-workspace"
    workspace.mkdir()
    session = write_session(
        home,
        "extract",
        [
            {"role": "system", "content": "system-content-must-not-leak"},
            {"role": "user", "content": "memory-extract-user-sentinel"},
            {"role": "assistant", "content": "first answer"},
            {
                "role": "user",
                "content": "Earlier context\n"
                + "長い行\n" * 7000
                + "passwd=excerpt-secret\nCorrection: please keep fixes concise.",
            },
            {"role": "assistant", "content": "understood"},
        ],
        cwd=workspace,
        session_id="memory-extract-test",
        turns=2,
        title="durable preference",
    )
    age(session)
    target = project_memory_dir(home, workspace) / "extracted.md"

    def extract(_, body):
        text = json.dumps(body)
        evidence = body["messages"][-1]["content"].split("<filtered_session>\n")[-1]
        transcript = evidence.removesuffix("</filtered_session>")
        rows = [json.loads(line) for line in transcript.splitlines()]
        valid = (
            not function_names(body)
            and "memory-extract-user-sentinel" in text
            and "system-content-must-not-leak" not in text
            and "excerpt-secret" not in text
            and len(transcript.encode()) <= 32 * 1024
            and any(
                row["role"] == "user" and "Correction: please keep fixes concise." in row["content"]
                for row in rows
            )
            and "at most one durable" in text
        )
        if not valid:
            return event({"content": "none"})
        return event({"content": "key: project/extracted\nKeep repository fixes concise."})

    with Server([extract]) as server:
        env = base_env(home, server.url)

        def extracted():
            markers = list((home / ".uagent/memory/.processed").rglob("*.state"))
            return target.exists() and any(
                marker.read_text(encoding="utf-8").partition("\n")[0] == "done"
                for marker in markers
            )

        code, output = run_pty(
            workspace,
            env,
            b"/q\n",
            before_payload=lambda: wait_until(
                extracted, "background memory extraction did not finish", timeout=5
            ),
            binary=binary,
        )
        assert_true(code == 0, output)
        assert_true(target.read_text(encoding="utf-8") == "Keep repository fixes concise.", target)
        assert_true(len(server.requests) == 1, server.requests)
        assert_true(b"memory-extract-user-sentinel" not in output, output)
        assert_true(b"Activity result" not in output, output)
        # The extractor's own pass is not saved as a conversation of the folder.
        saved = [path.read_text() for path in (home / ".uagent/history").rglob("*.json")]
        assert_true(not any("memory extraction" in text for text in saved), saved)

        # A completed source is not processed again, and disabling generation
        # keeps a changed one ineligible. The runtime decides about extraction
        # before it first reports a state, which is what "Ready" waits for.
        def states():
            return [
                marker.read_text(encoding="utf-8").partition("\n")[0]
                for marker in (home / ".uagent/memory/.processed").rglob("*.state")
            ]

        code, output = run_pty(workspace, env, b"/q\n", startup_marker=b"Ready", binary=binary)
        assert_true(code == 0 and len(server.requests) == 1 and states() == ["done"], output)
        # A different idle source revision would be eligible with generation on.
        age(session, IDLE_SECONDS + 60)
        disabled = dict(env)
        disabled["UAGENT_MEMORY_GENERATE"] = "0"
        code, output = run_pty(workspace, disabled, b"/q\n", startup_marker=b"Ready", binary=binary)
        assert_true(code == 0 and len(server.requests) == 1 and states() == ["done"], output)


def test_memory_extraction_preserves_changed_and_unseen_notes(root, home, *, binary):
    # Each provider reply lands after the corresponding user edit. Equal-length
    # replacements exercise the document revision rather than just its size.
    def exercise(scenario):
        crowded = not scenario.startswith("index")
        initial = {
            "a": "a" * 20000,
            "b": "original",
            "c": "c" * 20000 if crowded else "A short heading\nKeep this handwritten instruction.",
        }
        workspace = root / scenario
        workspace.mkdir()
        case_home = home / scenario
        source = write_session(
            case_home,
            "extract",
            [{"role": "user", "content": "a lesson"}, {"role": "assistant", "content": "noted"}],
            cwd=workspace,
            turns=2,
        )
        age(source)
        directory = project_memory_dir(case_home, workspace)
        directory.mkdir(parents=True)
        expected = dict(initial)
        for key, body in initial.items():
            (directory / f"{key}.md").write_text(body)

        def answer(_, request):
            evidence = request["messages"][-1]["content"]
            blocks = evidence.split("</memories>\n", 1)[1].split("<filtered_session>", 1)[0]
            assert len(blocks.encode()) <= 32 * 1024
            if crowded:
                assert '<memory key="project/a">\n' + initial["a"] + "\n</memory>" in blocks
                assert (
                    '<memory key="project/b">' in blocks
                    and '<memory key="project/c">' not in blocks
                )
            else:
                assert not blocks and "handwritten instruction" not in evidence
            assert "project/c" in evidence
            target, merge = "b", ""
            if scenario == "index-partial":
                target = "c"
            if scenario == "index-complete":
                expected[target] = "merged lesson"
            if scenario in ("edited", "merge-edited"):
                expected["b"] = "new text"
                (directory / "b.md").write_text(expected["b"])
            if scenario == "deleted":
                del expected["b"]
                (directory / "b.md").unlink()
            if scenario == "created":
                target = "new"
                expected[target] = "written by user"
                (directory / "new.md").write_text(expected[target])
            if scenario.startswith("merge"):
                target = "new"
                merge = "merge: project/" + ("c" if scenario == "merge-unseen" else "b") + "\n"
            if scenario == "merge":
                del expected["b"]
                expected[target] = "merged lesson"
            return event({"content": f"key: project/{target}\n{merge}merged lesson"})

        with Server([answer]) as server:
            env = base_env(case_home, server.url)
            env.update(UAGENT_MEMORY_MAX="3" if crowded else "8", UAGENT_MEMORY_BYTES="32768")
            markers = case_home / ".uagent/memory/.processed"
            code, output = run_pty(
                workspace,
                env,
                b"/q\n",
                binary=binary,
                before_payload=lambda: wait_until(
                    lambda: any(
                        m.read_text().partition("\n")[0] == "done" for m in markers.rglob("*.state")
                    ),
                    f"{scenario} did not settle",
                ),
            )
            assert_true(code == 0 and len(server.requests) == 1, output)
        assert_true({p.stem: p.read_text() for p in directory.glob("*.md")} == expected, scenario)
        if scenario == "merge":
            events = [
                json.loads(line)
                for line in (directory.parents[1] / "events.jsonl").read_text().splitlines()
            ]
            removed = next(evt for evt in events if evt["action"] == "deleted")
            assert_true(removed["automatic"] and removed["source_session"], removed)

    for scenario in (
        "edited",
        "deleted",
        "created",
        "index-partial",
        "index-complete",
        "merge-edited",
        "merge-unseen",
        "merge",
    ):
        exercise(scenario)


def test_memory_background_extractor_releases_failed_claims(root, _home, *, binary):
    def scenario(name, kinds=None):
        case_home = root / f"memory-{name}-home"
        workspace = root / f"memory-{name}-workspace"
        workspace.mkdir()
        session = write_session(
            case_home,
            "extract",
            [
                {"role": "user", "content": f"remember-{name}"},
                {"role": "assistant", "content": "understood"},
            ],
            cwd=workspace,
            kinds=kinds,
            session_id=f"memory-{name}",
            turns=2,
            title=name,
        )
        age(session)
        return case_home, workspace

    def markers(case_home):
        return list((case_home / ".uagent/memory/.processed").rglob("*.state"))

    no_write_home, no_write_workspace = scenario("no-write")
    with Server([event({"content": "Nothing durable to save."})]) as server:
        env = base_env(no_write_home, server.url)

        def wait_for_done():
            wait_until(
                lambda: any(
                    marker.read_text(encoding="utf-8").partition("\n")[0] == "done"
                    for marker in markers(no_write_home)
                ),
                "successful no-write extraction did not finish",
            )

        code, output = run_pty(
            no_write_workspace, env, b"/q\n", before_payload=wait_for_done, binary=binary
        )
        assert_true(code == 0, output)
        assert_true(len(server.requests) == 1, server.requests)
        assert_true(not list((no_write_home / ".uagent/memory").rglob("*.md")), no_write_home)

    def overfill_globals(case_home):
        """More global memory than the always-on slice can carry: the
        truncation warning must never stand in for the real error."""
        globals_dir = global_memory_dir(case_home)
        globals_dir.mkdir(parents=True, exist_ok=True)
        for index in range(8):
            (globals_dir / f"planted_{index}.md").write_text(
                f"Standing preference {index}. " + "padding " * 40, encoding="utf-8"
            )

    def run_cleanup_case(name, responder, kinds=None, wait_for_request=False):
        case_home, workspace = scenario(name, kinds)
        overfill_globals(case_home)
        trace = root / f"memory-{name}.jsonl"
        with Server([responder]) as server:
            env = base_env(case_home, server.url)
            env["UAGENT_REQUEST_TIMEOUT"] = "1"
            env["UAGENT_STREAM_TIMEOUT"] = "1"

            def completion_logged():
                return trace.exists() and '"event":"memory_extract_finished"' in trace.read_text(
                    encoding="utf-8"
                )

            def wait_for_cleanup():
                if wait_for_request:
                    wait_until(lambda: bool(server.requests), f"{name} request did not start")
                if name == "terminated":
                    wait_until(
                        lambda: any(
                            marker.read_text(encoding="utf-8").strip() == "processing"
                            for marker in markers(case_home)
                        ),
                        "termination case did not retain its live claim",
                    )
                else:
                    wait_until(completion_logged, f"{name} extractor did not complete")

            code, output = run_pty(
                workspace,
                env,
                b"/q\n",
                args=(f"--debug={trace}",),
                before_payload=wait_for_cleanup,
                timeout=40,
                binary=binary,
            )
            assert_true(code == 0, output)
            if name != "terminated":
                assert_true(completion_logged(), f"{name} extractor did not complete")
                wait_until(lambda: not markers(case_home), f"{name} claim survived shutdown")
                journal = case_home / ".uagent/memory/events.jsonl"
                previews = [
                    json.loads(line).get("preview", "")
                    for line in journal.read_text(encoding="utf-8").splitlines()
                    if line.strip() and json.loads(line).get("action") == "failed"
                ]
                assert_true(previews, f"{name} recorded no failure to explain")
                for preview in previews:
                    assert_true(
                        "memory context truncated" not in preview,
                        f"{name} recorded a startup warning as the failure: {preview!r}",
                    )
                return server.requests

            # A request cut off by shutdown leaves its claim behind. It must
            # say `processing`, which is reclaimable: `done` would skip the
            # session.
            try:
                wait_until(lambda: not markers(case_home), "claim released", timeout=10)
                released = True
            except AssertionError:
                released = False
            for marker in markers(case_home):
                state = marker.read_text(encoding="utf-8").strip()
                assert_true(state == "processing", f"killed extractor claimed {state!r}")
            # On a plain build the child always wins. A sanitizer leg is known
            # by the options its preset sets: every CI leg stretches deadlines.
            sanitized = any(name in os.environ for name in ("ASAN_OPTIONS", "TSAN_OPTIONS"))
            assert_true(released or sanitized, "claim survived shutdown")
            return server.requests

    def fail_request(handler, _):
        # This case owns claim cleanup, not transient-retry timing. A terminal
        # model error keeps the completion bound deterministic under ASan.
        write_json_response(handler, {"error": {"message": "extract failed"}}, status=400)

    failed_requests = run_cleanup_case("model-failure", fail_request, wait_for_request=True)
    assert_true(failed_requests, "model failure did not reach the provider")

    # One kind for two messages: a session the agent would never write, which
    # must be rejected before it reaches the provider.
    invalid_requests = run_cleanup_case(
        "invalid-session", event({"content": "unused"}), kinds=["user"]
    )
    assert_true(not invalid_requests, invalid_requests)

    request_started = threading.Event()

    def block_request(_, __):
        request_started.set()
        time.sleep(20)
        return event({"content": "too late"})

    run_cleanup_case("terminated", block_request, wait_for_request=True)
    assert_true(request_started.is_set(), "termination case never entered the provider request")


def test_no_memory_hides_index_and_tool(root, home, *, binary):
    workspace = root / "no-memory-workspace"
    workspace.mkdir()
    project_dir = project_memory_dir(home, workspace)
    project_dir.mkdir(parents=True)
    (project_dir / "project.md").write_text("project-memory-sentinel", encoding="utf-8")
    global_dir = global_memory_dir(home)
    global_dir.mkdir(parents=True, exist_ok=True)
    (global_dir / "global.md").write_text("global-memory-sentinel", encoding="utf-8")

    def verify(_, body):
        text = json.dumps(body)
        names = function_names(body)
        clean = (
            "[memory index;" not in text and "memory-sentinel" not in text and "memory" not in names
        )
        return event({"content": "no-memory-ok" if clean else "no-memory-bad"})

    with Server([verify]) as server:
        result = run(
            workspace, base_env(home, server.url), "--no-memory", "-p", "inspect", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "no-memory-ok", result.stdout)


def test_memory_index_goes_where_the_tool_goes(root, home, *, binary):
    """An agent that may not edit has no memory tool and gets no index; globals
    too large for the always-on slice stay in the index without a warning."""
    workspace = root / "index-workspace"
    workspace.mkdir()
    project_dir = project_memory_dir(home, workspace)
    project_dir.mkdir(parents=True)
    (project_dir / "deploy.md").write_text("Deploy with make ship.", encoding="utf-8")
    global_dir = global_memory_dir(home)
    global_dir.mkdir(parents=True, exist_ok=True)
    for name in ("first", "second", "third"):
        (global_dir / f"{name}.md").write_text(f"{name} preference. " + "x" * 900)
    seen = {}

    def route(_, body):
        text = json.dumps(body)
        if "child-brief" in text and "child" not in seen:
            seen["child"] = ("[memory index;" in text, "memory" in function_names(body))
            return event({"content": "child-done"})
        if "parent" not in seen:
            seen["parent"] = "project/deploy: Deploy with make ship" in text
            return tool_call("agent_start", {"brief": "child-brief"})
        return event({"content": "parent-done"})

    with Server([route]) as server:
        result = run(workspace, base_env(home, server.url), "--yolo", "-p", "go", binary=binary)
    assert_true(result.returncode == 0, result.stderr)
    assert_true(seen.get("parent") is True, seen)
    assert_true(seen.get("child") == (False, False), seen)
    assert_true("memory context truncated" not in result.stderr, result.stderr)


def test_extraction_only_commits_the_current_source_revision(root, home, *, binary):
    source = write_session(
        home, "source", [{"role": "user", "content": "initial evidence"}], cwd=root, turns=2
    )
    age(source)
    processed = home / ".uagent/memory/.processed"
    notes = project_memory_dir(home, root)

    def replace_source(content, enabled=True):
        # A real save replaces the inode. Keep the old timestamp to exercise
        # both an in-flight change and a completed marker for another revision.
        stamp = source.stat().st_mtime_ns
        staged = write_session(
            home,
            "staged",
            [{"role": "user", "content": content}],
            cwd=root,
            turns=2,
            memory=enabled,
        )
        os.utime(staged, ns=(stamp, stamp))
        staged.replace(source)

    def changed(_, body):
        assert_true("initial evidence" in json.dumps(body), body)
        replace_source("revised evidence")
        return event({"content": "key: project/stale\nStale advice."})

    def current(_, body):
        assert_true("revised evidence" in json.dumps(body), body)
        return event({"content": "key: project/current\nCurrent advice."})

    def now_private(_, body):
        assert_true("replacement evidence" in json.dumps(body), body)
        replace_source("private evidence", enabled=False)
        return event({"content": "key: project/private\nPrivate advice."})

    with Server([changed, current, now_private]) as server:
        env = base_env(home, server.url)

        def start(expected, settled):
            def wait():
                wait_until(
                    lambda: len(server.requests) == expected and settled(), "extraction failed"
                )

            code, output = run_pty(root, env, b"/q\n", before_payload=wait, binary=binary)
            assert_true(code == 0, output)

        start(1, lambda: not list(processed.rglob("*.state")))
        assert_true(not (notes / "stale.md").exists(), "outdated reply was saved")
        start(2, lambda: (notes / "current.md").exists())
        code, output = run_pty(root, env, b"/q\n", startup_marker=b"Ready", binary=binary)
        assert_true(code == 0 and len(server.requests) == 2, output)
        replace_source("replacement evidence")
        start(3, lambda: not list(processed.rglob("*.state")))
        assert_true(not (notes / "private.md").exists(), "opted-out reply was saved")
        code, output = run_pty(root, env, b"/q\n", startup_marker=b"Ready", binary=binary)
        assert_true(code == 0 and len(server.requests) == 3, output)


def test_memory_off_survives_resume_and_future_extraction(root, home, *, binary):
    # Include a previously eligible conversation resumed with --no-memory.
    for name, off_at in (("private-first", 0), ("private-later", 1)):
        workspace = root / name
        workspace.mkdir()
        with Server([lambda *_: event({"content": "noted"})]) as server:
            env = base_env(home, server.url)
            for index in range(3):
                flags = ["-c"] if index else []
                if index == off_at:
                    flags.append("--no-memory")
                result = run(workspace, env, *flags, "-p", f"turn {index}", binary=binary)
                assert_true(result.returncode == 0, result.stderr)
            sessions = [
                path
                for path in (home / ".uagent/history").glob("*.json")
                if json.loads(path.read_text().splitlines()[0])["cwd"] == str(workspace.resolve())
            ]
            assert_true(len(sessions) == 1, sessions)
            header = json.loads(sessions[0].read_text().splitlines()[0])
            assert_true(header.get("memory") is False and header["turns"] == 3, header)
            age(sessions[0])
            code, output = run_pty(workspace, env, b"/q\n", startup_marker=b"Ready", binary=binary)
            assert_true(code == 0 and len(server.requests) == 3, output)
            claimed = list((home / ".uagent/memory/.processed").rglob("*.state"))
            assert_true(not claimed, claimed)
