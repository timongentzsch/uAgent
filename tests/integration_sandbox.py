"""OS sandbox: what an agent-run command may and may not write.

Every case here asserts the filesystem, not the message: a command that fails
for the wrong reason still leaves the file absent, and a command that succeeds
in spite of the sandbox leaves it present whatever it printed.

The workspace is a subdirectory of the case root rather than the case root
itself, so that every "outside" path here can live in the case root: that is
neither the workspace nor under any other default root, so a write to one is
denied on both platforms.
"""

import socket
import sys

from integration_support import (
    Server,
    assert_true,
    base_env,
    event,
    run,
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


def run_once(root, env, command, *flags):
    """One turn that runs `command` through the shell tool."""
    with Server([tool_call("run", {"command": command}), event({"content": "ok"})]) as server:
        env = dict(env)
        env["UAGENT_BASE_URL"] = server.url
        return run(workspace(root), env, "--yolo", *flags, "-p", "go", timeout=30)


def tool_output(root, env, command, **arguments):
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
        run(workspace(root), env, "--yolo", "-p", "go", timeout=30)
    return seen[0] if seen else ""


def sandbox_enforced(root, home):
    """True when this host actually confines writes.

    Probing by behaviour rather than by platform: the answer on Linux depends
    on the kernel, and a suite that assumed enforcement would report a missing
    Landlock as a sandbox escape.
    """
    probe = root / "sandbox-probe"
    run_once(root, sandbox_env(home, ""), f"echo x > {probe}")
    escaped = probe.exists()
    probe.unlink(missing_ok=True)
    return not escaped


def test_sandbox_confines_writes_to_the_workspace(root, home):
    """Inside the workspace writes land; outside it they do not."""
    if not sandbox_enforced(root, home):
        return
    inside, outside = workspace(root) / "inside.txt", root / "outside.txt"
    # No trailing `true`: the shell's exit status has to carry the failure, or
    # the hint below has nothing to attach itself to.
    output = tool_output(root, sandbox_env(home, ""), f"echo in > {inside}; echo out > {outside}")
    assert_true(inside.exists(), "workspace write was blocked")
    assert_true(not outside.exists(), "wrote outside the workspace")
    # The errno the shell prints names a permission, not the policy that
    # withheld it. Whoever reads the failure has to be told which it was.
    assert_true("[sandbox:" in output, f"a refused write did not name the sandbox: {output}")


def test_sandbox_protects_agent_state(root, home):
    """A shell command cannot reach the config or the trust store.

    The unsandboxed control is the point of the case: without it a passing
    assertion could just mean the command was malformed.
    """
    if not sandbox_enforced(root, home):
        return
    target = home / ".uagent" / ".config"
    target.parent.mkdir(parents=True, exist_ok=True)
    command = f"echo UAGENT_YOLO=1 >> {target}"
    run_once(root, sandbox_env(home, ""), command)
    assert_true(not target.exists(), "sandboxed command wrote the config")

    run_once(root, sandbox_env(home, "", UAGENT_SANDBOX="0"), command)
    assert_true(target.exists(), "control run could not write the config either")


def test_sandbox_reads_stay_open(root, home):
    """Reads are deliberately unrestricted, and the suite pins that.

    A sandboxed `cat` of the config still reaches model context. Whoever
    narrows this later should have to change a test that says so.
    """
    if not sandbox_enforced(root, home):
        return
    secret = home / "readable.txt"
    secret.write_text("read-me-marker\n")
    output = tool_output(root, sandbox_env(home, ""), f"cat {secret}")
    assert_true("read-me-marker" in output, f"a sandboxed command could not read: {output}")


def test_sandbox_detached_log_writes_but_records_do_not(root, home):
    """The A4 split, from the sandbox side.

    A detached job's own log pump has to write under ~/.uagent/terminals/logs,
    so that directory is a writable root. The records beside it are not: a
    forged record would misdirect the kill and the expiry unlink that read it.
    """
    if not sandbox_enforced(root, home):
        return
    forged = home / ".uagent" / "terminals" / "99999.json"
    logs = home / ".uagent" / "terminals" / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    probe = logs / "probe.log"
    run_once(root, sandbox_env(home, ""), f"echo forged > {forged}; echo live > {probe}; true")
    assert_true(probe.exists(), "the detached log directory is not writable")
    assert_true(not forged.exists(), "a command forged a detached record")


def test_sandbox_extra_roots_are_granted_and_screened(root, home):
    """UAGENT_SANDBOX_WRITE widens the policy, but never onto agent state."""
    if not sandbox_enforced(root, home):
        return
    extra = root / "extra"
    extra.mkdir(exist_ok=True)
    (home / ".uagent").mkdir(parents=True, exist_ok=True)
    inside, denied = extra / "ok.txt", home / "granted.txt"
    # The second root resolves to HOME: it is only rejected because the roots
    # are canonicalised before the ancestor screen sees them.
    env = sandbox_env(home, "", UAGENT_SANDBOX_WRITE=f"{extra}:{home}/.uagent/..")
    run_once(root, env, f"echo a > {inside}; echo b > {denied}; true")
    assert_true(inside.exists(), "an extra root was not granted")
    assert_true(not denied.exists(), "an ancestor of ~/.uagent was granted")


def tcp_reachable(root, home, allow):
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
        )
    finally:
        listener.close()
    return marker.exists()


def test_sandbox_network_toggle(root, home):
    """Outbound is allowed by default and denied when the setting says so."""
    if not sandbox_enforced(root, home):
        return
    assert_true(tcp_reachable(root, home, True), "a sandboxed command could not connect")
    assert_true(not tcp_reachable(root, home, False), "UAGENT_SANDBOX_NET=0 did not deny")


def test_sandbox_refuses_when_it_cannot_enforce(root, home):
    """Explicitly configured on, host cannot enforce: the command must not run.

    The degraded tier -- on only by registry default -- is not reachable while
    that default is off, so it is covered where the default flips.
    """
    written = workspace(root) / "should-not-exist.txt"
    env = sandbox_env(home, "", UAGENT_INTERNAL_SANDBOX_UNAVAILABLE="1")
    output = tool_output(root, env, f"echo x > {written}")
    assert_true(not written.exists(), "a command ran on a host that cannot confine it")
    assert_true("UAGENT_SANDBOX" in output, f"the refusal did not explain itself: {output}")


def test_sandbox_escape_hatch_needs_a_person(root, home):
    """sandbox=false is mandatory-human: --yolo cannot answer for one."""
    if not sandbox_enforced(root, home):
        return
    outside = root / "hatch-headless.txt"
    output = tool_output(root, sandbox_env(home, ""), f"echo x > {outside}", sandbox=False)
    assert_true(not outside.exists(), "an unconfined command ran with nobody to approve it")
    assert_true("denied" in output.lower(), f"the hatch was not denied: {output}")


def test_sandbox_escape_hatch_runs_unconfined_when_approved(root, home):
    """Approved at a terminal, the command runs with no wrapper at all."""
    if not sandbox_enforced(root, home):
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
        )
    assert_true(code == 0, output)
    # The headline is part of the contract: whoever is asked has to be told
    # which of the two mandatory reasons this is.
    assert_true(b"runs without the OS sandbox" in output, output)
    assert_true(outside.exists(), f"an approved hatch was still confined: {output}")


def test_sandbox_protects_project_authority(root, home):
    """The workspace is writable, but not the two files that grant authority.

    Seatbelt only: Landlock has no deny form, so on Linux the same guarantee
    would mean not granting the workspace at all. The case skips rather than
    pretending, and the gap is written down in SECURITY.md.
    """
    if sys.platform != "darwin" or not sandbox_enforced(root, home):
        return
    ws = workspace(root)
    (ws / ".uagent").mkdir(exist_ok=True)
    config, mcp, scratch = ws / ".uagent" / ".config", ws / ".mcp.json", ws / ".uagent" / "s.py"
    command = f"echo a > {config}; echo b > {mcp}; echo c > {scratch}; true"
    run_once(root, sandbox_env(home, ""), command)
    assert_true(not config.exists(), "a command wrote the project config")
    assert_true(not mcp.exists(), "a command wrote the project .mcp.json")
    # The carve-out is two files, not the directory: scratch lives beside them.
    assert_true(scratch.exists(), "the carve-out took the whole .uagent directory")


def test_sandbox_reports_itself(root, home):
    """The two surfaces that answer "what is confining me": /status and startup.

    A root that was asked for and not granted has to be said out loud at
    startup. Finding out from a command that failed hours later is the same
    information arriving too late to act on.
    """
    if not sandbox_enforced(root, home):
        return
    with Server([event({"content": "ready-ok"})]) as server:
        code, output = run_pty(
            workspace(root),
            sandbox_env(home, server.url, UAGENT_SANDBOX_WRITE="/"),
            [(b"/status\n", b"sandbox"), b"", b"/q\n"],
            timeout=30,
        )
    assert_true(code == 0, output)
    assert_true(b"writes" in output, f"/status did not say what is enforced: {output!r}")
    assert_true(b"not granted as writable: /" in output, f"a dropped root was silent: {output!r}")


def test_sandbox_degrades_when_it_cannot_enforce_by_default(root, home):
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
    result = run_once(root, env, f"echo x > {written}", "--json-stream")
    assert_true(written.exists(), f"the degraded tier refused instead: {result.stdout}")
    assert_true("sandbox:" in result.stdout, f"degrading was silent: {result.stdout}")


def test_sandbox_off_leaves_spawning_unchanged(root, home):
    """UAGENT_SANDBOX=0: no wrapper, no refusal, no behaviour change."""
    target = root / "unconfined.txt"
    run_once(root, sandbox_env(home, "", UAGENT_SANDBOX="0"), f"echo x > {target}")
    assert_true(target.exists(), "the unsandboxed path changed")
