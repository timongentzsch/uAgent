"""A local SSH stand-in: commands use another HOME; forwarding owns sockets."""

import json
import os
import pathlib
import signal
import socket
import subprocess
import sys
import threading
import time

args = sys.argv[1:]
control = args[args.index("-S") + 1]
if "-O" in args and args[args.index("-O") + 1] == "check":
    sys.exit(0 if os.path.exists(control) else 255)
elif "-O" in args:
    with socket.socket(socket.AF_UNIX) as command:
        command.connect(control)
        command.sendall((args[args.index("-O") + 1] + " " + args[args.index("-L") + 1]).encode())
        assert command.recv(2) == b"ok"
elif "-M" not in args:
    env = {
        key: value
        for key, value in os.environ.items()
        if key in {"PATH", "SHELL", "LANG", "TERM"} or key.startswith("LC_")
    }
    env["HOME"] = (pathlib.Path(os.environ["HOME"]) / "ssh-fixture-home").read_text().strip()
    sys.exit(subprocess.call(["sh", "-c", args[-1]], env=env))
else:
    peers = []
    forwards = {}

    def stop(_signal, _frame):
        for peer in peers[:]:
            try:
                peer.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        # Keep Close() waiting while the truncated stream reports failure.
        time.sleep(0.2)
        os._exit(0)

    def copy(source, target):
        try:
            while data := source.recv(16384):
                target.sendall(data)
        except OSError:
            pass
        finally:
            try:
                target.shutdown(socket.SHUT_WR)
            except OSError:
                pass

    def forward(listener, target):
        while True:
            try:
                source, _ = listener.accept()
            except OSError:
                return
            destination = socket.socket(socket.AF_UNIX)
            try:
                destination.connect(target)
            except OSError:
                source.close()
                destination.close()
                continue
            peers.extend((source, destination))
            for first, second in ((source, destination), (destination, source)):
                threading.Thread(target=copy, args=(first, second), daemon=True).start()

    signal.signal(signal.SIGTERM, stop)
    with socket.socket(socket.AF_UNIX) as commands:
        commands.bind(control)
        commands.listen()
        while True:
            command, _ = commands.accept()
            with command:
                action, spec = command.recv(4096).decode().split(" ", 1)
                local, remote = spec.split(":", 1)
                if action == "cancel":
                    listener = forwards.pop(spec, None)
                    if listener:
                        listener.close()
                    try:
                        os.unlink(local)
                    except FileNotFoundError:
                        pass
                elif spec not in forwards:
                    listener = socket.socket(socket.AF_UNIX)
                    listener.bind(local)
                    listener.listen()
                    forwards[spec] = listener
                    threading.Thread(target=forward, args=(listener, remote), daemon=True).start()
                snapshot = pathlib.Path(os.environ["HOME"]) / "ssh-fixture-forwards.json"
                pending = snapshot.with_suffix(".tmp")
                pending.write_text(json.dumps(sorted(forwards)))
                pending.replace(snapshot)
                command.sendall(b"ok")
