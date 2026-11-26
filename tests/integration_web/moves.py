"""Moving a conversation, and the agents under it, to another folder."""

import fcntl
import http.client
import json
import subprocess
import threading

from integration_support import (
    Server,
    assert_true,
    base_env,
    event,
    run,
    session_id,
    text_of,
    tool_call,
    wait_until,
    write_session,
)
from web_support import WebClient, idle, web_host

LEAD = "lead"
AGENTS = ("agent-0001", "agent-0002", "agent-0003")


def history_of(home):
    return home / ".uagent" / "history"


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
    directory = history_of(home)
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
            "content": f"see {lead}.assets/pic.png, agent {session_id(one)}",
        },
    ]
    facts = {"t-1": {"change_path": f"{lead}.edits/diff-1", "output": "diff"}}
    write_session(
        home,
        LEAD,
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
            path.stem,
            [
                {"role": "system", "content": "baseline"},
                {"role": "user", "content": f"brief for {path.stem}"},
            ],
            cwd=cwd,
            display={"ids": [1, 2], "next": 3, "facts": {}, "statistics": {"complete": True}},
            session_id=f"{path.stem}-sid",
            title=path.stem,
            agent={"parent": session_id(parent), "edits": "folder", "depth": 1},
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
    connection, response = client.events(timeout=60)
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


def assert_whole(home, alpha, beta, *, notes=1):
    """The family works in the new folder, where it was, and says it moved once."""
    new = history_of(home)
    lead = new / f"{LEAD}.json"
    ids = {name: session_id(new / f"{name}.json") for name in (LEAD, *AGENTS)}
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


def test_web_file_origin_survives_repeated_moves(root, home, *, binary):
    old, middle, latest = (root / name for name in ("old", "middle", "latest"))
    for folder in (old, middle, latest):
        folder.mkdir()
        (folder / "same.html").write_text(folder.name)
    (old / "only-old.md").write_text("old file")
    directory, _, _ = save_family(home, old, root)
    with Server([event({"content": "ok"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            lead = listed(client, LEAD)
            route = "/api/sessions/" + lead["id"] + "/file-origin?"

            def origin(query):
                status, value, _ = client.json(route + query)
                assert_true(status == 200, (status, value))
                return value

            original = lead["file_origin"]
            grant = f"/api/sessions/{lead['id']}/preview"
            base = client.json(grant, {})[1]["base"]
            assert_true(client.request(base + "same.html")[0] == 200, "original preview")
            assert_true(origin("sequence=2")["current"], lead)
            client.command("move", lead, cwd=str(middle))
            moved = listed(client, LEAD)
            middle_id = moved["file_origin"]
            assert_true(
                client.request(base + "same.html")[0] == 404, "preview belongs to old folder"
            )
            new_base = client.json(grant, {})[1]["base"]
            assert_true(
                new_base != base and b"middle" in client.request(new_base + "same.html")[1],
                "new preview",
            )
            assert_true(middle_id != original, moved)
            display = read_session(directory / "lead.json")[1]["display"]
            first_boundary = display["file_moves"][0]["sequence"]
            client.command("move", moved, cwd=str(latest))
            assert_true(origin("sequence=2")["origin"] == original, "old reference")
            assert_true(origin(f"sequence={first_boundary}")["origin"] == middle_id, "middle")
            assert_true(origin("origin=" + original)["cwd"] == str(old.resolve()), "copied URL")
            assert_true(origin("path=only-old.md")["origin"] == original, "old path-only URL")
            status, conflict, _ = client.json(route + "path=same.html")
            assert_true(
                status == 409
                and {row["cwd"] for row in conflict["choices"]}
                == {str(folder.resolve()) for folder in (old, middle, latest)},
                conflict,
            )
            assert_true(client.json(route + "path=missing.md")[0] == 404, "missing old link")
            assert_true(client.json(route + "origin=unknown")[0] == 404, "unrecorded origin")
            child = "/api/sessions/" + session_id(directory / "agent-0001.json")
            assert_true(
                client.json(child + "/file-origin?sequence=2")[1]["origin"] == original, child
            )


def test_web_move_takes_a_conversation_and_its_agents_to_another_folder(root, home, *, binary):
    alpha, beta = root / "alpha", root / "beta"
    alpha.mkdir()
    beta.mkdir()
    old, _, _ = save_family(home, alpha, root)
    before = tree_of(old)
    with Server([lambda _, _body: event({"content": "resumed in beta"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            lead = listed(client, LEAD)
            assert_true(lead["id"] == session_id(old / f"{LEAD}.json"), lead)
            # Another device has it open while this one moves it.
            other = WebClient(client.port)
            other.cookie = client.cookie
            assert_true(note_for(alpha, beta) not in json.dumps(other.snapshot(lead)), "noted")
            frames = watch(other)
            moved = client.command("move", lead, cwd=str(beta))["result"]
            ids = assert_whole(home, alpha, beta)
            assert_true(moved["id"] == ids[LEAD] and moved["cwd"] == str(beta.resolve()), moved)
            # Nothing moved on disk, and nothing is new but the leases.
            assert_true(tree_of(old) == before, (tree_of(old), before))
            wait_until(
                lambda: any(
                    f.get("kind") == "moved" and f.get("session_id") == ids[LEAD] for f in frames
                ),
                f"no moved frame: {frames}",
            )
            assert_true(note_for(alpha, beta) in json.dumps(other.snapshot(lead)), "not noted")
            # The agents are listed under it still, at the same ids.
            wait_until(
                lambda: (
                    {item["id"] for item in sessions_of(client) if item.get("parent") == ids[LEAD]}
                    == {ids[AGENTS[0]], ids[AGENTS[2]]}
                ),
                "the agents are not listed under the moved conversation",
            )
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


def test_web_move_of_a_conversation_made_in_the_web_closes_it_and_lists_it_once(
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
            assert_true(moved["id"] == session["id"], moved)
            assert_true(not list(drafts.glob("*.json")), "the draft record is left")
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
                set(tree_of(old)) >= set(before)
                and read_session(old / f"{LEAD}.json")[0]["cwd"] == str(alpha.resolve()),
                "a refusal changed the disk",
            )
            client.command("move", client.snapshot(session)["metadata"], cwd=str(beta))
            assert_whole(home, alpha, beta)


def test_web_move_takes_a_side_chat_along_and_it_merges_back(root, home, *, binary):
    """A side chat moves with the conversation it belongs to, and still
    finds it to merge into."""
    alpha, beta = root / "alpha", root / "beta"
    alpha.mkdir()
    beta.mkdir()

    def route(_, body):
        text = text_of(body)
        if "merges this side chat" in text:
            return event({"content": "SIDE-SUMMARY"})
        if "SIDE-SUMMARY" in text:
            return event({"content": "PARENT-READ-IT"})
        return event({"content": "said"})

    with Server([route]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            parent = client.create(alpha)
            client.command("submit", parent, text="hello")
            client.until(parent, lambda value: idle(value) and "said" in json.dumps(value))
            fork = client.command("side", parent, operation="create")["result"]
            side = client.command("activate", {"id": fork["id"], "generation": ""})["session"]
            client.until(side, idle)
            parent = client.snapshot(parent)["metadata"]
            client.command("move", parent, cwd=str(beta))
            side = client.snapshot(side)["metadata"]
            assert_true(side["cwd"] == str(beta.resolve()), side)
            client.command("side", side, operation="merge")
            parent = client.command("activate", {"id": parent["id"], "generation": ""})["session"]
            client.until(parent, lambda value: "PARENT-READ-IT" in json.dumps(value))


def test_web_move_delivers_the_mail_waiting_for_its_conversation(root, home, *, binary):
    alpha, beta = root / "alpha", root / "beta"
    alpha.mkdir()
    beta.mkdir()
    old, _, _ = save_family(home, alpha, root)
    lead, agent = old / f"{LEAD}.json", old / f"{AGENTS[0]}.json"
    mailbox = home / ".uagent" / "mail" / session_id(lead)
    (mailbox / "new").mkdir(parents=True)
    (mailbox / "new" / "0000000000001-0-0-note.json").write_text(
        json.dumps(
            {
                "format": 1,
                "id": "note",
                "from": session_id(agent),
                "to": session_id(lead),
                "sender_path": str(agent),
                "hops": 0,
                "created_ms": 1,
                "expires_ms": 0,
                "body": {"state": "completed", "text": "MAIL-FROM-AGENT"},
            }
        )
    )

    def route(_, body):
        sent = text_of(body)
        return event({"content": "READ-MAIL" if "MAIL-FROM-AGENT" in sent else "nothing"})

    with Server([route]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            moved = client.command("move", listed(client, LEAD), cwd=str(beta))["result"]
            session = client.command("activate", {"id": moved["id"], "generation": ""})["session"]
            client.until(session, lambda value: "READ-MAIL" in json.dumps(value))


def test_web_move_keeps_the_spend_its_agents_left_to_count(root, home, *, binary):
    alpha, beta = root / "alpha", root / "beta"
    alpha.mkdir()
    beta.mkdir()
    old, _, _ = save_family(home, alpha, root)
    ledger = home / ".uagent" / "activity" / f"usage-{session_id(old / f'{LEAD}.json')}.jsonl"
    ledger.parent.mkdir(parents=True, exist_ok=True)
    ledger.write_text(json.dumps({"usage": {"cost": 1.25}, "route": "test/test"}) + "\n")
    with Server([event({"content": "counted"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            moved = client.command("move", listed(client, LEAD), cwd=str(beta))["result"]
            session = client.command("activate", {"id": moved["id"], "generation": ""})["session"]
            client.until(session, idle)
            client.command("submit", session, text="go on")
            value = client.until(
                session, lambda value: idle(value) and "counted" in json.dumps(value)
            )
            assert_true(value["metadata"]["cost"] >= 1.25, value["metadata"])
    assert_true(ledger.read_text() == "", "the agents' spend was not taken")


def test_web_move_that_stopped_halfway_is_finished_by_moving_again(root, home, *, binary):
    alpha, beta = root / "alpha", root / "beta"
    alpha.mkdir()
    beta.mkdir()
    old, _, _ = save_family(home, alpha, root)
    agent = old / f"{AGENTS[1]}.json"
    head, body = agent.read_text(encoding="utf-8").split("\n", 1)
    agent.write_text(
        json.dumps(json.loads(head) | {"cwd": str(beta.resolve())}) + "\n" + body, encoding="utf-8"
    )
    ids = [session_id(old / f"{name}.json") for name in AGENTS]

    def agents_of(client, frames, lead):
        """What `/agents` lists: the folder of the conversation, by its top."""
        session = client.command("activate", {"id": lead["id"], "generation": ""})["session"]
        client.until(session, idle)
        seen = len(frames)
        client.command("submit", session, text="/agents")
        wait_until(
            lambda: any(f.get("type") == "command.completed" for f in frames[seen:]), "no list"
        )
        done = next(f for f in frames[seen:] if f.get("type") == "command.completed")
        # The final checkpoint, after the completion, frees the conversation.
        client.until(session, idle)
        return done["data"]["output"]

    with Server([event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            frames = watch(client)
            # Halfway, the whole family is one folder's: the old one, where
            # its conversation still is, the moved agent too.
            lead = listed(client, LEAD)
            sessions = {item["title"]: item for item in sessions_of(client)}
            assert_true(lead["cwd"] == str(alpha.resolve()), lead)
            assert_true(sessions[AGENTS[1]]["parent"] == sessions[AGENTS[0]]["id"], sessions)
            before = agents_of(client, frames, lead)
            assert_true(all(agent in before for agent in ids), before)
            client.command("move", client.snapshot(lead)["metadata"], cwd=str(beta))
            after = agents_of(client, frames, listed(client, LEAD))
            assert_true(all(agent in after for agent in ids), after)
    assert_whole(home, alpha, beta)


def test_web_move_stops_at_an_unreadable_member_and_loses_nothing(root, home, *, binary):
    alpha, beta = root / "alpha", root / "beta"
    alpha.mkdir()
    beta.mkdir()
    old, _, _ = save_family(home, alpha, root)
    agent = old / f"{AGENTS[0]}.json"
    whole = agent.read_text(encoding="utf-8")
    agent.write_text(whole.split("\n", 1)[0] + "\nnot a transcript", encoding="utf-8")
    names = sorted(path.name for path in old.glob("*.json"))
    with Server([event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            lead = listed(client, LEAD)
            assert_true(AGENTS[0] in refusal(client, lead, cwd=str(beta)), "unreadable")
            assert_true(sorted(path.name for path in old.glob("*.json")) == names, "a file went")
            head, _ = read_session(old / f"{LEAD}.json")
            assert_true(head["cwd"] == str(alpha.resolve()), "the conversation moved")
            # Mended, moving again finishes it.
            agent.write_text(whole, encoding="utf-8")
            client.command("move", listed(client, LEAD), cwd=str(beta))
    assert_whole(home, alpha, beta)


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
        agent = session_id(history_of(home) / f"{AGENTS[2]}.json")
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
    assert_true(not (history_of(home) / f"{AGENTS[2]}.json").exists(), "agent file")


def test_web_move_keeps_pins_and_scheduled_runs(root, home, *, binary):
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
                        "session_id": session_id(lead_path),
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
            moved = client.command("move", lead, cwd=str(beta))["result"]
            expected = [{"kind": "session", "id": moved["id"]}, pins[1]]
            assert_true(moved["id"] == lead["id"], moved)
            assert_true(client.json("/api/sessions")[1]["pins"] == expected, "pins")
    run_record = json.loads(state.read_text())["runs"][0]
    assert_true(
        run_record["session_path"] == str(lead_path) and run_record["session_id"] == moved["id"],
        run_record,
    )
    assert_true(
        json.loads((home / ".uagent" / "web" / "pins.json").read_text()) == expected, "pins file"
    )
