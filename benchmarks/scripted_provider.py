#!/usr/bin/env python3
"""Run a benchmark command against a loopback provider that always answers 'ok'."""

import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def messages(model):
    usage = {"input_tokens": 8, "output_tokens": 1}
    message = dict(
        id="msg_fixture",
        type="message",
        role="assistant",
        model=model,
        content=[],
        stop_reason=None,
        stop_sequence=None,
        usage=usage,
    )
    yield "message_start", {"message": message}
    yield "content_block_start", {"index": 0, "content_block": {"type": "text", "text": ""}}
    yield "content_block_delta", {"index": 0, "delta": {"type": "text_delta", "text": "ok"}}
    yield "content_block_stop", {"index": 0}
    yield (
        "message_delta",
        {
            "delta": {"stop_reason": "end_turn", "stop_sequence": None},
            "usage": {"output_tokens": 1},
        },
    )
    yield "message_stop", {}


def responses(model):
    part = {"type": "output_text", "text": "ok", "annotations": []}
    item = {
        "type": "message",
        "id": "msg_fixture",
        "role": "assistant",
        "status": "completed",
        "content": [part],
    }
    response = {
        "object": "response",
        "id": "resp_fixture",
        "created_at": 1,
        "model": model,
        "usage": {"input_tokens": 8, "output_tokens": 1, "total_tokens": 9},
    }
    index = {"item_id": item["id"], "output_index": 0, "content_index": 0}
    yield "response.created", {"response": dict(response, status="in_progress", output=[])}
    yield (
        "response.output_item.added",
        {"output_index": 0, "item": dict(item, status="in_progress", content=[])},
    )
    yield "response.content_part.added", {**index, "part": dict(part, text="")}
    yield "response.output_text.delta", {**index, "delta": "ok"}
    yield "response.output_text.done", {**index, "text": "ok"}
    yield "response.content_part.done", {**index, "part": part}
    yield "response.output_item.done", {"output_index": 0, "item": item}
    yield "response.completed", {"response": dict(response, status="completed", output=[item])}


class Provider(BaseHTTPRequestHandler):
    def do_HEAD(self):
        if self.path.split("?")[0] != "/anthropic/api/hello":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        self.send_error(404)

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        route = self.path.split("?")[0].split("/")[-1]
        if route not in ("messages", "responses") or not data.get("stream"):
            self.send_error(400, "Expected a streaming Messages or Responses request")
            return
        stream = messages(data["model"]) if route == "messages" else responses(data["model"])
        chunks = []
        for i, (event, fields) in enumerate(stream):
            if route == "responses":
                fields["sequence_number"] = i
            packet = json.dumps({"type": event, **fields})
            chunks.append(f"event: {event}\ndata: {packet}\n\n")
        payload = "".join(chunks).encode()
        self.log_message(
            "%s %s tools=%d agent=%s",
            route,
            data["model"],
            len(data.get("tools", [])),
            self.headers.get("User-Agent", "unknown"),
        )
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


if __name__ == "__main__":
    with ThreadingHTTPServer(("127.0.0.1", 0), Provider) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        env = {
            **os.environ,
            "PROXY_URL": f"http://127.0.0.1:{server.server_port}",
            "PROXY_KEY": "fixture",
            "MODEL": "claude-haiku-4-5-20251001",
        }
        sys.exit(subprocess.run(sys.argv[1:], env=env).returncode)
