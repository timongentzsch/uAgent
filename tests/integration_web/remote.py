"""Stateless execution gateways: isolated runtimes, receipts and main ownership."""

import base64
import concurrent.futures
import contextlib
import json
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import time
import uuid
from urllib.parse import urlencode

from integration_support import Server, assert_true, base_env, budget, event, tool_call, wait_until
from session_support import remove_runtime
from web_support import web_host
from integration_web.terminal import Socket, listed


def exchange(path, request, *, abandon=False):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(budget(15))
        peer.connect(str(path))
        data = json.dumps(request)
        # Exercise the same bounded chunk framing as the main client.
        for at in range(0, len(data), 65536):
            peer.sendall((json.dumps({"v": 2, "data": data[at:at+65536], "end": at+65536 >= len(data)})+"\n").encode())
        if abandon:
            return
        chunks = []
        with peer.makefile("r") as stream:
            for line in stream:
                frame = json.loads(line)
                chunks.append(frame["data"])
                if frame["end"]:
                    return json.loads("".join(chunks))
        raise AssertionError("gateway closed before response")


@contextlib.contextmanager
def gateway(binary, home, folder, client=None, runtime=None):
    client, runtime = client or uuid.uuid4().hex, runtime or uuid.uuid4().hex
    auth = uuid.uuid4().hex
    startup = json.dumps({"auth":auth,"sandbox":"off"})+"\n"
    env = base_env(home, "http://127.0.0.1:1")
    # Provider config must not be loaded, even when one already exists.
    env["UAGENT_SANDBOX"] = "invalid-remote-env-must-not-be-used"
    started = subprocess.run([binary, "--execution-start", client, runtime, str(folder)], env=env,
                             input=startup,capture_output=True, text=True, timeout=budget(20))
    assert_true(started.returncode == 0, started.stderr + started.stdout)
    identity = json.loads(started.stdout)
    owner = pathlib.Path(identity["socket"]).parent / "owner"
    pid = int(owner.read_text().split()[0])
    try:
        identity["auth"] = auth
        yield identity, lambda request, **kwargs: exchange(identity["socket"], {**request,"auth":auth}, **kwargs)
    finally:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        wait_until(lambda: not pathlib.Path(identity["socket"]).exists(), timeout=budget(10), message="gateway did not stop")


def open_tools(call):
    value = call({"op":"open", "sandbox":"off"})
    assert_true("error" not in value, value)
    return value


def run(call, command, **fields):
    return call({"op":"run", "tool":"run", "id":uuid.uuid4().hex,
                 "args":{"command":command,"description":"Gateway test","yield_ms":10000},
                 "timeout_s":10,"approval":"yolo",**fields})


def test_execution_start_is_stateless_and_reuses_runtime(root, home, *, binary):
    folder = root / "project"
    folder.mkdir()
    (folder / "hello.txt").write_text("remote bytes")
    client, runtime = uuid.uuid4().hex, uuid.uuid4().hex
    with gateway(binary, home, folder, client, runtime) as (identity, call):
        assert_true(exchange(identity["socket"],{"op":"ping"}).get("error"),"unauthenticated gateway accepted request")
        tools = open_tools(call)
        names = {tool["name"] for tool in tools["tools"]}
        assert_true({"run","read_path","write_file","activity"} <= names, names)
        assert_true(not names & {"memory","todo","browser","uagent"}, names)
        assert_true("remote bytes" in call({"op":"run","tool":"read_path","args":{"path":"hello.txt"}})["output"], "read failed")
        assert_true("gateway-ok" in run(call,"printf gateway-ok")["output"], "run failed")
        started = subprocess.run([binary,"--execution-start",client,runtime,str(folder)], env=base_env(home,"http://127.0.0.1:1"),input=json.dumps({"auth":identity["auth"],"sandbox":"off"})+"\n",capture_output=True,text=True,timeout=budget(10))
        assert_true(json.loads(started.stdout)["socket"] == identity["socket"], "runtime replaced")
        assert_true(not (home / ".uagent").exists(), "gateway wrote durable application state")
        assert_true(call({"op":"run","tool":"models","args":{}}).get("error"), "provider RPC exposed")
    remove_runtime(home)


def test_execution_receipts_do_not_repeat_uncertain_mutations(root, home, *, binary):
    with gateway(binary,home,root) as (_,call):
        open_tools(call)
        request = {"op":"run","id":uuid.uuid4().hex,"tool":"run","approval":"yolo","timeout_s":10,
                   "args":{"command":"sleep 0.2; printf x >> once.txt; printf done","description":"Once","yield_ms":10000}}
        call(request,abandon=True)
        receipt = None
        def done():
            nonlocal receipt
            receipt = call({"op":"result","id":request["id"]})
            return receipt.get("done")
        wait_until(done,timeout=budget(10),message="lost response had no receipt")
        assert_true("done" in receipt["result"]["output"],receipt)
        assert_true(call(request) == receipt["result"], "duplicate changed result")
        assert_true((root / "once.txt").read_text() == "x", "mutation ran twice")


def test_execution_clients_have_isolated_cwd_cancellation_and_terminals(root, home, *, binary):
    left,right = root / "left",root / "right"
    left.mkdir();right.mkdir()
    with gateway(binary,home,left) as (a,first),gateway(binary,home,right) as (b,second):
        open_tools(first);open_tools(second)
        assert_true(a["socket"] != b["socket"], "workers shared")
        assert_true(exchange(b["socket"],{"op":"ping","auth":a["auth"]}).get("error"),"another main credential reached this worker")
        request = {"op":"run","id":uuid.uuid4().hex,"tool":"run","approval":"yolo","timeout_s":10,
                   "args":{"command":"sleep 8","description":"Cancel me","yield_ms":10000}}
        with concurrent.futures.ThreadPoolExecutor() as pool:
            pending = pool.submit(first,request)
            wait_until(lambda:first({"op":"result","id":request["id"]}).get("known"),timeout=budget(3),message="command not started")
            assert_true(not second({"op":"cancel","id":request["id"]})["known"],"other client could cancel")
            before = time.monotonic()
            first({"op":"cancel","id":request["id"]})
            result = pending.result(timeout=budget(3))
            assert_true(time.monotonic()-before < budget(3) and result["status"] == 2,result)
        assert_true(str(right) in run(second,"pwd")["output"],"cwd crossed clients")
        shell = second({"op":"terminal","action":"open","id":uuid.uuid4().hex})["terminal"]
        viewer = uuid.uuid4().hex
        def terminal(action,**fields):
            return second({"op":"terminal","action":action,"terminal":shell,"viewer":viewer,**fields})
        assert_true(terminal("attach") == {},"attach failed")
        terminal("write",data=b"printf 'terminal-survives\n'\n".hex())
        output = bytearray()
        def seen():
            output.extend(bytes.fromhex(terminal("read")["data"]))
            return b"terminal-survives" in output
        wait_until(seen,timeout=budget(10),message="terminal output missing")
        terminal("detach")
        viewer = uuid.uuid4().hex
        assert_true(terminal("attach") == {},"terminal did not survive transport")
        terminal("close")


def test_execution_binary_files_revisions_and_remote_approval(root, home, *, binary):
    project = root / "project";project.mkdir()
    outside = root / "outside";outside.write_text("outside")
    (project / "link").symlink_to(outside)
    (project / "text").write_text("before")
    binary_bytes = bytes(range(256))*2048
    with gateway(binary,home,project) as (_,call):
        open_tools(call)
        checked = call({"op":"check","tool":"read_path","approval":"ask","args":{"path":"link"}})
        assert_true(checked["approval"] != 0,checked)
        (project / "link").unlink()
        (project / "link").symlink_to(project / "text")
        refused = call({"op":"run","tool":"read_path","id":uuid.uuid4().hex,"approval":"ask","args":checked["args"],"scope":checked["scope"]})
        assert_true(refused["status"] == 1 and "changed after approval" in refused["output"],refused)
        value = call({"op":"run","tool":"write_file","id":uuid.uuid4().hex,"approval":"yolo","args":{"path":"blob.bin","content_hex":binary_bytes.hex()}})
        assert_true(value["status"] == 0,value)
        raw = call({"op":"files","mode":"raw","path":"blob.bin"})
        assert_true(bytes.fromhex(raw["data"]) == binary_bytes,"binary changed")
        read = call({"op":"files","mode":"read","path":"text"})
        (project / "text").write_text("outside edit")
        conflict = call({"op":"files","mode":"write","path":"text","text":"overwrite","revision":read["revision"]})
        assert_true(conflict.get("conflict"),conflict)
        assert_true((project / "text").read_text() == "outside edit","conflict overwritten")


def test_remote_auth_failure_stops_until_manual_reconnect(root, home, *, binary):
    tools = root / "bin"
    tools.mkdir()
    ssh = tools / "ssh"
    ssh.write_text(
        '#!/bin/sh\necho attempt >> "$HOME/ssh-attempts"\n'
        'echo "Permission denied (publickey)." >&2\nexit 255\n'
    )
    ssh.chmod(0o700)
    attempts = home / "ssh-attempts"
    with web_host(
        binary,
        root,
        home,
        "http://127.0.0.1:1",
        extra_env={"PATH": f"{tools}:{os.environ['PATH']}"},
    ) as (client, code, _, _):
        client.pair(code)
        status, value, _ = client.json("/api/hosts", {"action": "add", "address": "GPU"})
        assert_true(status == 200, value)
        host = value["id"]

        def failed():
            rows = client.json("/api/hosts")[1]["hosts"]
            return rows and rows[0]["phase"] == "disconnected" and rows[0]["error"]

        wait_until(failed, timeout=budget(10), message="authentication failure did not settle")
        assert_true("ssh-add" in failed(), failed())
        time.sleep(2)
        assert_true(len(attempts.read_text().splitlines()) == 1, "authentication was retried")
        client.json("/api/hosts", {"action": "reconnect", "id": host})
        wait_until(
            lambda: len(attempts.read_text().splitlines()) == 2,
            timeout=budget(10),
            message="manual reconnect did not start a fresh attempt",
        )



def test_remote_main_owns_provider_history_and_settings(root, home, *, binary):
    remote_home = root / "remote-home";remote_home.mkdir()
    project = root / "remote-project";project.mkdir()
    canary = root / "main-canary";canary.write_text("untouched")
    build = subprocess.check_output([binary,"--build-id"],text=True).strip()
    installed = remote_home / ".uagent/remote/builds" / build / "uagent"
    installed.parent.mkdir(parents=True)
    installed.symlink_to(binary)
    (home / "ssh-fixture-home").write_text(str(remote_home))
    tools = root / "bin";tools.mkdir()
    ssh = tools / "ssh"
    source = pathlib.Path(__file__).resolve().parents[1] / "fixtures/ssh_relay.py"
    ssh.write_text(f"#!{sys.executable}\n"+source.read_text());ssh.chmod(0o700)
    (project / "AGENTS.md").write_text("Remote workspace instruction canary.")
    (project / ".mcp.json").write_text('{"mcpServers":{"blocked":{"command":"sh","args":["-c","touch forbidden"],"env":{"KEY":"${MAIN_SECRET}"}}}}')
    (project / "pixel.png").write_bytes(base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aTp8AAAAASUVORK5CYII="))
    def answer(_,body):
        assert_true("Remote workspace instruction canary" in json.dumps(body),"remote project prompt missing")
        return tool_call("run",{"command":'printf remote-command-ok > remote.txt; printf %s "${MAIN_SECRET-}${OPENROUTER_API_KEY-}${UAGENT_PROVIDERS-}" > remote-env; pwd',"description":"Write remote marker","yield_ms":10000})
    def received_image(_,body):
        assert_true('"image_url"' in json.dumps(body),"remote image did not reach main provider")
        return event({"content":"main-provider-complete"})
    environment = {"PATH":f"{tools}:{os.environ['PATH']}","UAGENT_TEST_SSH_HOME":str(remote_home),"MAIN_SECRET":"main-private-canary","OPENROUTER_API_KEY":"private-provider-key-canary"}
    with Server([answer,tool_call("read_path",{"path":"pixel.png"}),received_image]) as provider:
        with web_host(binary,root,home,provider.url,extra_env=environment) as (web,code,_,_):
            web.pair(code)
            status,value,_ = web.json("/api/hosts",{"action":"add","address":"peer"})
            assert_true(status == 200,value)
            host = value["id"]
            def phase():return web.json("/api/hosts")[1]["hosts"][0]["phase"]
            wait_until(lambda:phase() == "connected",timeout=budget(12),message="SSH did not connect")
            session = web.command("create",cwd=str(project),host=host)["session"]
            assert_true(session["host"] == host,session)
            session = web.command("activate",session)["session"]
            web.until(session,lambda value:value["metadata"]["status"] == "idle")
            web.command("submit",session,text="/yolo")
            web.until(session,lambda value:value["state"].get("yolo",False))
            web.command("submit",session,text="Write a marker in the remote project")
            web.until(session,lambda value:"main-provider-complete" in json.dumps(value))
            assert_true((project / "remote.txt").read_text() == "remote-command-ok","remote command missing")
            assert_true((project / "remote-env").read_text() == "","main settings or secrets reached remote child")
            assert_true(canary.read_text() == "untouched","main canary changed")
            assert_true(not (project / "forbidden").exists(),"remote MCP ran")
            assert_true(any((home / ".uagent/history").rglob("*.json")),"history not main owned")
            assert_true(list((remote_home / ".uagent").iterdir()) == [remote_home / ".uagent/remote"],"remote acquired app state")
            result = web.json(f"/api/sessions/{session['id']}/files?"+urlencode({"read":"remote.txt"}))[1]
            assert_true(result["text"] == "remote-command-ok",result)
            # A worker can disappear while its SSH master remains healthy.
            rows = subprocess.check_output(["ps","-axo","pid=,args="],text=True)
            for row in rows.splitlines():
                fields = row.split(maxsplit=1)
                if len(fields) == 2 and "--execution-worker" in fields[1] and str(project) in fields[1]:
                    with contextlib.suppress(ProcessLookupError): os.kill(int(fields[0]),signal.SIGTERM)
            time.sleep(0.2)
            restarted = web.json(f"/api/sessions/{session['id']}/files?read=remote.txt")
            assert_true(restarted[0] == 200,restarted)
            terminal = web.command("terminal",action="open",cwd=str(project),host=host)["result"]["id"]
            viewer = Socket(web,f"/api/terminals/{terminal}")
            viewer.type("export REMOTE_PTY_SURVIVAL=still-here\n")
            viewer.type('printf "before-restart-%s\\n" "$REMOTE_PTY_SURVIVAL"\n')
            viewer.until("before-restart-still-here")
            viewer.close()
            web.json("/api/hosts",{"action":"disconnect","id":host})
            wait_until(lambda:phase() == "disconnected",timeout=budget(5),message="disconnect ignored")
            assert_true(web.snapshot(session)["metadata"]["host"] == host,"offline history lost")
            web.json("/api/hosts",{"action":"reconnect","id":host})
            wait_until(lambda:phase() == "connected",timeout=budget(12),message="manual reconnect failed")
            recovered = web.json(f"/api/sessions/{session['id']}/files?read=remote.txt")
            assert_true(recovered[0] == 200,recovered)
        with web_host(binary,root,home,provider.url,extra_env=environment) as (web,code,_,_):
            web.pair(code)
            assert_true(any(row["id"] == terminal for row in listed(web)),"main restart lost terminal catalogue")
            web.json("/api/hosts",{"action":"reconnect","id":host})
            wait_until(lambda:web.json("/api/hosts")[1]["hosts"][0]["phase"] == "connected",timeout=budget(12),message="restart reconnect failed")
            viewer = Socket(web,f"/api/terminals/{terminal}")
            viewer.type('printf "after-restart-%s\\n" "$REMOTE_PTY_SURVIVAL"\n')
            output,_,closed = viewer.until("after-restart-still-here")
            assert_true(closed is None,output)
            viewer.close()
            web.command("terminal",action="close",id=terminal)
    # Main session teardown closes owned commands. Stop temporary idle workers
    # under this test HOME before its directory is removed.
    rows = subprocess.check_output(["ps","-axo","pid=,args="],text=True)
    for row in rows.splitlines():
        fields=row.split(maxsplit=1)
        if len(fields)==2 and "--execution-worker" in fields[1] and str(project) in fields[1]:
            with contextlib.suppress(ProcessLookupError):os.kill(int(fields[0]),signal.SIGTERM)
    remove_runtime(remote_home)
