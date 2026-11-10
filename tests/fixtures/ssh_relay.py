"""A local SSH stand-in: commands use another HOME; forwarding owns sockets."""

import os
import signal
import socket
import subprocess
import sys
import threading
import time

args = sys.argv[1:]
control = args[args.index("-S") + 1]
if "-O" in args:
    with socket.socket(socket.AF_UNIX) as command:
        command.connect(control)
        command.sendall(args[args.index("-L") + 1].encode())
        assert command.recv(2) == b"ok"
elif "-M" not in args:
    env = dict(os.environ, HOME=os.environ["UAGENT_TEST_SSH_HOME"])
    sys.exit(subprocess.call(["sh", "-c", args[-1]], env=env))
else:
    peers = []

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

    def forward(listener, target):
        while True:
            source, _ = listener.accept()
            destination = socket.socket(socket.AF_UNIX)
            destination.connect(target)
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
                local, remote = command.recv(4096).decode().split(":", 1)
                listener = socket.socket(socket.AF_UNIX)
                listener.bind(local)
                listener.listen()
                threading.Thread(target=forward, args=(listener, remote), daemon=True).start()
                command.sendall(b"ok")
