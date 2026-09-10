"""Management controls use native storage and ordinary session workers."""

import datetime
import json
import subprocess

from integration_support import Server, assert_true, base_env, event, wait_until
from web_support import web_host


def control(binary, root, env, kind, **fields):
    request = dict(kind=kind, cwd=str(root), **fields)
    result = subprocess.run(
        [str(binary), "--control", "-"],
        input=json.dumps(request),
        text=True,
        capture_output=True,
        env=env,
        cwd=root,
        timeout=20,
    )
    assert_true(result.returncode in (0, 1), result.stderr)
    return json.loads(result.stdout)


def test_library_revisions_scopes_and_external_copy(root, home, *, binary):
    env = base_env(home, "http://127.0.0.1:1")

    def call(kind="memory", **fields):
        return control(binary, root, env, kind, **fields)

    for scope in ("global", "project"):
        item = call(
            action="set",
            key=f"{scope}/lesson",
            revision="",
            content="Keep shared helpers centralized.",
        )["item"]
        assert_true(item["scope"] == scope and item["provenance"]["automatic"] is False, item)
        assert_true(
            call(action="set", key=item["key"], revision="", content="stale")["conflict"],
            "overwrite without revision",
        )
        updated = call(
            action="set", key=item["key"], revision=item["revision"], content="New lesson"
        )["item"]
        assert_true(updated["revision"] != item["revision"], updated)
        assert_true(
            call(
                action="rename",
                key=item["key"],
                revision=updated["revision"],
                target=f"{scope}/renamed",
            )["item"]["content"]
            == "New lesson",
            "rename failed",
        )
    foreign = home / ".codex/memories"
    foreign.mkdir(parents=True)
    (foreign / "reference.md").write_text("external lesson")
    item = call(action="get", key="codex/reference")["item"]
    assert_true(not item["writable"], item)
    assert_true(
        call(action="copy", key=item["key"], revision=item["revision"], target="global/copied")[
            "item"
        ]["content"]
        == "external lesson",
        "external copy failed",
    )
    skill = call(
        "skills",
        action="set",
        key="project/review",
        revision="",
        content="---\ndescription: Review changes\n---\nCheck the diff.",
    )["item"]
    assert_true(skill["status"] == "available" and skill["writable"], skill)
    call("skills", action="disable", key=skill["key"])
    assert_true(
        call("skills", action="get", key=skill["key"])["item"]["status"] == "disabled",
        "exclusion not persisted",
    )
    assert_true(
        "error" in call(action="set", key="global/../escape", revision="", content="invalid"),
        "path traversal",
    )
    managed = home / ".uagent/skills"
    managed.mkdir(exist_ok=True)
    (managed / "linked").symlink_to(root, target_is_directory=True)
    assert_true(
        "error"
        in call(
            "skills",
            action="set",
            key="global/linked",
            revision="",
            content="---\ndescription: Bad\n---\n",
        ),
        "symlink followed",
    )
    assert_true(not (root / "SKILL.md").exists(), "wrote outside managed skill directory")


def test_schedule_calendar_dst_and_conflicts(root, home, *, binary):
    env = base_env(home, "http://127.0.0.1:1")

    def call(**fields):
        return control(binary, root, env, "schedule", **fields)

    def stamp(value):
        return int(datetime.datetime.fromisoformat(value).timestamp())

    rule = dict(type="weekly", days=[0], time="02:30", timezone="Europe/Zurich")
    times = call(action="preview", schedule=rule, after=stamp("2026-03-28T00:00:00+00:00"))["times"]
    assert_true(times[0] == stamp("2026-04-05T00:30:00+00:00"), times)
    times = call(action="preview", schedule=rule, after=stamp("2026-10-24T00:00:00+00:00"))["times"]
    assert_true(times[0] == stamp("2026-10-25T00:30:00+00:00"), times)
    times = call(action="preview", schedule=rule, after=stamp("2026-10-25T00:45:00+00:00"))["times"]
    assert_true(times[0] == stamp("2026-11-01T01:30:00+00:00"), times)
    task = dict(
        name="Review",
        prompt="Review the repository.",
        cwd=str(root),
        permissions="prompt",
        environment="local",
        schedule=dict(type="interval", seconds=3600),
    )
    item = call(action="save", revision="", task=task)["item"]
    assert_true(
        call(action="save", key=item["id"], revision="old", task=task)["conflict"], "stale edit"
    )
    assert_true("error" in call(action="run", key=item["id"]), "offline run accepted")
    assert_true(
        "error" in call(action="preview", schedule=dict(type="interval", seconds=0)),
        "invalid interval accepted",
    )
    paused = call(action="pause", key=item["id"], revision=item["revision"])["item"]
    assert_true(not paused["enabled"], paused)
    assert_true(
        call(action="forget", key=item["id"], revision=item["revision"])["conflict"],
        "stale deletion",
    )
    call(action="forget", key=item["id"], revision=paused["revision"])
    assert_true(call()["tasks"] == [], "task not deleted")


def test_scheduled_run_native_session_and_restart(root, home, *, binary):
    with Server([lambda _, _body: event({"content": "Scheduled review complete"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, process, env):
            client.pair(code)
            task = dict(
                name="Scheduled review",
                prompt="Inspect the workspace",
                cwd=str(root),
                environment="local",
                permissions="yolo",
                schedule=dict(type="interval", seconds=3600),
            )
            response = client.command("schedule", action="save", revision="", task=task)
            assert_true(response.get("accepted"), response)
            item = response["result"]["item"]
            # CLI changes feed the same store observed by the host.
            launched = control(binary, root, env, "schedule", action="run", key=item["id"])["run"]
            duplicate = control(binary, root, env, "schedule", action="run", key=item["id"])
            assert_true("error" in duplicate, duplicate)

            def finished():
                state = client.command("schedule", action="list")["result"]
                return next(
                    (
                        run
                        for run in state["runs"]
                        if run["id"] == launched["id"] and run["status"] == "completed"
                    ),
                    None,
                )

            wait_until(finished, lambda: client.command("schedule", action="list"), timeout=20)
            snapshot = client.snapshot(dict(id=launched["session_id"]))
            assert_true("Scheduled review complete" in json.dumps(snapshot), snapshot)
            assert_true(len(provider.requests) == 1, provider.requests)
        path = home / ".uagent/scheduled/state.json"
        stored = json.loads(path.read_text())
        stored["runs"][0]["status"] = "running"
        path.write_text(json.dumps(stored))
        with web_host(binary, root, home, provider.url) as (client, code, process, env):
            client.pair(code)
            wait_until(
                lambda: (
                    client.command("schedule", action="list")["result"]["runs"][0]["status"]
                    == "interrupted"
                ),
                "restart did not preserve interruption",
                timeout=10,
            )
            assert_true(len(provider.requests) == 1, "restart submitted the same run again")


def test_schedule_worktree_and_failed_turn(root, home, *, binary):
    def git(*args):
        subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True)

    git("init", "-q")
    (root / "baseline.txt").write_text("committed baseline")
    git("add", "baseline.txt")
    git(
        "-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-qm", "baseline"
    )
    with Server([lambda _, _body: event({"content": "Worktree verified"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _process, _env):
            client.pair(code)
            task = dict(
                name="Worktree test",
                prompt="Inspect the worktree",
                cwd=str(root),
                environment="worktree",
                permissions="yolo",
                schedule=dict(type="interval", seconds=3600),
            )
            item = client.command("schedule", action="save", task=task, revision="")["result"][
                "item"
            ]
            run = client.command("schedule", action="run", key=item["id"])["result"]["run"]

            def done():
                state = client.command("schedule", action="list")["result"]
                return next(
                    (
                        entry
                        for entry in state["runs"]
                        if entry["id"] == run["id"] and entry["status"] == "completed"
                    ),
                    None,
                )

            wait_until(done, "worktree run did not complete", timeout=20)
            worktree = home / ".uagent/worktrees" / run["id"]
            assert_true(
                (worktree / "baseline.txt").read_text() == "committed baseline", "missing baseline"
            )
            assert_true(
                str(worktree) in json.dumps(provider.requests), "worker did not use the worktree"
            )
            git("worktree", "remove", "--force", str(worktree))

    from integration_support import write_json_response

    def rejected(handler, _body):
        write_json_response(
            handler,
            {"error": {"message": "unsupported model", "type": "invalid_request_error"}},
            status=400,
        )

    with Server([rejected]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _process, _env):
            client.pair(code)
            task.update(name="Rejected run", environment="local")
            item = client.command("schedule", action="save", task=task, revision="")["result"][
                "item"
            ]
            run = client.command("schedule", action="run", key=item["id"])["result"]["run"]
            wait_until(
                lambda: any(
                    entry["id"] == run["id"] and entry["status"] == "failed"
                    for entry in client.command("schedule", action="list")["result"]["runs"]
                ),
                "failed provider response was reported as completion",
                timeout=20,
            )
