"""Moving a conversation, and the agents saved beside it, to another folder."""

import fcntl
import http.client
import json
import shutil
import subprocess
import threading

from integration_support import (
    Server,
    assert_true,
    base_env,
    event,
    fnv1a64,
    run,
    tool_call,
    wait_until,
    write_session,
)
from web_support import web_host

LEAD = "lead"
AGENTS = ("agent-0001", "agent-0002", "agent-0003")


def history_of(home, folder):
    return home / ".uagent" / "history" / fnv1a64(str(folder.resolve()))


def note_for(old, new):
    old, new = old.resolve(), new.resolve()
    return f"Moved from {old} to {new}. Files stayed in {old}."


def run_git(directory, *args):
    subprocess.run(
        ["git", "-C", str(directory), "-c", "user.email=t@t", "-c", "user.name=t", *args],
        check=True,
        capture_output=True,
    )


def save_family(home, folder, root):
    """A conversation with assets and an agent of an agent, one agent working
    in a worktree of a real repository, and what sits beside the lead."""
    directory = history_of(home, folder)
    directory.mkdir(parents=True, exist_ok=True)
    directory.chmod(0o700)
    lead = directory / f"{LEAD}.json"
    one, two, tree = (directory / f"{name}.json" for name in AGENTS)
    repo = root / f"repo-{folder.name}"
    repo.mkdir(exist_ok=True)
    run_git(repo, "init", "-q")
    run_git(repo, "commit", "-q", "--allow-empty", "-m", "base")
    worktree = home / ".uagent" / "worktrees" / AGENTS[2]
    run_git(repo, "worktree", "add", "-q", "--detach", str(worktree), "HEAD")
    said = [
        {"role": "system", "content": "baseline"},
        {"role": "user", "content": "first question"},
        {
            "role": "assistant",
            "content": f"see {lead}.assets/pic.png, agent {fnv1a64(str(one))}",
        },
    ]
    facts = {"t-1": {"change_path": f"{lead}.edits/diff-1", "output": "diff"}}
    write_session(
        home,
        f"{directory.name}/{LEAD}",
        said,
        cwd=folder,
        session_id="lead-sid",
        title=LEAD,
        display={"ids": [1, 2, 3], "next": 4, "facts": facts, "statistics": {"complete": True}},
    )
    for path, parent, cwd in (
        (one, lead, folder),
        (two, one, folder),
        (tree, lead, worktree),
    ):
        write_session(
            home,
            f"{directory.name}/{path.stem}",
            [
                {"role": "system", "content": "baseline"},
                {"role": "user", "content": f"brief for {path.stem}"},
            ],
            cwd=cwd,
            display={"ids": [1, 2], "next": 3, "facts": {}, "statistics": {"complete": True}},
            session_id=f"{path.stem}-sid",
            title=path.stem,
            agent={"parent": fnv1a64(str(parent)), "edits": "folder", "depth": 1},
        )
    # As the host spells its own folders, which write_session resolves.
    head, body = tree.read_text(encoding="utf-8").split("\n", 1)
    head = json.loads(head) | {"cwd": str(worktree)}
    tree.write_text(json.dumps(head) + "\n" + body, encoding="utf-8")
    for name, text in (
        (f"{LEAD}.json.assets/pic.png", "png"),
        (f"{LEAD}.json.edits/diff-1", "diff"),
        (f"{LEAD}.json.events.jsonl", "{}\n"),
        (f"{LEAD}.json.plan.md", "plan"),
        (f"{LEAD}.share.md", "shared"),
        (f"{AGENTS[0]}.json.assets/note.txt", "note"),
    ):
        (directory / name).parent.mkdir(parents=True, exist_ok=True)
        (directory / name).write_text(text)
    return directory, repo, worktree


def private_web_folder(home):
    folder = home / ".uagent" / "web"
    folder.mkdir(parents=True, exist_ok=True)
    folder.chmod(0o700)
    return folder


def tree_of(directory):
    return sorted(
        str(path.relative_to(directory))
        for path in directory.rglob("*")
        if not path.name.endswith(".lock")
    )


def read_session(path):
    head, body = path.read_text(encoding="utf-8").split("\n", 1)
    return json.loads(head), json.loads(body)


def sessions_of(client):
    return client.json("/api/sessions?refresh=1")[1]["sessions"]


def listed(client, title):
    return next(item for item in sessions_of(client) if item["title"] == title)


def refusal(client, session, **values):
    """Why the host refused to move `session`; fails when it did not."""
    client.sequence += 1
    payload = {
        "v": 2,
        "kind": "move",
        "request_id": f"{client.sequence:032x}",
        "session_id": session["id"],
        "generation": session.get("generation", ""),
    }
    payload.update(values)
    status, result, _ = client.json("/api/command", payload)
    assert_true(status == 409 and not result["accepted"], result)
    return result["error"]


def watch(client):
    """The events the host publishes from now on, as they arrive."""
    listing = client.json("/api/sessions")[1]
    connection = http.client.HTTPConnection("127.0.0.1", client.port, timeout=60)
    connection.request(
        "GET",
        f"/api/events?cursor={listing['epoch']}:{listing['cursor']}",
        headers={"Cookie": client.cookie},
    )
    response = connection.getresponse()
    assert_true(response.status == 200, response.status)
    frames = []

    def read():
        try:
            while line := response.readline():
                if line.startswith(b"data: "):
                    frames.append(json.loads(line[6:]))
        except (OSError, http.client.HTTPException):
            pass

    threading.Thread(target=read, daemon=True).start()
    return frames


def idle(value):
    return value["metadata"]["status"] == "idle"


def text_of(body):
    return json.dumps(body["messages"])


def assert_whole(home, alpha, beta, *, notes=1):
    """The family is in the new folder, rewritten, and says it moved once."""
    old, new = history_of(home, alpha), history_of(home, beta)
    left = sorted(path.name for path in old.iterdir()) if old.exists() else []
    assert_true(not left, f"left in the old folder: {left}")
    lead = new / f"{LEAD}.json"
    ids = {name: fnv1a64(str(new / f"{name}.json")) for name in (LEAD, *AGENTS)}
    head, body = read_session(lead)
    assert_true(head["cwd"] == str(beta.resolve()), head)
    texts = [message["content"] for message in body["messages"]]
    seen = next(text for text in texts if text.startswith("see "))
    assert_true(f"{new}/{LEAD}.json.assets/pic.png" in seen and ids[AGENTS[0]] in seen, texts)
    assert_true(
        body["display"]["facts"]["t-1"]["change_path"] == f"{new}/{LEAD}.json.edits/diff-1", body
    )
    assert_true(
        sum(note_for(alpha, beta) in str(text) for text in texts) == notes,
        (notes, texts),
    )
    parents = {AGENTS[0]: LEAD, AGENTS[1]: AGENTS[0], AGENTS[2]: LEAD}
    for name, parent in parents.items():
        agent_head, _ = read_session(new / f"{name}.json")
        assert_true(agent_head["agent"]["parent"] == ids[parent], (name, agent_head))
    assert_true(read_session(new / f"{AGENTS[0]}.json")[0]["cwd"] == str(beta.resolve()), "cwd")
    assert_true(read_session(new / f"{AGENTS[1]}.json")[0]["cwd"] == str(beta.resolve()), "cwd")
    worktree = read_session(new / f"{AGENTS[2]}.json")[0]["cwd"]
    assert_true(worktree.endswith(f"/worktrees/{AGENTS[2]}"), worktree)
    return ids


def test_web_move_takes_a_conversation_and_its_agents_to_another_folder(root, home, *, binary):
    alpha, beta = root / "alpha", root / "beta"
    alpha.mkdir()
    beta.mkdir()
    old, _, _ = save_family(home, alpha, root)
    before = tree_of(old)
    with Server([lambda _, _body: event({"content": "resumed in beta"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            frames = watch(client)
            lead = listed(client, LEAD)
            assert_true(lead["id"] == fnv1a64(str(old / f"{LEAD}.json")), lead)
            moved = client.command("move", lead, cwd=str(beta))["result"]
            new = history_of(home, beta)
            ids = assert_whole(home, alpha, beta)
            assert_true(moved["id"] == ids[LEAD] and moved["cwd"] == str(beta.resolve()), moved)
            # Everything that sat beside the files came along; nothing was
            # left behind, and nothing is new but the leases.
            assert_true(tree_of(new) == before, (tree_of(new), before))
            assert_true(not list((home / ".uagent" / "web").glob("move-*.json")), "journal left")
            wait_until(
                lambda: any(f.get("moved_to") == ids[LEAD] for f in frames),
                f"no deleted frame follows the conversation: {frames}",
            )
            old_frame = next(f for f in frames if f.get("moved_to") == ids[LEAD])
            assert_true(
                old_frame["kind"] == "deleted" and old_frame["session_id"] == lead["id"], old_frame
            )
            # The agents are listed under it again, at their new ids.
            wait_until(
                lambda: (
                    {item["id"] for item in sessions_of(client) if item.get("parent") == ids[LEAD]}
                    == {ids[AGENTS[0]], ids[AGENTS[2]]}
                ),
                "the agents are not listed under the moved conversation",
            )
            assert_true(lead["id"] not in {item["id"] for item in sessions_of(client)}, "old id")
            # It resumes in the new folder, and the model is told.
            session = client.command("activate", {"id": ids[LEAD], "generation": ""})["session"]
            client.until(session, idle)
            client.command("submit", session, text="go on")
            client.until(
                session, lambda value: idle(value) and "resumed in beta" in json.dumps(value)
            )
            assert_true(len(provider.requests) == 1, len(provider.requests))
            body = provider.requests[0][1]
            sent = text_of(body)
            assert_true(note_for(alpha, beta) in sent, sent)
            assert_true(str(beta.resolve()) in sent.replace(note_for(alpha, beta), ""), sent)
            nodes = client.snapshot(session)["state"]["view"]["nodes"]
            assert_true(
                any(
                    note_for(alpha, beta) in json.dumps(node)
                    for node in nodes
                    if node["kind"] == "notice"
                ),
                nodes,
            )


def test_web_move_of_a_conversation_made_in_the_web_closes_it_and_leaves_no_draft(
    root, home, *, binary
):
    alpha, beta = root / "alpha", root / "beta"
    alpha.mkdir()
    beta.mkdir()
    drafts = home / ".uagent" / "web" / "drafts"
    with Server([lambda _, _body: event({"content": "said"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            cookie = client.cookie
            session = client.create(alpha)
            client.command("submit", session, text="hello")
            client.until(session, lambda value: idle(value) and "said" in json.dumps(value))
            assert_true(len(list(drafts.glob("*.json"))) == 1, "no draft was written")
            # Still running, idle: the move closes it first.
            running = client.snapshot(session)["metadata"]
            assert_true(running["presence"] == "active", running)
            moved = client.command("move", running, cwd=str(beta))["result"]
            assert_true(moved["id"] != session["id"], moved)
            assert_true(not list(drafts.glob("*.json")), "the draft is left")
        with web_host(binary, root, home, provider.url) as (again, _, _, _):
            again.cookie = cookie
            ids = [item["id"] for item in sessions_of(again)]
            assert_true(ids == [moved["id"]], (ids, moved))
            assert_true(sessions_of(again)[0]["cwd"] == str(beta.resolve()), sessions_of(again))


def test_web_move_makes_the_terminal_continue_from_the_new_folder(root, home, *, binary):
    alpha, beta = root / "alpha", root / "beta"
    alpha.mkdir()
    beta.mkdir()
    save_family(home, alpha, root)
    with Server([lambda _, _body: event({"content": "resumed"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            client.command("move", listed(client, LEAD), cwd=str(beta))
        env = base_env(home, provider.url)
        elsewhere = run(alpha, env, "-c", "-p", "go on", binary=binary)
        assert_true("no saved conversation" in elsewhere.stderr, elsewhere)
        assert_true("first question" not in text_of(provider.requests[0][1]), "continued")
        here = run(beta, env, "-c", "-p", "go on", binary=binary)
        assert_true(here.returncode == 0 and here.stdout.strip() == "resumed", here)
        sent = text_of(provider.requests[1][1])
        assert_true("first question" in sent and note_for(alpha, beta) in sent, sent)


def test_web_move_refuses_what_it_could_not_do_safely(root, home, *, binary):
    alpha, beta = root / "alpha", root / "beta"
    alpha.mkdir()
    beta.mkdir()
    old, _, _ = save_family(home, alpha, root)
    before = tree_of(old)
    gate, entered = threading.Event(), threading.Event()

    def route(_, _body):
        entered.set()
        gate.wait(30)
        return event({"content": "done"})

    with Server([route]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            lead = listed(client, LEAD)
            assert_true("already" in refusal(client, lead, cwd=str(alpha)), "same folder")
            assert_true("agent" in refusal(client, listed(client, AGENTS[0]), cwd=str(beta)), "")
            draft = client.create(alpha, activate=False)
            assert_true("nothing saved" in refusal(client, draft, cwd=str(beta)), "draft")
            assert_true("accessible" in refusal(client, lead, cwd=str(root / "none")), "folder")
            # Mail waiting for a member of the family.
            mailbox = (
                home / ".uagent" / "mail" / fnv1a64(str((old / f"{AGENTS[1]}.json").resolve()))
            )
            (mailbox / "new").mkdir(parents=True)
            (mailbox / "new" / "0000000000001-0-0-note.json").write_text(
                json.dumps(
                    {
                        "format": 1,
                        "id": "note",
                        "from": "x",
                        "to": mailbox.name,
                        "sender_path": "",
                        "hops": 0,
                        "created_ms": 1,
                        "expires_ms": 0,
                        "body": {"text": "hi"},
                    }
                )
            )
            assert_true("mail is waiting" in refusal(client, lead, cwd=str(beta)), "mail")
            shutil.rmtree(mailbox)
            # Held by a runtime elsewhere, a terminal maybe.
            with (old / f"{LEAD}.json.lock").open("a+") as lock:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                assert_true("open elsewhere" in refusal(client, lead, cwd=str(beta)), "lease")
            # A turn in progress.
            session = client.command("activate", lead)["session"]
            client.until(session, idle)
            client.command("submit", session, text="work")
            wait_until(entered.is_set, "the turn never reached the model")
            assert_true("is running" in refusal(client, session, cwd=str(beta)), "running")
            gate.set()
            client.until(session, lambda value: idle(value) and "done" in json.dumps(value))
            assert_true(
                set(tree_of(old)) >= set(before) and not history_of(home, beta).exists(),
                "a refusal changed the disk",
            )
            assert_true(not list((home / ".uagent" / "web").glob("move-*")), "a journal is left")
            # Idle again, it moves.
            client.command("move", client.snapshot(session)["metadata"], cwd=str(beta))
            assert_whole(home, alpha, beta)


CRASH_STATES = ("journal only", "one file renamed", "all renamed", "rewritten")


def journal_of(home, alpha, beta, names):
    old, new = history_of(home, alpha), history_of(home, beta)
    return {
        "format": 1,
        "from": str(alpha.resolve()),
        "to": str(beta.resolve()),
        "note": note_for(alpha, beta),
        "files": [
            {"from": str(old / f"{name}.json"), "to": str(new / f"{name}.json")} for name in names
        ],
    }


def rename_set(home, alpha, beta, name):
    old, new = history_of(home, alpha), history_of(home, beta)
    new.mkdir(parents=True, exist_ok=True)
    new.chmod(0o700)
    for path in sorted(old.glob(f"{name}.json*")) + sorted(old.glob(f"{name}.share.md")):
        path.rename(new / path.name)


def test_web_move_that_stopped_is_finished_when_the_host_starts(root, _home, *, binary):
    for state in CRASH_STATES:
        base = root / state.replace(" ", "-")
        base.mkdir()
        house = base / "home"
        house.mkdir()
        alpha, beta = base / "alpha", base / "beta"
        alpha.mkdir()
        beta.mkdir()
        save_family(house, alpha, base)
        names = (LEAD, *AGENTS)
        journal = house / ".uagent" / "web" / f"move-{fnv1a64('x' + state)}.json"
        private_web_folder(house)
        if state == "rewritten":
            # A full move, then its journal written back as if the host had
            # stopped before deleting it.
            with Server([event({"content": "unused"})]) as provider:
                with web_host(binary, base, house, provider.url) as (client, code, _, _):
                    client.pair(code)
                    client.command("move", listed(client, LEAD), cwd=str(beta))
            assert_whole(house, alpha, beta)
        elif state == "one file renamed":
            rename_set(house, alpha, beta, AGENTS[2])
        elif state == "all renamed":
            for name in names:
                rename_set(house, alpha, beta, name)
        journal.write_text(json.dumps(journal_of(house, alpha, beta, names)))
        with Server([event({"content": "unused"})]) as provider:
            with web_host(binary, base, house, provider.url) as (client, code, _, _):
                client.pair(code)
                ids = assert_whole(house, alpha, beta)
                wait_until(
                    lambda ids=ids: ids[LEAD] in {item["id"] for item in sessions_of(client)},
                    f"{state}: the conversation is not listed",
                )
        assert_true(not journal.exists(), f"{state}: the journal is left")


def test_web_move_leaves_a_journal_it_cannot_trust_alone(root, home, *, binary):
    alpha, beta = root / "alpha", root / "beta"
    alpha.mkdir()
    beta.mkdir()
    old, _, _ = save_family(home, alpha, root)
    before = tree_of(old)
    folder = private_web_folder(home)
    stray = journal_of(home, alpha, beta, (LEAD,))
    stray["files"][0]["to"] = str(root / "elsewhere" / f"{LEAD}.json")
    for number, text in enumerate(("{not json", json.dumps(stray))):
        (folder / f"move-{number}.json").write_text(text)
    with Server([event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, host):
            client.pair(code)
            assert_true(len(sessions_of(client)) == 4, sessions_of(client))
    assert_true(tree_of(old) == before and not history_of(home, beta).exists(), "files moved")
    assert_true(len(list(folder.glob("move-*.json"))) == 2, "a journal was removed")
    log = (root / "web-host.log").read_text()
    assert_true(log.count("left as it is") == 2, log)


def test_web_move_keeps_the_worktree_of_an_agent_deletable(root, home, *, binary):
    alpha, beta = root / "alpha", root / "beta"
    alpha.mkdir()
    beta.mkdir()
    _, repo, worktree = save_family(home, alpha, root)
    done = []

    def route(_, body):
        results = [m["content"] for m in body["messages"] if m.get("role") == "tool"]
        if results:
            done.extend(results)
            return event({"content": "removed"})
        agent = fnv1a64(str(history_of(home, beta) / f"{AGENTS[2]}.json"))
        return tool_call("agents", {"action": "delete", "id": agent})

    with Server([route]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            moved = client.command("move", listed(client, LEAD), cwd=str(beta))["result"]
            session = client.command("activate", {"id": moved["id"], "generation": ""})["session"]
            client.until(session, idle)
            client.command("submit", session, text="remove the worktree agent")
            asked = client.until(session, lambda value: value["pending"] is not None)
            client.command("reply", session, interaction_id=asked["pending"]["id"], text="y")
            client.until(session, lambda value: idle(value) and "removed" in json.dumps(value))
    assert_true(done == ["deleted"], done)
    assert_true(not worktree.exists(), "the worktree is still there")
    listing = subprocess.run(
        ["git", "-C", str(repo), "worktree", "list", "--porcelain"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    assert_true(str(worktree.resolve()) not in listing, listing)
    assert_true(not (history_of(home, beta) / f"{AGENTS[2]}.json").exists(), "agent file")


def test_web_move_carries_pins_and_scheduled_runs(root, home, *, binary):
    alpha, beta = root / "alpha", root / "beta"
    alpha.mkdir()
    beta.mkdir()
    old, _, _ = save_family(home, alpha, root)
    lead_path = old / f"{LEAD}.json"
    state = home / ".uagent" / "scheduled" / "state.json"
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text(
        json.dumps(
            {
                "v": 1,
                "revision": "r",
                "tasks": [],
                "runs": [
                    {
                        "id": "run-1",
                        "task_id": "task-1",
                        "status": "completed",
                        "session_path": str(lead_path),
                        "session_id": fnv1a64(str(lead_path)),
                        "cwd": str(alpha),
                        "definition": {},
                    }
                ],
            }
        )
    )
    with Server([event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            lead = listed(client, LEAD)
            pins = [{"kind": "session", "id": lead["id"]}, {"kind": "folder", "id": str(alpha)}]
            client.command("pins", pins=pins)
            frames = watch(client)
            moved = client.command("move", lead, cwd=str(beta))["result"]
            expected = [{"kind": "session", "id": moved["id"]}, pins[1]]
            assert_true(client.json("/api/sessions")[1]["pins"] == expected, "pins")
            wait_until(
                lambda: any(f.get("kind") == "pins.changed" for f in frames), "pins not pushed"
            )
    run_record = json.loads(state.read_text())["runs"][0]
    new_path = str(history_of(home, beta) / f"{LEAD}.json")
    assert_true(
        run_record["session_path"] == new_path and run_record["session_id"] == moved["id"],
        run_record,
    )
    assert_true(
        json.loads((home / ".uagent" / "web" / "pins.json").read_text()) == expected, "pins file"
    )
