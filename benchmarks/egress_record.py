#!/usr/bin/env python3
"""Drives a uagent build through the situations in which an install could
contact a host, under a DNS that resolves nothing and logs every name asked
for (dns_sink.py). Prints, per situation, the hosts looked up, then all of
them as JSON.

    egress_record.py BINARY DNS_LOG

Model endpoints are scripted servers on 127.0.0.1, so a name in the log is a
host other than the model. benchmarks/README.md has the container to run it in.
"""

import base64
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time

binary, dns_log = (pathlib.Path(arg).resolve() for arg in sys.argv[1:3])
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tests"))

from integration_support import Server, base_env, event, tool_call  # noqa: E402
from web_support import web_host  # noqa: E402

results = {}


def looked_up(since):
    names = []
    for line in dns_log.read_text().splitlines():
        stamp, name, _ = line.split()
        if float(stamp) >= since and name not in names:
            names.append(name)
    return names


def scenario(name):
    def wrap(run):
        work = pathlib.Path(tempfile.mkdtemp(prefix="egress-"))
        root, home = work / "project", work / "home"
        root.mkdir()
        home.mkdir()
        since = time.time()
        note = ""
        try:
            note = run(root, home) or ""
        except Exception as error:  # a refused connection is an expected end
            note = f"ended: {type(error).__name__}: {str(error)[:160]}"
        time.sleep(1.0)
        results[name] = {"hosts": looked_up(since), "note": note}
        print(f"{name}: {results[name]['hosts']}  {note}", flush=True)
        return run

    return wrap


def headless(root, home, provider, *args, env=None):
    environment = base_env(home, provider)
    environment.update(env or {})
    return subprocess.run(
        [str(binary), "--json", "-p", "go", *args],
        cwd=root,
        env=environment,
        stdin=subprocess.DEVNULL,
        text=True,
        capture_output=True,
        timeout=60,
    )


def say(text):
    return lambda _, _body: event({"content": text})


@scenario("web host starts, a device pairs, the catalogue is read")
def _(root, home):
    with Server([say("unused")]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            client.json("/api/sessions")
            return f"update state: {json.dumps(client.command('update')['result'])[:120]}"


@scenario("a turn in the web client (model on 127.0.0.1)")
def _(root, home):
    with Server([say("done")]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)
            session = client.create(root)
            client.command("submit", session, text="hello")
            client.until(session, lambda value: not value["metadata"].get("turn_active"))
            time.sleep(2.0)  # the title and memory side requests


@scenario("a headless turn (model on 127.0.0.1)")
def _(root, home):
    with Server([say("done")]) as provider:
        return headless(root, home, provider.url).stdout[:80]


@scenario("a headless turn, model endpoint named by host")
def _(root, home):
    providers = {"main": {"base_url": "https://model-endpoint.example/v1", "api_key": "k"}}
    return headless(
        root,
        home,
        "http://127.0.0.1:9",
        env={"UAGENT_PROVIDERS": json.dumps(providers), "UAGENT_MODEL": "main/some-model"},
    ).stdout[:120]


@scenario("About: check for an update now")
def _(root, home):
    with Server([say("unused")]) as provider:
        with web_host(
            binary, root, home, provider.url, extra_env={"UAGENT_INTERNAL_RELEASES": None}
        ) as (client, code, _, _):
            client.pair(code)
            return json.dumps(client.command("update", action="check")["result"])[:200]


def auto_run(root, home, **settings):
    """A command judged in Auto: the model on 127.0.0.1, an OpenRouter provider beside it."""

    def model(_, body):
        if any(message.get("role") == "tool" for message in body["messages"]):
            return event({"content": "done"})
        return tool_call("run", {"command": "printf hi > reviewed.txt"}, call_id="c1")

    with Server([model]) as provider:
        providers = {
            "test": {"base_url": provider.url},
            "router": {"base_url": "https://openrouter.ai/api/v1", "api_key": "k"},
        }
        env = {
            "UAGENT_PROVIDERS": json.dumps(providers),
            "UAGENT_MODEL": "test/test",
            "UAGENT_APPROVAL": "auto",
            **settings,
        }
        return headless(root, home, provider.url, env=env).stdout[:160]


@scenario("Auto approval, an OpenRouter provider configured beside the conversation's")
def _(root, home):
    return auto_run(root, home)


@scenario("Auto approval, that provider named as the reviewer")
def _(root, home):
    return auto_run(root, home, UAGENT_PERMISSION_MODEL="router/")


@scenario("the agent calls web_fetch on a page (yolo, so nobody is asked)")
def _(root, home):
    def model(_, body):
        if any(message.get("role") == "tool" for message in body["messages"]):
            return event({"content": "done"})
        return tool_call("web_fetch", {"url": "https://docs.example.org/page"}, call_id="c1")

    with Server([model]) as provider:
        return headless(root, home, provider.url, "--yolo").stdout[:160]


@scenario("a push to a subscribed device")
def _(root, home):
    def model(_, body):
        if any(message.get("role") == "tool" for message in body["messages"]):
            return event({"content": "done"})
        return tool_call("run", {"command": "echo hi"}, call_id="c1")

    key = subprocess.run(
        "openssl ecparam -name prime256v1 -genkey -noout | openssl ec -pubout -outform DER",
        shell=True,
        capture_output=True,
        check=True,
    ).stdout[-65:]
    with Server([model]) as provider:
        env = {"UAGENT_WEB_PUSH_CONTACT": "mailto:test@example.invalid"}
        with web_host(binary, root, home, provider.url, extra_env=env) as (client, code, _, _):
            client.pair(code)
            client.json(
                "/api/push/subscriptions",
                {
                    "endpoint": "https://fcm.googleapis.com/send/abc",
                    "keys": {
                        "p256dh": base64.urlsafe_b64encode(key).rstrip(b"=").decode(),
                        "auth": base64.urlsafe_b64encode(os.urandom(16)).rstrip(b"=").decode(),
                    },
                },
            )
            session = client.create(root)
            client.command("submit", session, text="run it")
            client.until(session, lambda value: bool(value.get("pending")))
            time.sleep(3.0)


print(json.dumps(results, indent=1))
