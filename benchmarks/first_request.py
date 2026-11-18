#!/usr/bin/env python3
"""Records the first model request a harness sends, then refuses it.

    first_request.py serve PORT OUT_DIR     # every POST body is saved, answer 400
    first_request.py count FILE...          # tools and tokens of saved requests

Counting needs tiktoken (o200k_base): uv run --with tiktoken first_request.py count ...
"""

import http.server
import json
import pathlib
import sys


def serve(port, out):
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    seen = [0]

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            seen[0] += 1
            name = self.path.strip("/").replace("/", "_").split("?")[0]
            (out / f"{seen[0]:02d}-{name}.json").write_bytes(body)
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"error":{"message":"recorded","type":"invalid_request_error"}}')

        def do_GET(self):
            (out / "gets.log").open("a").write(self.path + "\n")
            self.send_response(404)
            self.end_headers()

        def log_message(self, *_):
            pass

    http.server.ThreadingHTTPServer(("127.0.0.1", int(port)), Handler).serve_forever()


def text_of(value):
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return "\n".join(text_of(item) for item in value)
    if isinstance(value, dict):
        return text_of(value.get("text") or value.get("content") or "")
    return ""


def count(paths):
    import tiktoken

    encoding = tiktoken.get_encoding("o200k_base")

    def tokens(text):
        return len(encoding.encode(text))

    for path in paths:
        body = json.loads(pathlib.Path(path).read_text())
        tools = body.get("tools") or []
        # Instructions: Anthropic `system`, Responses `instructions`, and the
        # system/developer messages of either message list.
        parts = [text_of(body.get("system")), text_of(body.get("instructions"))]
        for message in body.get("messages") or body.get("input") or []:
            if isinstance(message, dict) and message.get("role") in ("system", "developer"):
                parts.append(text_of(message.get("content")))
        row = {
            "file": pathlib.Path(path).name,
            "model": body.get("model"),
            "tools": len(tools),
            "tool_tokens": tokens(json.dumps(tools)) if tools else 0,
            "instruction_tokens": tokens("\n".join(part for part in parts if part)),
            "request_bytes": len(pathlib.Path(path).read_bytes()),
        }
        row["total"] = row["tool_tokens"] + row["instruction_tokens"]
        print(json.dumps(row))


if __name__ == "__main__":
    if sys.argv[1] == "serve":
        serve(sys.argv[2], sys.argv[3])
    else:
        count(sys.argv[2:])
