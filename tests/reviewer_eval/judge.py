#!/usr/bin/env python3
"""Serves a decisions endpoint whose answers come from a chat model, so Auto's
review runs against a chat model where no decision model is at hand.

  CHAT_API_KEY=... judge.py --chat-url http://127.0.0.1:8787/v1 \\
      --model claude-haiku-5-5 --port 18790

Point a provider at it: "decisions_url": "http://127.0.0.1:18790/decisions",
"decisions_model": "chat-judge", with a decision_bars row for "chat-judge".
The bars of a real decision model do not carry over; run.py --chat fits them."""

import argparse
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from run import chat


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--chat-url", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18790)
    options = parser.parse_args()
    key = os.environ.get("CHAT_API_KEY", "")
    if not key:
        sys.exit("set CHAT_API_KEY")

    class Judge(BaseHTTPRequestHandler):
        def do_POST(self):  # noqa: N802 (the handler's name)
            if not self.path.endswith("/decisions"):
                self.send_error(404)
                return
            try:
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                answer = chat(options.chat_url, key, options.model, body)
                status = 200
            except (OSError, ValueError, KeyError) as error:
                answer, status = {"error": {"message": str(error)}}, 502
            data = json.dumps(answer).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *_):
            pass

    ThreadingHTTPServer((options.host, options.port), Judge).serve_forever()


if __name__ == "__main__":
    main()
