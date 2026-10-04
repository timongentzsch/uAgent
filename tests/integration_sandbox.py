"""OS sandbox: what an agent-run command may and may not write.

Every case here asserts the filesystem, not the message: a command that fails
for the wrong reason still leaves the file absent, and a command that succeeds
in spite of the sandbox leaves it present whatever it printed.

The workspace is a subdirectory of the case root rather than the case root
itself, so that every "outside" path here can live in the case root: that is
neither the workspace nor under any other default root, so a write to one is
denied on both platforms.
"""

import os
import pathlib
import shlex
import socket
import sys

from integration_support import (
    Server,
    assert_true,
    base_env,
    event,
    run,
    run_dialog,
    run_pty,
    tool_call,
)


def workspace(root):
    """The agent's cwd, with the case root left over as unwritable ground."""
    path = root / "ws"
    path.mkdir(exist_ok=True)
    return path


def sandbox_env(home, url, **overrides):
    env = base_env(home, url)
    env["UAGENT_SANDBOX"] = "1"
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
    run_once(root, sandbox_env(home, ""), f"echo x > {probe}", binary=binary)
    escaped = probe.exists()
    probe.unlink(missing_ok=True)
    return not escaped


def test_yolo_keeps_the_sandbox_and_only_its_setting_lifts_it(root, home, *, binary):
    """Yolo means nobody is asked, in either startup form; confinement is the
    sandbox setting's alone."""
    if not sandbox_enforced(root, home, binary=binary):
        return
    for source, sandbox in (("cli", "1"), ("config", "1"), ("cli", "0")):
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
            outside.exists() == (sandbox == "0"),
            f"{source} yolo with UAGENT_SANDBOX={sandbox}: {result.stdout}",
        )


def delegating_server(outside, inside, loosen=None):
    """A parent that delegates one task; its child writes inside the
    workspace, then tries to write outside it. `loosen` runs once the parent
    is up, before it delegates."""

    def route(_, body):
        users = [str(m.get("content", "")) for m in body["messages"] if m.get("role") == "user"]
        results = [m for m in body["messages"] if m.get("role") == "tool"]
        if any("child-task" in text for text in users):
            if results:
                return event({"content": "child-done"})
            return tool_call("run", {"command": f"echo x > {inside}; echo x > {outside}"})
        if "second" not in users and loosen:
            loosen()
            return event({"content": "first-done"})
        if results:
            return event({"content": "parent-done"})
        return tool_call("subagent", {"prompt": "child-task", "mode": "full", "background": False})

    return Server([route] * 6)


def test_a_subagent_is_no_less_confined_than_its_parent(root, home, *, binary):
    """A delegated child approves its own calls, and runs them under the
    sandbox of the session that delegated to it."""
    if not sandbox_enforced(root, home, binary=binary):
        return
    # The second folder's name reads, in a colon-separated list, as itself
    # and its parent: a child handed its parent's roots that way gains one.
    for sandbox, ws in (("1", workspace(root)), ("0", workspace(root)), ("1", root / "ws:..")):
        ws.mkdir(exist_ok=True)
        outside, inside = root / f"{ws.name}-{sandbox}.txt", ws / f"child-{sandbox}.txt"
        with delegating_server(outside, inside) as server:
            env = sandbox_env(home, server.url, UAGENT_SANDBOX=sandbox)
            result = run(ws, env, "--yolo", "-p", "go", timeout=60, binary=binary)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true("parent-done" in result.stdout, result.stdout)
        # The child ran: it wrote where it may. Unconfined only when its
        # parent is.
        assert_true(inside.exists(), f"the child never ran its command: {result.stdout}")
        assert_true(
            outside.exists() == (sandbox == "0"),
            f"child of a parent with UAGENT_SANDBOX={sandbox}: {result.stdout}",
        )


def test_a_subagent_keeps_the_sandbox_its_parent_runs_under(root, home, *, binary):
    """Not the one configured now, and no shell startup file has a say: the
    parent's sandbox is fixed at its start, and so is its child's."""
    if not sandbox_enforced(root, home, binary=binary):
        return
    ws = workspace(root)
    config = home / ".uagent" / ".config"
    config.parent.mkdir(parents=True, exist_ok=True)
    config.write_text("UAGENT_SANDBOX=1\n")
    outside, inside, hooked = root / "later.txt", ws / "later.txt", root / "hooked.txt"
    # A startup file a confined command could have written: the child starts
    # outside the sandbox, so it is started through no shell.
    hook = ws / "hook.sh"
    hook.write_text(f"export UAGENT_SANDBOX=0\ntouch {hooked}\n")

    def loosen():
        config.write_text("UAGENT_SANDBOX=0\n")

    with delegating_server(outside, inside, loosen) as server:
        env = base_env(home, server.url)
        env.pop("UAGENT_SANDBOX", None)
        env["BASH_ENV"] = str(hook)
        result = run_dialog(ws, env, "first\nsecond\n/q\n", "--yolo", timeout=60, binary=binary)
    assert_true(result.returncode == 0, (result.stdout, result.stderr))
    assert_true("parent-done" in result.stdout, result.stdout)
    assert_true(inside.exists(), f"the child never ran its command: {result.stdout}")
    assert_true(not outside.exists(), "a reload loosened the child ahead of its parent")
    assert_true(not hooked.exists(), "the child's launch ran a startup file unconfined")


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
            assert_true(("SUDO_FIXTURE" in output) == (mode != "denied"), (tool, mode, output))
            # Yolo spares the question, not the confinement.
            assert_true(outside.exists() == (mode == "approved"), (tool, mode, output))


def test_yolo_toggle_leaves_sandboxing_alone(root, home, *, binary):
    """Interactive /yolo stops the questions; commands stay confined either way."""
    if not sandbox_enforced(root, home, binary=binary):
        return
    unconfined = root / "toggle-yolo.txt"
    confined = root / "toggle-prompt.txt"

    def route(_, body):
        messages = body["messages"]
        turns = [
            (index, str(message.get("content", "")))
            for index, message in enumerate(messages)
            if message.get("role") == "user"
            and str(message.get("content", "")) in {"unconfined", "confined"}
        ]
        assert_true(turns, messages)
        turn_start, prompt = turns[-1]
        results = [
            message for message in messages[turn_start + 1 :] if message.get("role") == "tool"
        ]
        if results:
            return event({"content": f"{prompt}-ok"})
        target = unconfined if prompt == "unconfined" else confined
        return tool_call("run", {"command": f"echo x > {target}"})

    with Server([route]) as server:
        result = run_dialog(
            workspace(root),
            sandbox_env(home, server.url),
            "/yolo\nunconfined\n/yolo\nconfined\ny\n/q\n",
            timeout=30,
            binary=binary,
        )
    assert_true(result.returncode == 0, (result.stdout, result.stderr))
    assert_true("unconfined-ok" in result.stdout, result.stdout)
    assert_true(
        not unconfined.exists() and not confined.exists(),
        f"a command escaped the sandbox: {result.stdout}",
    )


def test_sandbox_confines_writes_to_the_workspace(root, home, *, binary):
    """Inside the workspace writes land; outside it they do not."""
    if not sandbox_enforced(root, home, binary=binary):
        return
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


def test_sandbox_protects_agent_state(root, home, *, binary):
    """A shell command cannot reach the config or the trust store.

    The unsandboxed control is the point of the case: without it a passing
    assertion could just mean the command was malformed.
    """
    if not sandbox_enforced(root, home, binary=binary):
        return
    target = home / ".uagent" / ".config"
    target.parent.mkdir(parents=True, exist_ok=True)
    command = f"echo UAGENT_YOLO=1 >> {target}"
    run_once(root, sandbox_env(home, ""), command, binary=binary)
    assert_true(not target.exists(), "sandboxed command wrote the config")

    run_once(root, sandbox_env(home, "", UAGENT_SANDBOX="0"), command, binary=binary)
    assert_true(target.exists(), "control run could not write the config either")


def test_sandbox_reads_stay_open(root, home, *, binary):
    """Reads are deliberately unrestricted, and the suite pins that.

    A sandboxed `cat` of the config still reaches model context. Whoever
    narrows this later should have to change a test that says so.
    """
    if not sandbox_enforced(root, home, binary=binary):
        return
    secret = home / "readable.txt"
    secret.write_text("read-me-marker\n")
    output = tool_output(root, sandbox_env(home, ""), f"cat {secret}", binary=binary)
    assert_true("read-me-marker" in output, f"a sandboxed command could not read: {output}")


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
        "control": f"grep -q read-me-marker {root / 'readable.txt'}",
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
    if not sandbox_enforced(root, home, binary=binary):
        return
    # Canonical, because both mechanisms match the resolved path.
    profile = pathlib.Path(os.path.realpath(root)) / "browser"
    profile.mkdir()
    (profile / "Login Data").write_text("login-secret\n")
    (root / "readable.txt").write_text("read-me-marker\n")
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
        # Yolo asks nobody and confines as before.
        reached = browser_reach(root, env, profile, binary=binary, yolo=True)
        assert_true(reached == expected, f"yolo: reached {sorted(reached)}")
        for case in ("off", "approve"):
            case_env = dict(env, UAGENT_SANDBOX="0") if case == "off" else env
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
    if not sandbox_enforced(root, home, binary=binary):
        return
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
    if not sandbox_enforced(root, home, binary=binary):
        return
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


def tcp_reachable(root, home, allow, *, binary):
    """Whether a sandboxed command can open a TCP connection to a live socket.

    The listener is local and the marker is a file, so a run that never got as
    far as connecting is indistinguishable from one that was denied -- which is
    the assertion either way.
    """
    marker = workspace(root) / f"net-{int(allow)}.txt"
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    connect = f"import socket;socket.create_connection(('127.0.0.1',{port}),2)"
    try:
        run_once(
            root,
            sandbox_env(home, "", UAGENT_SANDBOX_NET="1" if allow else "0"),
            f'{sys.executable} -c "{connect}" && echo up > {marker}',
            binary=binary,
        )
    finally:
        listener.close()
    return marker.exists()


def test_sandbox_network_toggle(root, home, *, binary):
    """Outbound is allowed by default and denied when the setting says so."""
    if not sandbox_enforced(root, home, binary=binary):
        return
    assert_true(
        tcp_reachable(root, home, True, binary=binary), "a sandboxed command could not connect"
    )
    assert_true(
        not tcp_reachable(root, home, False, binary=binary), "UAGENT_SANDBOX_NET=0 did not deny"
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
    if not sandbox_enforced(root, home, binary=binary):
        return
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
    if not sandbox_enforced(root, home, binary=binary):
        return
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
    """The workspace is writable, but not the two files that grant authority.

    Seatbelt only: Landlock has no deny form, so on Linux the same guarantee
    would mean not granting the workspace at all. The case skips rather than
    pretending, and the gap is written down in SECURITY.md.
    """
    if sys.platform != "darwin" or not sandbox_enforced(root, home, binary=binary):
        return
    ws = workspace(root)
    (ws / ".uagent").mkdir(exist_ok=True)
    config, mcp, scratch = ws / ".uagent" / ".config", ws / ".mcp.json", ws / ".uagent" / "s.py"
    command = f"echo a > {config}; echo b > {mcp}; echo c > {scratch}; true"
    run_once(root, sandbox_env(home, ""), command, binary=binary)
    assert_true(not config.exists(), "a command wrote the project config")
    assert_true(not mcp.exists(), "a command wrote the project .mcp.json")
    # The carve-out is two files, not the directory: scratch lives beside them.
    assert_true(scratch.exists(), "the carve-out took the whole .uagent directory")


def test_sandbox_keeps_repository_config_and_hooks(root, home, *, binary):
    """A command may commit, but not plant config or hooks your own git runs.

    Seatbelt only, like the project config carve-out above.
    """
    import subprocess

    if sys.platform != "darwin" or not sandbox_enforced(root, home, binary=binary):
        return
    ws = workspace(root)
    subprocess.run(["git", "init", "-q", str(ws)], check=True)
    command = (
        "echo x > tracked && git add tracked && "
        "git -c user.email=a@b -c user.name=a commit -qm m && echo committed; "
        "git config core.fsmonitor evil; echo hook > .git/hooks/pre-commit; true"
    )
    output = tool_output(root, sandbox_env(home, ""), command, binary=binary)
    assert_true("committed" in output, output)
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
    if not sandbox_enforced(root, home, binary=binary):
        return
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
    """UAGENT_SANDBOX=0: no wrapper, no refusal, no behaviour change."""
    target = root / "unconfined.txt"
    run_once(root, sandbox_env(home, "", UAGENT_SANDBOX="0"), f"echo x > {target}", binary=binary)
    assert_true(target.exists(), "the unsandboxed path changed")
