"""OS sandbox: what an agent-run command may and may not write.

Every case here asserts the filesystem, not the message: a command that fails
for the wrong reason still leaves the file absent, and a command that succeeds
in spite of the sandbox leaves it present whatever it printed.

The workspace is a subdirectory of the case root rather than the case root
itself, so that every "outside" path here can live in the case root: that is
neither the workspace nor under any other default root, so a write to one is
denied on both platforms.
"""

import json
import os
import pathlib
import shlex
import shutil
import socket
import sys

from integration_support import (
    Server,
    Skipped,
    assert_true,
    base_env,
    event,
    run,
    run_dialog,
    run_pty,
    save_settings,
    settings_path,
    tool_call,
)


def workspace(root):
    """The agent's cwd, with the case root left over as unwritable ground."""
    path = root / "ws"
    path.mkdir(exist_ok=True)
    return path


def test_sandbox_keeps_spawning_after_binary_replacement(root, home, *, binary):
    if not sys.platform.startswith("linux"):
        raise Skipped("Linux sandbox trampoline uses /proc/self/exe")
    if not sandbox_enforced(root, home, binary=binary):
        raise Skipped("host does not enforce the OS sandbox")
    installed = root / "installed-agent"
    replacement = root / "replacement-agent"
    shutil.copy2(binary, installed)
    shutil.copy2(binary, replacement)
    outside = root / "outside-after-upgrade"

    def upgrade(_, _body):
        # The running inode survives, but readlink(/proc/self/exe) now names
        # a deleted path. Commands must still use that same sandbox image.
        os.replace(replacement, installed)
        return tool_call(
            "run",
            {"command": f"printf ok > shell-ran; printf denied > {shlex.quote(str(outside))}"},
        )

    with Server(
        [
            upgrade,
            tool_call(
                "scratch",
                {"path": "upgrade.py", "code": "open('scratch-ran', 'w').write('ok')"},
            ),
            event({"content": "done"}),
        ]
    ) as server:
        result = run(
            workspace(root),
            sandbox_env(home, server.url),
            "--yolo",
            "-p",
            "check upgrade",
            timeout=30,
            binary=str(installed),
        )
    assert_true(result.returncode == 0, result.stderr)
    assert_true((workspace(root) / "shell-ran").exists(), result.stdout)
    assert_true((workspace(root) / "scratch-ran").exists(), result.stdout)
    assert_true(not outside.exists(), "sandbox was bypassed after replacement")


def sandbox_env(home, url, **overrides):
    env = base_env(home, url)
    env["UAGENT_SANDBOX"] = "default"
    env.update(overrides)
    return env


def run_once(root, env, command, *flags, binary):
    """One turn that runs `command` through the shell tool."""
    with Server([tool_call("run", {"command": command}), event({"content": "ok"})]) as server:
        env = dict(env)
        env["UAGENT_BASE_URL"] = server.url
        return run_dialog(
            workspace(root), env, "y\n", *flags, "-p", "go", timeout=30, binary=binary
        )


def tool_output(root, env, command, headless=False, *, binary, **arguments):
    """The same turn, but returning what the tool reported back to the model."""
    seen = []

    def route(_, body):
        contents = [str(m.get("content", "")) for m in body["messages"] if m.get("role") == "tool"]
        if contents:
            seen.append(contents[-1])
            return event({"content": "ok"})
        return tool_call("run", {"command": command, **arguments})

    with Server([route]) as server:
        env = dict(env)
        env["UAGENT_BASE_URL"] = server.url
        if headless:
            run(workspace(root), env, "--yolo", "-p", "go", timeout=30, binary=binary)
        else:
            run_dialog(workspace(root), env, "y\n", "-p", "go", timeout=30, binary=binary)
    return seen[0] if seen else ""


def sandbox_enforced(root, home, *, binary):
    """True when this host actually confines writes.

    Probing by behaviour rather than by platform: the answer on Linux depends
    on the kernel, and a suite that assumed enforcement would report a missing
    Landlock as a sandbox escape.
    """
    probe = root / "sandbox-probe"
    # Inside the workspace first: a command that never ran escaped nothing,
    # and is not the sandbox holding.
    ran = workspace(root) / "sandbox-ran"
    run_once(root, sandbox_env(home, ""), f"echo x > {ran}; echo x > {probe}", binary=binary)
    assert_true(ran.exists(), "the probe's command did not run")
    ran.unlink()
    escaped = probe.exists()
    probe.unlink(missing_ok=True)
    return not escaped


def require_sandbox(root, home, *, binary):
    """A case about confinement proves nothing where nothing confines: it is
    skipped there, and fails where UAGENT_TEST_REQUIRE_SANDBOX says the host
    must enforce."""
    if sandbox_enforced(root, home, binary=binary):
        return
    assert_true(
        not os.environ.get("UAGENT_TEST_REQUIRE_SANDBOX"), "this host does not enforce the sandbox"
    )
    raise Skipped("this host does not enforce the sandbox")


def test_yolo_keeps_the_sandbox_and_only_its_setting_lifts_it(root, home, *, binary):
    """Yolo means nobody is asked, in either startup form. It raises the
    default level to relaxed, which still confines writes outside the home
    folder; only the sandbox setting lifts the sandbox."""
    require_sandbox(root, home, binary=binary)
    for source, sandbox in (("cli", "default"), ("config", "default"), ("cli", "off")):
        outside = root / f"yolo-{source}-{sandbox}.txt"
        command = f"echo x > {outside}"
        with Server([tool_call("run", {"command": command}), event({"content": "ok"})]) as server:
            env = sandbox_env(home, server.url)
            env["UAGENT_SANDBOX"] = sandbox
            flags = ("--yolo",) if source == "cli" else ()
            if source == "config":
                env["UAGENT_APPROVAL"] = "yolo"
            result = run(workspace(root), env, *flags, "-p", "go", timeout=30, binary=binary)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(
            outside.exists() == (sandbox == "off"),
            f"{source} yolo with UAGENT_SANDBOX={sandbox}: {result.stdout}",
        )


def test_an_agent_runs_in_its_own_sandbox_whatever_its_parents(root, home, *, binary):
    """An agent that may edit runs its commands in the default sandbox, one
    that may not in the read-only one, even under a parent that runs
    unconfined."""
    require_sandbox(root, home, binary=binary)
    ws = workspace(root)
    for edits in ("folder", "none"):
        outside, inside = root / f"outside-{edits}.txt", ws / f"inside-{edits}.txt"

        def route(_, body, edits=edits, outside=outside, inside=inside):
            users = [str(m.get("content", "")) for m in body["messages"] if m.get("role") == "user"]
            results = [m for m in body["messages"] if m.get("role") == "tool"]
            if any(text.startswith("BRIEF ") for text in users):
                if len(results) > 1:
                    return event({"content": "agent-done"})
                return tool_call("run", {"command": f"echo x > {inside}; echo x > {outside}"})
            if results:
                return event({"content": "parent-done"})
            return tool_call("agent_start", {"brief": "BRIEF write", "edits": edits})

        with Server([route]) as server:
            env = sandbox_env(home, server.url, UAGENT_SANDBOX="off")
            result = run(ws, env, "--yolo", "-p", "go", timeout=60, binary=binary)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true("parent-done" in result.stdout, result.stdout)
        assert_true(inside.exists() == (edits == "folder"), f"edits {edits}: {result.stdout}")
        assert_true(not outside.exists(), f"edits {edits} wrote outside: {result.stdout}")


def test_sudo_uses_shared_approval_and_sandbox_policy(root, home, *, binary):
    """A harmless sudo stand-in exercises dispatch without needing root."""
    enforced = sandbox_enforced(root, home, binary=binary)
    ws = workspace(root)
    executables = ws / "bin"
    executables.mkdir()
    sudo = executables / "sudo"
    sudo.write_text('#!/bin/sh\nprintf "SUDO_FIXTURE\\n"\nexec "$@"\n')
    sudo.chmod(0o755)
    for tool in ("run", "scratch"):
        for mode in ("yolo", "confined", "approved", "denied"):
            if mode == "approved" and tool == "scratch":
                continue  # scratch has no per-command escape hatch
            if mode in ("yolo", "confined", "approved") and not enforced:
                continue
            outside = root / f"{tool}-{mode}.txt"
            command = "sudo sh -c " + shlex.quote(f"echo written > {shlex.quote(str(outside))}")
            if tool == "scratch":
                script = ws / ".uagent" / "scratch" / f"{mode}.sh"
                script.parent.mkdir(parents=True, exist_ok=True)
                script.write_text(command + "\n")
            arguments = {"command": command} if tool == "run" else {"path": f"{mode}.sh"}
            if mode == "approved":
                arguments["sandbox"] = False
            seen = []

            def finish(_, body, seen=seen):
                seen.extend(
                    str(m.get("content", "")) for m in body["messages"] if m.get("role") == "tool"
                )
                return event({"content": "policy-ok"})

            with Server([tool_call(tool, arguments), finish]) as server:
                env = sandbox_env(home, server.url)
                env["PATH"] = str(executables) + ":" + env["PATH"]
                flags = ("--yolo",) if mode == "yolo" else ()
                if mode == "approved":
                    # A person is there, so root=true is offered and the
                    # command's own sudo points at it instead of running.
                    code, transcript = run_pty(
                        ws,
                        env,
                        [(b"go\n", b"Allow run?  [y] Allow"), (b"y\n", b"policy-ok"), b"", b"/q\n"],
                        timeout=30,
                        binary=binary,
                    )
                    assert_true(code == 0, transcript)
                else:
                    result = run_dialog(
                        ws,
                        env,
                        "n\n" if mode == "denied" else "y\n",
                        *flags,
                        "-p",
                        "run the fixture",
                        timeout=30,
                        binary=binary,
                    )
                    assert_true(result.returncode == 0, (tool, mode, result.stdout, result.stderr))
            output = "\n".join(seen)
            assert_true("privileged commands are unavailable" not in output, output)
            ran = mode not in ("denied", "approved")
            assert_true(("SUDO_FIXTURE" in output) == ran, (tool, mode, output))
            assert_true(
                ("use run root=true" in output) == (mode == "approved"), (tool, mode, output)
            )
            # Yolo spares the question, not the confinement.
            assert_true(not outside.exists(), (tool, mode, output))


def test_yolo_toggle_raises_the_level_from_the_next_command(root, home, *, binary):
    """Interactive /yolo stops the questions and raises the default level to
    relaxed from the next command: a closed folder opens, writes outside the
    home folder stay confined. Turned off, the default level is back."""
    require_sandbox(root, home, binary=binary)
    sibling = home / "Projects" / "other" / "notes.txt"
    sibling.parent.mkdir(parents=True)
    sibling.write_text("marker-notes.txt\n")
    prompts = ("before", "unconfined", "confined")
    outputs = {}

    def route(_, body):
        messages = body["messages"]
        turns = [
            (index, str(message.get("content", "")))
            for index, message in enumerate(messages)
            if message.get("role") == "user" and str(message.get("content", "")) in prompts
        ]
        assert_true(turns, messages)
        turn_start, prompt = turns[-1]
        results = [
            message for message in messages[turn_start + 1 :] if message.get("role") == "tool"
        ]
        if results:
            outputs[prompt] = str(results[-1].get("content", ""))
            return event({"content": f"{prompt}-ok"})
        target = root / f"toggle-{prompt}.txt"
        return tool_call("run", {"command": f"echo x > {target}; cat {sibling}; true"})

    with Server([route]) as server:
        result = run_dialog(
            workspace(root),
            sandbox_env(home, server.url),
            "before\ny\n/yolo\nunconfined\n/yolo\nconfined\ny\n/q\n",
            timeout=30,
            binary=binary,
        )
    assert_true(result.returncode == 0, (result.stdout, result.stderr))
    assert_true("unconfined-ok" in result.stdout, result.stdout)
    assert_true("sandbox relaxed (yolo)" in result.stdout, result.stdout)
    assert_true("yolo off; sandbox default" in result.stdout, result.stdout)
    for prompt in prompts:
        assert_true(
            not (root / f"toggle-{prompt}.txt").exists(),
            f"{prompt}: a command escaped the sandbox: {result.stdout}",
        )
        assert_true(
            ("marker-notes.txt" in outputs.get(prompt, "")) == (prompt == "unconfined"),
            f"{prompt}: {outputs}",
        )


def test_yolo_raises_the_default_level_to_relaxed(root, home, *, binary):
    """Under yolo a default-level command reads your other folders and writes
    the home folder, but not ~/.uagent or a credential store; a level written
    down as readonly stays as it is."""
    require_sandbox(root, home, binary=binary)
    plant_credentials(home)
    sibling = home / "Projects" / "other" / "notes.txt"
    sibling.parent.mkdir(parents=True)
    sibling.write_text("marker-notes.txt\n")
    env = sandbox_env(home, "")
    assert_true(not readable(root, env, sibling, binary=binary), "ask read a closed folder")

    def yolo_output(command, environment=env):
        return tool_output(root, environment, command, headless=True, binary=binary)

    assert_true("marker-notes.txt" in yolo_output(f"cat {sibling}"), "yolo kept it closed")
    stores = (home / ".aws" / "credentials", home / ".uagent" / "config" / "notes.txt")
    for hidden in stores:
        assert_true(f"marker-{hidden.name}" not in yolo_output(f"cat {hidden}"), hidden)
    # Landlock grants the home folder entry by entry when a command starts,
    # so the write goes into a folder that exists, as on every platform.
    project, state = home / "Projects" / "project.txt", home / ".uagent" / "planted.txt"
    yolo_output(f"echo x > {project}; echo x > {state}; true")
    assert_true(project.exists(), "yolo did not open the home folder")
    assert_true(not state.exists(), "yolo opened ~/.uagent")
    readonly = sandbox_env(home, "", UAGENT_SANDBOX="readonly")
    assert_true(
        "marker-notes.txt" not in yolo_output(f"cat {sibling}", readonly),
        "yolo raised the readonly level",
    )
    with Server([event({"content": "ready-ok"})]) as server:
        code, output = run_pty(
            workspace(root),
            sandbox_env(home, server.url),
            [(b"/status\n", b"sandbox"), b"", b"/q\n"],
            args=("--yolo",),
            timeout=30,
            binary=binary,
        )
    assert_true(code == 0, output)
    assert_true(b"relaxed (yolo)" in output, f"/status did not name the level: {output!r}")


def test_sandbox_confines_writes_to_the_workspace(root, home, *, binary):
    """Inside the workspace writes land; outside it they do not."""
    require_sandbox(root, home, binary=binary)
    inside, outside = workspace(root) / "inside.txt", root / "outside.txt"
    # No trailing `true`: the shell's exit status has to carry the failure, or
    # the hint below has nothing to attach itself to.
    output = tool_output(
        root, sandbox_env(home, ""), f"echo in > {inside}; echo out > {outside}", binary=binary
    )
    assert_true(inside.exists(), "workspace write was blocked")
    assert_true(not outside.exists(), "wrote outside the workspace")
    # The errno the shell prints names a permission, not the policy that
    # withheld it. Whoever reads the failure has to be told which it was.
    assert_true("[sandbox:" in output, f"a refused write did not name the sandbox: {output}")


def test_sandbox_opens_the_package_cache_not_the_data_directory(root, home, *, binary):
    """Other programs keep keys and autostart entries beside uv's tools."""
    require_sandbox(root, home, binary=binary)
    data = home / ".local" / "share"
    (data / "autostart").mkdir(parents=True, exist_ok=True)
    (home / ".cache").mkdir(exist_ok=True)
    run_once(
        root,
        sandbox_env(home, ""),
        f"echo x > {data}/uv/tool; echo x > {data}/autostart/job; "
        f"echo x > {home}/.cache/entry; echo x > {home}/.npm/entry",
        binary=binary,
    )
    assert_true((data / "uv" / "tool").exists(), "the package manager's own folder was closed")
    assert_true((home / ".cache" / "entry").exists(), "the cache was closed")
    assert_true((home / ".npm" / "entry").exists(), "npm's cache, which npx needs, was closed")
    assert_true(not (data / "autostart" / "job").exists(), "wrote another program's data")


def test_sandbox_protects_agent_state(root, home, *, binary):
    """A shell command cannot reach the saved settings or the trust store.

    The unsandboxed control is the point of the case: without it a passing
    assertion could just mean the command was malformed.
    """
    require_sandbox(root, home, binary=binary)
    target = settings_path(home)
    target.parent.mkdir(parents=True, exist_ok=True)
    command = f"echo x >> {target}"
    run_once(root, sandbox_env(home, ""), command, binary=binary)
    assert_true(not target.exists(), "sandboxed command wrote the saved settings")

    run_once(root, sandbox_env(home, "", UAGENT_SANDBOX="off"), command, binary=binary)
    assert_true(target.exists(), "control run could not write the saved settings either")


def test_sandbox_hides_the_web_hosts_devices(root, home, *, binary):
    """A paired device's token would let a command act as the person at a
    browser: commands cannot read the web host's state, and the file tools
    refuse it. The unsandboxed control shows the command itself works."""
    require_sandbox(root, home, binary=binary)
    devices = home / ".uagent" / "web" / "devices.json"
    devices.parent.mkdir(parents=True, exist_ok=True)
    devices.write_text("device-token-marker\n")
    command = f"cat {devices}"
    assert_true(
        "device-token-marker"
        not in tool_output(root, sandbox_env(home, ""), command, binary=binary),
        "a sandboxed command read the paired devices",
    )
    assert_true(
        "device-token-marker"
        in tool_output(root, sandbox_env(home, "", UAGENT_SANDBOX="off"), command, binary=binary),
        "control run could not read the file either",
    )


def test_a_link_made_in_the_same_batch_does_not_reach_the_saved_settings(root, home, *, binary):
    """A call is approved for what it reaches when the batch is prepared. A
    call before it can make its path a link to something else: what it needs
    is decided again when it runs, and it is refused. So for the saved
    settings, which only a person may touch, and for any file outside the
    folder, which is a different question from one inside it."""
    ws = workspace(root)
    settings = settings_path(home)
    settings.parent.mkdir(parents=True, exist_ok=True)
    for name, link, target, path in (
        ("settings", ws / "alias", settings.parent, "alias/settings.json"),
        ("outside", ws / "local.txt", root / "outside.txt", "local.txt"),
    ):
        seen = []
        if name == "outside":
            target.write_text("before")

        def route(_, body, seen=seen, link=link, target=target, path=path):
            results = [
                str(m.get("content", "")) for m in body["messages"] if m.get("role") == "tool"
            ]
            if results:
                seen.extend(results)
                return event({"content": "done"})
            calls = (
                ("link", "run", {"command": f"ln -s {target} {link}"}),
                ("write", "write_file", {"path": path, "content": "{}"}),
            )
            return event(
                {
                    "tool_calls": [
                        {
                            "index": index,
                            "id": call,
                            "function": {"name": tool, "arguments": json.dumps(arguments)},
                        }
                        for index, (call, tool, arguments) in enumerate(calls)
                    ]
                },
                finish="tool_calls",
            )

        with Server([route] * 2) as server:
            env = sandbox_env(home, server.url, UAGENT_SANDBOX="off")
            result = run(ws, env, "--yolo", "-p", "go", timeout=30, binary=binary)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(link.is_symlink(), f"{name}: the link was never made: {seen}")
        assert_true(
            not settings.exists() and (name == "settings" or target.read_text() == "before"),
            f"{name}: a file tool wrote through the link",
        )
        assert_true(any("changed after it was approved" in text for text in seen), (name, seen))


def test_sandbox_reads_only_the_workspace_among_your_folders(root, home, *, binary):
    """The home folder's visible entries, the workspaces in ~/.uagent/spaces
    and the workspace's neighbours are closed: a command reads none of them
    but the workspace, and a file tool asks a
    person, refusing where nobody can answer. Dot-folders and everything
    outside the home folder stay readable, and sandbox.read opens a folder."""
    require_sandbox(root, home, binary=binary)
    sibling = home / "Projects" / "other" / "notes.txt"
    sibling.parent.mkdir(parents=True)
    sibling.write_text("marker-notes.txt\n")
    space = home / ".uagent" / "spaces" / "other" / "plan.txt"
    space.parent.mkdir(parents=True)
    space.write_text("marker-plan.txt\n")
    tool_state = home / ".config" / "tool" / "settings.txt"
    tool_state.parent.mkdir(parents=True)
    tool_state.write_text("marker-settings.txt\n")
    # The workspace's neighbour is another project, as in /workspaces.
    neighbour = root / "neighbour.txt"
    neighbour.write_text("marker-neighbour.txt\n")
    outside = pathlib.Path(os.environ["TMPDIR"]) / f"{root.name}-outside.txt"
    outside.write_text(f"marker-{outside.name}\n")
    env = sandbox_env(home, "")
    output = tool_output(root, env, f"cat {sibling}", binary=binary)
    assert_true("marker-notes.txt" not in output, f"a command read {sibling}: {output}")
    assert_true("sandbox.read" in output, f"the refusal did not say what opens it: {output}")
    for closed in (space, neighbour):
        assert_true(not readable(root, env, closed, binary=binary), f"a command read {closed}")
    for open_path in (tool_state, outside):
        assert_true(
            readable(root, env, open_path, binary=binary), f"a command could not read {open_path}"
        )
    # Not under yolo, which raises the level to relaxed and opens them.
    refused = file_tool_output(
        root, env, "read_path", {"path": str(sibling)}, binary=binary, yolo=False
    )
    assert_true("marker-notes.txt" not in refused, f"read_path read {sibling} unasked")
    assert_true("sandbox.read" in refused, refused)
    # A failed edit hands back lines of the file, so writing asks as reading does.
    edited = file_tool_output(
        root,
        env,
        "edit_file",
        {"path": str(sibling), "edits": [{"old": "no such text", "new": "x"}]},
        binary=binary,
        yolo=False,
    )
    assert_true("marker-notes.txt" not in edited, f"edit_file leaked {sibling}: {edited}")
    assert_true("sandbox.read" in edited, edited)
    opened = sandbox_env(home, "", UAGENT_SANDBOX_READ=str(home / "Projects" / "other"))
    assert_true(readable(root, opened, sibling, binary=binary), "sandbox.read did not open it")
    # Opened, it is an ordinary read outside the folder: no person needed
    # under yolo, and not the closed folder's refusal in ask mode.
    read = file_tool_output(root, opened, "read_path", {"path": str(sibling)}, binary=binary)
    assert_true("marker-notes.txt" in read, read)
    asked = file_tool_output(
        root, opened, "read_path", {"path": str(sibling)}, binary=binary, yolo=False
    )
    assert_true("not approved" in asked and "sandbox.read" not in asked, asked)
    # The documented ~/ spelling means the home folder, not a folder named ~.
    tilde = sandbox_env(home, "", UAGENT_SANDBOX_READ="~/Projects/other")
    assert_true(readable(root, tilde, sibling, binary=binary), "sandbox.read=~/... opened nothing")


def test_sandbox_opens_the_tools_the_environment_names(root, home, *, binary):
    """A bin folder on PATH opens its parent, or itself when the parent is the
    home folder; Go's default ~/go opens while GOPATH is unset."""
    require_sandbox(root, home, binary=binary)
    tool = home / "bin" / "mytool"
    tool.parent.mkdir(parents=True)
    tool.write_text("marker-mytool\n")
    go = home / "go" / "pkg" / "mod" / "gotool"
    go.parent.mkdir(parents=True)
    go.write_text("marker-gotool\n")
    plain = sandbox_env(home, "")
    plain.pop("GOPATH", None)
    assert_true(readable(root, plain, go, binary=binary), "~/go stayed closed")
    assert_true(not readable(root, plain, tool, binary=binary), "~/bin was open without PATH")
    on_path = dict(plain, PATH=f"{home / 'bin'}:{os.environ['PATH']}")
    assert_true(readable(root, on_path, tool, binary=binary), "~/bin on PATH stayed closed")
    elsewhere = dict(plain, GOPATH=str(root / "gopath"))
    assert_true(not readable(root, elsewhere, go, binary=binary), "~/go opened with GOPATH set")


def git(*arguments, cwd):
    import subprocess

    identity = ["-c", "user.name=t", "-c", "user.email=t@t"]
    subprocess.run(["git", *identity, *arguments], cwd=cwd, check=True, capture_output=True)


def test_sandbox_lets_a_linked_worktree_use_its_repository(root, home, *, binary):
    """A worktree's git directories sit beside the main checkout, in a folder
    the sandbox closes. git status, add and commit work in the worktree, and
    the main checkout stays closed."""
    require_sandbox(root, home, binary=binary)
    main = root / "main-repo"
    main.mkdir()
    git("init", "-q", "-b", "main", cwd=main)
    (main / "secret.txt").write_text("marker-main-secret\n")
    git("add", ".", cwd=main)
    git("commit", "-qm", "base", cwd=main)
    git("worktree", "add", "-q", "-b", "wt", str(workspace(root)), cwd=main)
    env = sandbox_env(home, "", GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t")
    env.update(GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
    status = tool_output(root, env, "git status --short && echo STATUS-OK", binary=binary)
    assert_true("STATUS-OK" in status, f"git status failed in the worktree: {status}")
    committed = tool_output(
        root,
        env,
        "echo change > change.txt && git add change.txt && git -c user.email=a@b -c user.name=a commit -qm wt-commit"
        " && git log --oneline | head -1",
        binary=binary,
    )
    assert_true("wt-commit" in committed, f"git commit failed in the worktree: {committed}")
    assert_true(
        not readable(root, env, main / "secret.txt", binary=binary), "the main checkout was open"
    )
    config = main / ".git" / "config"
    assert_true("wt-commit" in git_log(main), "the commit did not reach the repository")
    assert_true(config.exists(), "the repository lost its config")


def git_log(repo):
    import subprocess

    return subprocess.run(
        ["git", "log", "--all", "--oneline"], cwd=repo, capture_output=True, text=True
    ).stdout


def browser_reach(root, env, profile, *, binary, approve=False, yolo=False):
    """What a command could get at in the browser profile, as marker names.

    Each probe writes its marker only on success, into the workspace, so a
    probe that was stopped for any reason reads as unreached.
    """
    ws = workspace(root)
    for marker in ws.glob("reach-*"):
        marker.unlink()
    connect = "import socket;socket.socket(socket.AF_UNIX).connect('s.sock')"
    probes = {
        "list": f"ls {profile} | grep -q Login",
        "read": f"grep -q login-secret {shlex.quote(str(profile / 'Login Data'))}",
        "connect": f'(cd {profile} && {sys.executable} -c "{connect}")',
        # The owner of the profile is this test, which is not the command's
        # descendant: its /proc root must not reach the files either.
        "proc": f"grep -q login-secret "
        f"{shlex.quote(f'/proc/{os.getpid()}/root{profile}/Login Data')}",
        "control": f"grep -q read-me-marker {ws / 'readable.txt'}",
        # Last, since it succeeds unsandboxed. A renamed profile would be out
        # from under every rule that names it.
        "rename": f"mv {profile} {profile}.moved",
    }
    command = "; ".join(f"{probe} && echo x > reach-{name}" for name, probe in probes.items())
    arguments = {"command": command, **({"sandbox": False} if approve else {})}
    with Server([tool_call("run", arguments), event({"content": "reach-ok"})]) as server:
        env = dict(env, UAGENT_BASE_URL=server.url)
        if approve:
            run_pty(
                ws,
                env,
                [(b"go\n", b"Allow run?  [y] Allow"), (b"y\n", b"reach-ok"), b"", b"/q\n"],
                timeout=30,
                binary=binary,
            )
        elif yolo:
            run(ws, env, "--yolo", "-p", "go", timeout=30, binary=binary)
        else:
            run_dialog(ws, env, "y\n", "-p", "go", timeout=30, binary=binary)
    return {marker.name.removeprefix("reach-") for marker in ws.glob("reach-*")}


def landlock_abi():
    """The kernel's Landlock ABI, or 0; decides whether sockets can be hidden."""
    import ctypes

    abi = ctypes.CDLL(None, use_errno=True).syscall(444, None, 0, 1)
    return max(abi, 0)


def test_sandbox_hides_the_browser_profile(root, home, *, binary):
    """A sandboxed command cannot read the browser profile; nothing else changes.

    A person-approved sandbox=false and a disabled sandbox lift it, as they
    lift the sandbox itself; yolo does not.
    """
    require_sandbox(root, home, binary=binary)
    # Canonical, because both mechanisms match the resolved path.
    profile = pathlib.Path(os.path.realpath(root)) / "browser"
    profile.mkdir()
    (profile / "Login Data").write_text("login-secret\n")
    (workspace(root) / "readable.txt").write_text("read-me-marker\n")
    # Bound and reached by a relative name: the full path can exceed the
    # small limit a unix socket address has.
    listener = socket.socket(socket.AF_UNIX)
    cwd = os.getcwd()
    os.chdir(profile)
    try:
        listener.bind("s.sock")
    finally:
        os.chdir(cwd)
    listener.listen(8)
    everything = {"list", "read", "connect", "control", "rename"}
    expected = {"control"}
    if sys.platform.startswith("linux"):
        everything.add("proc")
        # Listing names is allowed there, and Landlock before ABI 9 cannot
        # refuse a connect to a pathname socket.
        expected.add("list")
        if landlock_abi() < 9:
            expected.add("connect")
    try:
        env = sandbox_env(home, "", UAGENT_BROWSER_DATA=str(profile))
        reached = browser_reach(root, env, profile, binary=binary)
        assert_true(reached == expected, f"confined: reached {sorted(reached)}")
        # Yolo asks nobody, and its relaxed level still hides the profile.
        reached = browser_reach(root, env, profile, binary=binary, yolo=True)
        assert_true(reached == expected, f"yolo: reached {sorted(reached)}")
        for case in ("off", "approve"):
            case_env = dict(env, UAGENT_SANDBOX="off") if case == "off" else env
            options = {} if case == "off" else {case: True}
            reached = browser_reach(root, case_env, profile, binary=binary, **options)
            assert_true(reached == everything, f"{case}: reached only {sorted(reached)}")
            profile.with_name("browser.moved").rename(profile)
    finally:
        listener.close()


def test_sandbox_detached_log_writes_but_records_do_not(root, home, *, binary):
    """The A4 split, from the sandbox side.

    A detached job's own log pump has to write under ~/.uagent/terminals/logs,
    so that directory is a writable root. The records beside it are not: a
    forged record would misdirect the kill and the expiry unlink that read it.
    """
    require_sandbox(root, home, binary=binary)
    forged = home / ".uagent" / "terminals" / "99999.json"
    logs = home / ".uagent" / "terminals" / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    probe = logs / "probe.log"
    run_once(
        root,
        sandbox_env(home, ""),
        f"echo forged > {forged}; echo live > {probe}; true",
        binary=binary,
    )
    assert_true(probe.exists(), "the detached log directory is not writable")
    assert_true(not forged.exists(), "a command forged a detached record")


def test_sandbox_extra_roots_are_granted_and_screened(root, home, *, binary):
    """UAGENT_SANDBOX_WRITE widens the policy, but never onto agent state."""
    require_sandbox(root, home, binary=binary)
    extra = root / "extra"
    extra.mkdir(exist_ok=True)
    (home / ".uagent").mkdir(parents=True, exist_ok=True)
    inside, denied = extra / "ok.txt", home / "granted.txt"
    # The second root resolves to HOME: it is only rejected because the roots
    # are canonicalised before the ancestor screen sees them.
    env = sandbox_env(home, "", UAGENT_SANDBOX_WRITE=f"{extra}:{home}/.uagent/..")
    run_once(root, env, f"echo a > {inside}; echo b > {denied}; true", binary=binary)
    assert_true(inside.exists(), "an extra root was not granted")
    assert_true(not denied.exists(), "an ancestor of ~/.uagent was granted")


def tcp_reachable(root, home, allow, *, binary, **settings):
    """Whether a sandboxed command can open a TCP connection to a live socket.

    The listener is local and the marker is a file, so a run that never got as
    far as connecting is indistinguishable from one that was denied -- which is
    the assertion either way.
    """
    marker = (
        workspace(root)
        / f"net-{allow}-{len(settings)}-{settings.get('UAGENT_SANDBOX_NET', '')}.txt"
    )
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    connect = f"import socket;socket.create_connection(('127.0.0.1',{port}),2)"
    try:
        run_once(
            root,
            sandbox_env(home, "", **({"UAGENT_SANDBOX_NET": "1" if allow else "0"} | settings)),
            f'{sys.executable} -c "{connect}" && echo up > {marker}',
            binary=binary,
        )
    finally:
        listener.close()
    return marker.exists()


def test_sandbox_network_toggle(root, home, *, binary):
    """Outbound is allowed by default and denied when the setting says so."""
    require_sandbox(root, home, binary=binary)
    assert_true(
        tcp_reachable(root, home, True, binary=binary), "a sandboxed command could not connect"
    )
    assert_true(
        not tcp_reachable(root, home, False, binary=binary), "UAGENT_SANDBOX_NET=0 did not deny"
    )


def test_sandbox_names_itself_when_a_lookup_fails_offline(root, home, *, binary):
    """Without network a refused connection reads as a failed name lookup; the
    sandbox note beside a failure says the network is off only when it is."""
    require_sandbox(root, home, binary=binary)
    failed = "echo 'curl: (6) Could not resolve host: example.invalid' >&2; exit 6"
    offline = tool_output(
        root, sandbox_env(home, "", UAGENT_SANDBOX_NET="0"), failed, binary=binary
    )
    assert_true("reach no network]" in offline, f"the sandbox did not name itself: {offline}")
    online = tool_output(root, sandbox_env(home, ""), failed, binary=binary)
    assert_true(
        "reach no network" not in online, f"an allowed network blamed the sandbox: {online}"
    )


def test_sandbox_refuses_when_it_cannot_enforce(root, home, *, binary):
    """Explicitly configured on, host cannot enforce: the command must not run.

    The degraded tier -- on only by registry default -- is not reachable while
    that default is off, so it is covered where the default flips.
    """
    written = workspace(root) / "should-not-exist.txt"
    env = sandbox_env(home, "", UAGENT_INTERNAL_SANDBOX_UNAVAILABLE="1")
    output = tool_output(root, env, f"echo x > {written}", binary=binary)
    assert_true(not written.exists(), "a command ran on a host that cannot confine it")
    assert_true("UAGENT_SANDBOX" in output, f"the refusal did not explain itself: {output}")


def test_sandbox_escape_hatch_needs_a_person(root, home, *, binary):
    """sandbox=false is mandatory-human: --yolo cannot answer for one."""
    require_sandbox(root, home, binary=binary)
    outside = root / "hatch-headless.txt"
    output = tool_output(
        root,
        sandbox_env(home, ""),
        f"echo x > {outside}",
        headless=True,
        sandbox=False,
        binary=binary,
    )
    assert_true(not outside.exists(), "an unconfined command ran with nobody to approve it")
    assert_true("needs a person's approval" in output, f"the hatch was not denied: {output}")


def test_sandbox_escape_hatch_runs_unconfined_when_approved(root, home, *, binary):
    """Approved at a terminal, the command runs with no wrapper at all."""
    require_sandbox(root, home, binary=binary)
    outside = root / "hatch-approved.txt"
    call = tool_call("run", {"command": f"echo x > {outside}", "sandbox": False})
    with Server([call, event({"content": "hatch-ok"})]) as server:
        code, output = run_pty(
            workspace(root),
            sandbox_env(home, server.url),
            # Wait for the question, not the headline that precedes it: the
            # composer is still reading until `Confirm` takes over, so a "y"
            # typed on the headline is captured as steering and the child then
            # blocks on an answer that has already been consumed.
            [(b"go\n", b"Allow run?  [y] Allow"), (b"y\n", b"hatch-ok"), b"", b"/q\n"],
            timeout=30,
            binary=binary,
        )
    assert_true(code == 0, output)
    # The headline is part of the contract: whoever is asked has to be told
    # which of the two mandatory reasons this is.
    assert_true(b"runs without the OS sandbox" in output, output)
    assert_true(outside.exists(), f"an approved hatch was still confined: {output}")


def test_sandbox_protects_project_authority(root, home, *, binary):
    """The workspace is writable, but not the file that grants authority.

    Seatbelt denies the write. Landlock has no deny form, so there the file is
    put back when the command ends, and the model is told.
    """
    require_sandbox(root, home, binary=binary)
    ws = workspace(root)
    (ws / ".uagent").mkdir(exist_ok=True)
    mcp, scratch = ws / ".mcp.json", ws / ".uagent" / "s.py"
    command = f"echo b > {mcp}; echo c > {scratch}; true"
    output = tool_output(root, sandbox_env(home, ""), command, binary=binary)
    assert_true(not mcp.exists(), "a command wrote the project .mcp.json")
    if sys.platform != "darwin":
        assert_true("restored .mcp.json" in output, f"the restore was silent: {output}")
    # The carve-out is that file, not the folder's own scratch space.
    assert_true(scratch.exists(), "the carve-out took the .uagent directory")


def test_sandbox_keeps_repository_config_and_hooks(root, home, *, binary):
    """A command may commit, but not plant config or hooks your own git runs.

    Denied under Seatbelt; under Landlock undone when the command ends, and said.
    """
    import subprocess

    require_sandbox(root, home, binary=binary)
    ws = workspace(root)
    subprocess.run(["git", "init", "-q", str(ws)], check=True)
    command = (
        "echo x > tracked && git add tracked && "
        "git -c user.email=a@b -c user.name=a commit -qm m && echo committed; "
        "git config core.fsmonitor evil; echo hook > .git/hooks/pre-commit; true"
    )
    output = tool_output(root, sandbox_env(home, ""), command, binary=binary)
    assert_true("committed" in output, output)
    if sys.platform != "darwin":
        assert_true("restored .git/hooks" in output, f"the restore was silent: {output}")
        assert_true("restored .git/config" in output, f"the restore was silent: {output}")
    config = subprocess.run(
        ["git", "-C", str(ws), "config", "--get", "core.fsmonitor"], capture_output=True, text=True
    )
    assert_true(config.stdout.strip() == "", config.stdout)
    assert_true(not (ws / ".git" / "hooks" / "pre-commit").exists(), "a command planted a hook")


def test_sandbox_reports_itself(root, home, *, binary):
    """The two surfaces that answer "what is confining me": /status and startup.

    A root that was asked for and not granted has to be said out loud at
    startup. Finding out from a command that failed hours later is the same
    information arriving too late to act on.
    """
    require_sandbox(root, home, binary=binary)
    with Server([event({"content": "ready-ok"})]) as server:
        code, output = run_pty(
            workspace(root),
            sandbox_env(home, server.url, UAGENT_SANDBOX_WRITE="/"),
            [(b"/status\n", b"sandbox"), b"", b"/q\n"],
            timeout=30,
            binary=binary,
        )
    assert_true(code == 0, output)
    assert_true(b"writes" in output, f"/status did not say what is enforced: {output!r}")
    assert_true(b"not granted as writable: /" in output, f"a dropped root was silent: {output!r}")


def test_status_lists_the_folders_opened_for_reading(root, home, *, binary):
    require_sandbox(root, home, binary=binary)
    opened = home / "Projects" / "other"
    opened.mkdir(parents=True)
    with Server([event({"content": "ready-ok"})]) as server:
        code, output = run_pty(
            workspace(root),
            sandbox_env(home, server.url, UAGENT_SANDBOX_READ=str(opened)),
            [(b"/status\n", b"sandbox reads"), b"", b"/q\n"],
            timeout=30,
            binary=binary,
        )
    assert_true(code == 0, output)
    assert_true(opened.name.encode() in output, f"/status did not list {opened}: {output!r}")


def test_sandbox_explains_a_workspace_it_cannot_grant(root, home, *, binary):
    """A home directory holds ~/.uagent, so it is never a writable root.

    The model has to read that before it plans, and again with the roots when
    a command fails: left to the kernel's "Permission denied" it spends a
    round per guess finding out where it may write.
    """
    require_sandbox(root, home, binary=binary)
    # The web host's state is hidden from commands. Landlock can only read
    # around it, so on Linux this workspace also goes stale for a running one.
    (home / ".uagent" / "web").mkdir(parents=True, exist_ok=True)
    seen = {}

    def route(_, body):
        tools = [str(m.get("content", "")) for m in body["messages"] if m.get("role") == "tool"]
        if tools:
            seen["tool"] = tools[-1]
            return event({"content": "ok"})
        seen["context"] = " ".join(
            str(m.get("content", "")) for m in body["messages"] if m.get("role") == "user"
        )
        return tool_call("run", {"command": "echo x > refused.txt"})

    # Asked and approved, not under yolo, which raises the level to relaxed
    # and so opens the home folder.
    with Server([route]) as server:
        result = run_dialog(
            home, sandbox_env(home, server.url), "y\n", "-p", "go", timeout=30, binary=binary
        )
    assert_true(not (home / "refused.txt").exists(), "a command wrote the home directory")
    assert_true(result.returncode == 0, (result.stdout, result.stderr))
    for said in (seen.get("context", ""), seen.get("tool", "")):
        assert_true("commands cannot write the folder" in said, said)
        assert_true("/tmp" in said and "write_file" in said, said)
        assert_true(("cannot read a file made" in said) == (sys.platform == "linux"), said)
    # And the person is told at startup, before anything has failed.
    with Server([event({"content": "ready-ok"})]) as server:
        code, output = run_pty(
            home,
            sandbox_env(home, server.url),
            [(b"/status\n", b"sandbox"), b"", b"/q\n"],
            timeout=30,
            binary=binary,
        )
    assert_true(code == 0, output)
    assert_true(b"cannot write the folder" in output, f"startup was silent: {output!r}")
    # At the readonly level an unwritable workspace is the point, and
    # startup does not warn about it.
    with Server([event({"content": "ready-ok"})]) as server:
        code, output = run_pty(
            workspace(root),
            sandbox_env(home, server.url, UAGENT_SANDBOX="readonly"),
            [(b"/status\n", b"sandbox"), b"", b"/q\n"],
            timeout=30,
            binary=binary,
        )
    assert_true(code == 0, output)
    assert_true(b"Start in a folder" not in output, output)


def test_sandbox_grants_a_space_and_not_the_state_beside_it(root, home, *, binary):
    """A space is a workspace inside ~/.uagent: its commands write it, and the
    settings and trust store one folder over stay out of reach."""
    require_sandbox(root, home, binary=binary)
    space = home / ".uagent" / "spaces" / "notes"
    config = home / ".uagent" / "config"
    space.mkdir(parents=True)
    config.mkdir(exist_ok=True)
    command = f"echo x > inside.txt; echo x > {config}/planted.json; echo x > ../planted; true"
    with Server([tool_call("run", {"command": command}), event({"content": "ok"})]) as server:
        result = run(
            space, sandbox_env(home, server.url), "--yolo", "-p", "go", timeout=30, binary=binary
        )
    assert_true(result.returncode == 0, (result.stdout, result.stderr))
    assert_true((space / "inside.txt").exists(), "a command could not write its space")
    assert_true(not (config / "planted.json").exists(), "a space reached the saved settings")
    assert_true(not (space.parent / "planted").exists(), "a space wrote beside itself")


def test_sandbox_degrades_when_it_cannot_enforce_by_default(root, home, *, binary):
    """On by default, host cannot enforce: run unconfined and say so.

    The refusal above is for a session that asked for the sandbox by name. A
    session that only inherited the default gets the opposite answer, because
    shipping a default that bricks an old kernel is worse than the exposure.
    """
    written = root / "degraded.txt"
    env = base_env(home, "")
    env["UAGENT_INTERNAL_SANDBOX_UNAVAILABLE"] = "1"
    # --json-stream because a plain headless run prints the answer and nothing
    # else: the notice is an event, and this is where events are observable.
    result = run_once(root, env, f"echo x > {written}", "--json-stream", binary=binary)
    assert_true(written.exists(), f"the degraded tier refused instead: {result.stdout}")
    assert_true("sandbox:" in result.stdout, f"degrading was silent: {result.stdout}")


def test_sandbox_off_leaves_spawning_unchanged(root, home, *, binary):
    """UAGENT_SANDBOX=off: no wrapper, no refusal, no behaviour change."""
    target = root / "unconfined.txt"
    run_once(root, sandbox_env(home, "", UAGENT_SANDBOX="off"), f"echo x > {target}", binary=binary)
    assert_true(target.exists(), "the unsandboxed path changed")


def file_tool_output(root, env, name, arguments, *, binary, yolo=True):
    """What a file tool reported back to the model for one call, headless:
    under yolo, or in ask mode, where nobody answers."""
    seen = []

    def route(_, body):
        contents = [str(m.get("content", "")) for m in body["messages"] if m.get("role") == "tool"]
        if contents:
            seen.append(contents[-1])
            return event({"content": "ok"})
        return tool_call(name, arguments)

    with Server([route]) as server:
        env = dict(env)
        env["UAGENT_BASE_URL"] = server.url
        flags = ("--yolo",) if yolo else ()
        run(workspace(root), env, *flags, "-p", "go", timeout=30, binary=binary)
    return seen[0] if seen else ""


def plant_credentials(home):
    """Stores of each kind: hidden by default, deliberately readable, and one
    nobody has heard of."""
    for relative in (
        ".aws/credentials",
        ".git-credentials",
        ".ssh/id_marker",
        ".config/gh/hosts.yml",
        ".config/mycloud/token",
    ):
        path = home / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"marker-{path.name}\n")
    (home / ".uagent" / "config").mkdir(parents=True, exist_ok=True)
    (home / ".uagent" / "config" / "notes.txt").write_text("marker-notes.txt\n")


def readable(root, env, path, *, binary):
    return f"marker-{path.name}" in tool_output(root, env, f"cat {path}", binary=binary)


def test_sandbox_hides_credential_stores_by_default(root, home, *, binary):
    """Commands cannot read the cloud and settings credentials; ssh and the
    GitHub CLI login stay readable, as does a store nobody listed. The
    unsandboxed control shows each command itself works."""
    require_sandbox(root, home, binary=binary)
    plant_credentials(home)
    env = sandbox_env(home, "")
    for hidden in (
        home / ".aws" / "credentials",
        home / ".git-credentials",
        home / ".uagent" / "config" / "notes.txt",
    ):
        assert_true(not readable(root, env, hidden, binary=binary), f"a command read {hidden}")
        control = sandbox_env(home, "", UAGENT_SANDBOX="off")
        assert_true(
            readable(root, control, hidden, binary=binary), f"the control could not read {hidden}"
        )
    for open_path in (
        home / ".ssh" / "id_marker",
        home / ".config" / "gh" / "hosts.yml",
        home / ".config" / "mycloud" / "token",
    ):
        assert_true(
            readable(root, env, open_path, binary=binary), f"{open_path} should stay readable"
        )


def test_sandbox_hide_and_reveal_adjust_the_list(root, home, *, binary):
    """sandbox.hide adds a store, sandbox.reveal lifts a default."""
    require_sandbox(root, home, binary=binary)
    plant_credentials(home)
    env = sandbox_env(
        home,
        "",
        UAGENT_SANDBOX_HIDE="~/.config/mycloud",
        UAGENT_SANDBOX_REVEAL=str(home / ".aws"),
    )
    assert_true(
        not readable(root, env, home / ".config" / "mycloud" / "token", binary=binary),
        "a path from sandbox.hide was readable",
    )
    assert_true(
        readable(root, env, home / ".aws" / "credentials", binary=binary),
        "a path from sandbox.reveal stayed hidden",
    )


def test_file_tools_refuse_the_hidden_set(root, home, *, binary):
    """The built-in file tools refuse what commands cannot read, with no sandbox
    needed; a revealed path opens for both."""
    plant_credentials(home)
    env = sandbox_env(home, "")
    for path in (home / ".aws" / "credentials", home / ".uagent" / "config" / "notes.txt"):
        output = file_tool_output(root, env, "read_path", {"path": str(path)}, binary=binary)
        assert_true("marker-" not in output, f"a file tool read {path}: {output}")
        assert_true(
            "credentials or uAgent" in output, f"the refusal did not explain itself: {output}"
        )
    ssh = home / ".ssh" / "id_marker"
    assert_true(
        "marker-id_marker"
        in file_tool_output(root, env, "read_path", {"path": str(ssh)}, binary=binary),
        "a file tool could not read ~/.ssh",
    )
    revealed = sandbox_env(home, "", UAGENT_SANDBOX_REVEAL=str(home / ".aws"))
    assert_true(
        "marker-credentials"
        in file_tool_output(
            root, revealed, "read_path", {"path": str(home / ".aws" / "credentials")}, binary=binary
        ),
        "a revealed path stayed refused",
    )


def test_sandbox_levels(root, home, *, binary):
    """relaxed opens the home folder except
    ~/.uagent and the hidden stores."""
    require_sandbox(root, home, binary=binary)
    plant_credentials(home)
    project = home / "project"
    project.mkdir(exist_ok=True)

    def writes(level):
        reached = {}
        env = sandbox_env(home, "", UAGENT_SANDBOX=level)
        targets = {
            "project": project / f"{level}.txt",
            "agent state": home / ".uagent" / f"{level}.txt",
            "hidden store": home / ".aws" / f"{level}.txt",
        }
        command = "; ".join(f"echo x > {path}" for path in targets.values()) + "; true"
        run_once(root, env, command, binary=binary)
        for name, path in targets.items():
            reached[name] = path.exists()
        return reached

    assert_true(
        writes("default") == {"project": False, "agent state": False, "hidden store": False},
        "default reached into the home folder",
    )
    assert_true(
        writes("relaxed") == {"project": True, "agent state": False, "hidden store": False},
        "relaxed is not the home folder minus ~/.uagent and the hidden stores",
    )

    if sys.platform == "darwin" or landlock_abi() >= 4:
        assert_true(
            not tcp_reachable(root, home, None, binary=binary, UAGENT_SANDBOX_NET="0"),
            "sandbox.network=0 left the network open",
        )


def test_a_level_that_is_no_level_refuses_rather_than_opening_the_network(root, home, *, binary):
    """`strict` is gone, and neither it nor a typo may quietly become the
    default, which allows the network: in the environment or saved, commands
    are refused with what to set instead."""
    require_sandbox(root, home, binary=binary)
    for level in ("strict", "0", "loose"):
        env = sandbox_env(home, "", UAGENT_SANDBOX=level)
        output = tool_output(root, env, "echo ran > ran.txt", headless=True, binary=binary)
        assert_true("sandbox.network=false" in output, f"{level}: {output}")
        assert_true(not (workspace(root) / "ran.txt").exists(), f"{level}: the command ran")
    env = sandbox_env(home, "")
    del env["UAGENT_SANDBOX"]
    save_settings(home, binary=binary, UAGENT_SANDBOX="strict")
    output = tool_output(root, env, "echo ran > ran.txt", headless=True, binary=binary)
    assert_true("sandbox.network=false" in output, f"saved: {output}")
    assert_true(not (workspace(root) / "ran.txt").exists(), "saved: the command ran")


def mcp_probe_server(path):
    """A stdio MCP server whose one tool tries to write each path it is given."""
    path.write_text(
        "import json, sys\n"
        "for line in sys.stdin:\n"
        "    message = json.loads(line)\n"
        "    if 'id' not in message:\n"
        "        continue\n"
        "    method = message.get('method')\n"
        "    if method == 'server/discover':\n"
        "        result = {'supportedVersions': ['2026-07-28'], 'capabilities': {'tools': {}}}\n"
        "    elif method == 'tools/list':\n"
        "        result = {'tools': [{'name': 'put', 'description': 'write files', "
        "'inputSchema': {'type': 'object', 'properties': {'paths': {'type': 'array'}}}}]}\n"
        "    elif method == 'tools/call':\n"
        "        for target in message['params']['arguments']['paths']:\n"
        "            try:\n"
        "                open(target, 'w').write('x')\n"
        "            except OSError:\n"
        "                pass\n"
        "        result = {'content': [{'type': 'text', 'text': 'done'}]}\n"
        "    else:\n"
        "        result = {}\n"
        "    print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], 'result': result}), flush=True)\n",
        encoding="utf-8",
    )


def mcp_writes(root, home, entry, *, binary):
    """Whether a trusted project server could write outside the workspace."""
    ws = workspace(root)
    fake = root / "probe_mcp.py"
    mcp_probe_server(fake)
    (ws / ".mcp.json").write_text(
        json.dumps(
            {"mcpServers": {"probe": {"command": sys.executable, "args": [str(fake)], **entry}}}
        )
    )
    outside, inside = root / "mcp-outside.txt", ws / "mcp-inside.txt"
    outside.unlink(missing_ok=True)
    inside.unlink(missing_ok=True)
    with Server(
        [tool_call("probe_put", {"paths": [str(outside), str(inside)]}), event({"content": "ok"})]
    ) as server:
        run(
            ws,
            sandbox_env(home, server.url),
            "--trust-project-config",
            "--yolo",
            "-p",
            "go",
            timeout=30,
            binary=binary,
        )
    assert_true(inside.exists(), "the server could not write the workspace")
    return outside.exists()


def test_mcp_servers_are_confined_unless_they_opt_out(root, home, *, binary):
    """A stdio server runs under the sandbox: it cannot write outside the
    workspace. `"sandbox": false` in its entry lifts that, after the project is
    trusted."""
    require_sandbox(root, home, binary=binary)
    assert_true(
        not mcp_writes(root, home, {}, binary=binary), "an MCP server wrote outside the workspace"
    )
    assert_true(
        mcp_writes(root, home, {"sandbox": False}, binary=binary),
        "sandbox: false did not lift the confinement",
    )


def test_mcp_trust_prompt_names_servers_that_run_unconfined(root, home, *, binary):
    """Trusting a project's .mcp.json is where `"sandbox": false` is approved."""
    ws = workspace(root)
    entry = {"command": "true"}
    (ws / ".mcp.json").write_text(
        json.dumps({"mcpServers": {"confined": entry, "loose": {**entry, "sandbox": False}}})
    )
    with Server([event({"content": "ok"})]) as server:
        _, output = run_pty(
            ws,
            base_env(home, server.url),
            [b"n\n", b"/q\n"],
            # Trust may be painted alongside the first composer. Waiting for
            # it as a later response can miss a prompt already in the buffer.
            startup_marker=b"outside the sandbox: loose",
            timeout=30,
            binary=binary,
        )
    assert_true(b"outside the sandbox: loose." in output, f"the prompt did not say: {output!r}")
