"""OS sandbox: what an agent-run command may and may not write.

Every case here asserts the filesystem, not the message: a command that fails
for the wrong reason still leaves the file absent, and a command that succeeds
in spite of the sandbox leaves it present whatever it printed.

The workspace is a subdirectory of the case root rather than the case root
itself, so that every "outside" path here can live in the case root: that is
neither the workspace nor under any other default root, so a write to one is
denied on both platforms.
"""

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


def test_yolo_disables_the_sandbox_from_cli_and_config(root, home, *, binary):
    """Both startup forms of yolo run the default shell path unconfined."""
    if not sandbox_enforced(root, home, binary=binary):
        return
    for source in ("cli", "config"):
        outside = root / f"yolo-{source}.txt"
        command = f"echo x > {outside}"
        with Server([tool_call("run", {"command": command}), event({"content": "ok"})]) as server:
            env = sandbox_env(home, server.url)
            flags = ("--yolo",) if source == "cli" else ()
            if source == "config":
                env["UAGENT_APPROVAL"] = "yolo"
            result = run(workspace(root), env, *flags, "-p", "go", timeout=30, binary=binary)
        assert_true(result.returncode == 0, (result.stdout, result.stderr))
        assert_true(outside.exists(), f"{source} yolo still used the sandbox")


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
            if mode in ("confined", "approved") and not enforced:
                continue
            outside = root / f"{tool}-{mode}.txt"
            command = "sudo sh -c " + shlex.quote(f"echo written > {shlex.quote(str(outside))}")
            arguments = (
                {"command": command}
                if tool == "run"
                else {"path": f"{mode}.sh", "code": command, "packages": []}
            )
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
                        [(b"go\n", b"allow run? [y/N] "), (b"y\n", b"policy-ok"), b"", b"/q\n"],
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
            assert_true(outside.exists() == (mode in ("yolo", "approved")), (tool, mode, output))


def test_yolo_toggle_changes_sandboxing_for_the_next_command(root, home, *, binary):
    """Interactive /yolo disables confinement and restores it when toggled off."""
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
    assert_true(unconfined.exists(), f"/yolo did not disable confinement: {result.stdout}")
    assert_true(
        not confined.exists(),
        f"toggling /yolo off did not restore confinement: {result.stdout}",
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
    assert_true("denied" in output.lower(), f"the hatch was not denied: {output}")


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
            [(b"go\n", b"allow run? [y/N] "), (b"y\n", b"hatch-ok"), b"", b"/q\n"],
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
