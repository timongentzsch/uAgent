import json
import os
import threading
import time

from integration_support import (
    TIMEOUT_SCALE,
    Server,
    assert_true,
    base_env,
    budget,
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


def test_collaborator_retention_prunes_whole_records(root, home, *, binary):
    collaborators = home / ".uagent" / "collaborators"
    collaborators.mkdir(parents=True)
    now = time.time()
    for index, stamp in (("old", now - 120), ("new", now - 60)):
        (collaborators / f"{index}.json").write_text("{}", encoding="utf-8")
        (collaborators / f"{index}.session.json").write_text("{}", encoding="utf-8")
        os.utime(collaborators / f"{index}.json", (stamp, stamp))
        os.utime(collaborators / f"{index}.session.json", (stamp, stamp))

    with Server([event({"content": "retention-ok"})]) as server:
        env = base_env(home, server.url)
        env["UAGENT_DEBUG_FILES"] = "1"
        result = run(root, env, "-p", "reply", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "retention-ok", result.stdout)

    remaining = sorted(path.name for path in collaborators.iterdir())
    assert_true(remaining == ["new.json", "new.session.json"], remaining)


def test_project_agent_config_trust(root, home, *, binary):
    workspace = root / "config-workspace"
    (workspace / ".uagent").mkdir(parents=True)
    (home / ".uagent").mkdir(exist_ok=True)
    (home / ".uagent" / ".config").write_text("UAGENT_MODEL=global/model\n", encoding="utf-8")
    (workspace / ".uagent" / ".config").write_text("UAGENT_MODEL=project/model\n", encoding="utf-8")
    server = Server([event({"content": "ok"}), event({"content": "ok"})])
    try:
        env = base_env(home, server.url)
        env.pop("UAGENT_MODEL")
        # Untrusted the workspace file is ignored, but the run still works off
        # the global config instead of failing.
        ignored = run(workspace, env, "-p", "reply", binary=binary)
        assert_true(ignored.returncode == 0, ignored.stderr)
        assert_true("untrusted" in ignored.stderr, ignored.stderr)
        assert_true(server.requests[0][1]["model"] == "global/model", server.requests[0][1])
        trusted = run(workspace, env, "--trust-project-config", "-p", "reply", binary=binary)
        assert_true(trusted.returncode == 0, trusted.stderr)
        assert_true(server.requests[1][1]["model"] == "project/model", server.requests[1][1])
    finally:
        server.close()
        # HOME is shared by every test; leave it as it was found.
        (home / ".uagent" / ".config").unlink(missing_ok=True)


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

    marker = "[memory names only; non-authoritative metadata]"

    def memory_context(body):
        # Memory rides with the runtime context, never in message zero: that
        # message is the prefix a provider caches and the place authority
        # lives, and memory is both untrusted and different in every session.
        messages = body["messages"]
        carrier = next(
            (
                str(m.get("content", ""))
                for m in messages[1:]
                if marker in str(m.get("content", ""))
            ),
            "",
        )
        memories = carrier[carrier.index(marker) :] if marker in carrier else ""
        return messages, memories

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
            and "project/build" in memories
            and "project-memory-sentinel" not in memories
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

    server = Server([verify, verify_isolated])
    try:
        env = base_env(home, server.url)
        result = run(workspace, env, "-p", "reply", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "memory-ok", result.stdout)
        elsewhere = run(other, env, "-p", "reply", binary=binary)
        assert_true(elsewhere.returncode == 0, elsewhere.stderr)
        assert_true(elsewhere.stdout.strip() == "isolated-ok", elsewhere.stdout)
    finally:
        server.close()
        # HOME is shared by every test; a global memory would join them all.
        (global_dir / "style.md").unlink(missing_ok=True)


def test_configured_redaction_keywords_apply(root, home, *, binary):
    # The keyword list is read once per process, so this needs a fresh agent
    # rather than a unit test. Configured keywords must extend the built-ins,
    # never replace them, and must be matched literally.
    workspace = root / "redact-workspace"
    workspace.mkdir()
    # Global memories carry their body into the request; project ones are
    # listed by name only, so only a global memory exercises the redactor here.
    global_dir = global_memory_dir(home)
    global_dir.mkdir(parents=True, exist_ok=True)
    (global_dir / "creds.md").write_text(
        "db_dsn=postgres://user/redact-dsn-sentinel\n"
        "passwd=redact-passwd-sentinel\n"
        "harmless value 42\n",
        encoding="utf-8",
    )

    def verify(_, body):
        # Redaction has to hold wherever the memory rides, so this reads the
        # whole request rather than one message.
        sent = "\n".join(str(message.get("content", "")) for message in body["messages"])
        valid = (
            "redact-dsn-sentinel" not in sent
            and "redact-passwd-sentinel" not in sent
            and "[REDACTED]" in sent
            and "harmless value 42" in sent
        )
        return event({"content": "redact-ok" if valid else "redact-bad"})

    server = Server([verify])
    try:
        env = base_env(home, server.url)
        # ".*" must be treated as a literal keyword, not a pattern.
        env["UAGENT_MEMORY_REDACT_KEYWORDS"] = "db_dsn, .*"
        result = run(workspace, env, "-p", "reply", binary=binary)
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "redact-ok", result.stdout)
    finally:
        server.close()
        # HOME is shared by every test; a global memory would join them all.
        (global_dir / "creds.md").unlink(missing_ok=True)


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
            {"role": "user", "content": "please keep fixes concise"},
            {"role": "assistant", "content": "understood"},
        ],
        cwd=workspace,
        session_id="memory-extract-test",
        turns=2,
        title="durable preference",
    )
    target = project_memory_dir(home, workspace) / "extracted.md"

    def extract(_, body):
        text = json.dumps(body)
        valid = (
            function_names(body) == {"memory"}
            and "memory-extract-user-sentinel" in text
            and "system-content-must-not-leak" not in text
            and "at most one durable" in text
        )
        if not valid:
            return event({"content": "extract-schema-bad"})
        return tool_call("memory", {"action": "list"})

    def search(_, _body):
        return tool_call("memory", {"action": "search", "key": "concise"})

    def inspect(_, _body):
        return tool_call("memory", {"action": "get", "key": "project/extracted"})

    def write(_, _body):
        return tool_call(
            "memory",
            {
                "action": "set",
                "key": "project/extracted",
                "content": "Keep repository fixes concise.",
            },
        )

    def finish(_, body):
        wrote = any(
            message.get("role") == "tool" and "wrote " in str(message.get("content", ""))
            for message in body["messages"]
        )
        return event({"content": "extract-done" if wrote else "extract-write-bad"})

    with Server([extract, search, inspect, write, finish]) as server:
        env = base_env(home, server.url)
        env["UAGENT_MEMORY_IDLE_SECONDS"] = "0"

        def extracted():
            markers = list((home / ".uagent/memory/.processed").rglob("*.state"))
            return target.exists() and any(
                marker.read_text(encoding="utf-8").strip() == "done" for marker in markers
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
        assert_true(len(server.requests) == 5, server.requests)
        assert_true(b"memory-extract-user-sentinel" not in output, output)
        assert_true(b"Background result" not in output, output)

        # A completed source is not processed again. Disabling generation also
        # prevents a changed source from becoming eligible.
        code, output = run_pty(
            workspace, env, b"/q\n", before_payload=lambda: time.sleep(0.2), binary=binary
        )
        assert_true(code == 0 and len(server.requests) == 5, output)
        time.sleep(0.01)
        os.utime(session, None)
        disabled = dict(env)
        disabled["UAGENT_MEMORY_GENERATE"] = "0"
        code, output = run_pty(
            workspace, disabled, b"/q\n", before_payload=lambda: time.sleep(0.2), binary=binary
        )
        assert_true(code == 0 and len(server.requests) == 5, output)


def test_memory_background_extractor_releases_failed_claims(root, _home, *, binary):
    def scenario(name, kinds=None):
        case_home = root / f"memory-{name}-home"
        workspace = root / f"memory-{name}-workspace"
        workspace.mkdir()
        write_session(
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
        return case_home, workspace

    def markers(case_home):
        return list((case_home / ".uagent/memory/.processed").rglob("*.state"))

    no_write_home, no_write_workspace = scenario("no-write")
    with Server([event({"content": "Nothing durable to save."})]) as server:
        env = base_env(no_write_home, server.url)
        env["UAGENT_MEMORY_IDLE_SECONDS"] = "0"

        def wait_for_done():
            wait_until(
                lambda: any(
                    marker.read_text(encoding="utf-8").strip() == "done"
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
        """More global memory than the always-on slice can carry.

        The extractor child then warns about truncation at startup, which is
        what makes the preview assertion below discriminate: without it the
        child prints nothing before the real error and any policy passes.
        """
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
            env["UAGENT_MEMORY_IDLE_SECONDS"] = "0"
            env["UAGENT_REQUEST_TIMEOUT"] = "1"
            env["UAGENT_FIRST_EVENT_TIMEOUT"] = "1"
            env["UAGENT_STREAM_IDLE_TIMEOUT"] = "1"

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
                # A failure preview is read to find out why, and the globals
                # planted above guarantee the child prints a truncation warning
                # before it reaches whatever actually stopped it. Recording the
                # head of its output made that warning the cause; the tail does
                # not, so the reader is not sent to consolidate memories over a
                # failure that had nothing to do with them.
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

            # Shutdown gives a background group 500ms to run its EXIT trap
            # before SIGKILL (BgShutdownAll in jobs.cc), and a shell defers
            # that trap until its foreground child is reaped -- so a child too
            # slow to unwind loses the race and the claim outlives it. That is
            # a liveness cost the stale-claim sweep reclaims after 15 minutes,
            # not a correctness one, and it is reproducible under TSan.
            #
            # The invariant that has to hold either way is what a surviving
            # claim *says*. `processing` is reclaimable; `done` is not, and a
            # killed extractor claiming completion would skip that session for
            # good.
            released = False
            deadline = time.monotonic() + budget(30)
            while time.monotonic() < deadline:
                if not markers(case_home):
                    released = True
                    break
                time.sleep(0.02)
            for marker in markers(case_home):
                state = marker.read_text(encoding="utf-8").strip()
                assert_true(state == "processing", f"killed extractor claimed {state!r}")
            # On a plain build the trap always wins, so a survivor there is a
            # real regression rather than the documented race.
            assert_true(released or TIMEOUT_SCALE > 1, "claim survived shutdown")
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
            "[memory names only;" not in text
            and "memory-sentinel" not in text
            and "memory" not in names
        )
        return event({"content": "no-memory-ok" if clean else "no-memory-bad"})

    server = Server([verify])
    try:
        result = run(
            workspace, base_env(home, server.url), "--no-memory", "-p", "inspect", binary=binary
        )
        assert_true(result.returncode == 0, result.stderr)
        assert_true(result.stdout.strip() == "no-memory-ok", result.stdout)
    finally:
        server.close()
        (global_dir / "global.md").unlink(missing_ok=True)
