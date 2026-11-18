#!/usr/bin/env python3
"""A DNS server that answers every name with 127.0.0.2 and logs who was asked
for: dns_sink.py LOG. Nothing resolves, so nothing leaves; the log is the list
of hosts the software under it tried to reach."""

import socket
import struct
import sys
import time


def name_of(packet, at=12):
    labels = []
    while packet[at]:
        size = packet[at]
        labels.append(packet[at + 1 : at + 1 + size].decode("ascii", "replace"))
        at += 1 + size
    return ".".join(labels), at + 1


def main():
    log = open(sys.argv[1], "a", buffering=1)
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind(("127.0.0.1", 53))
    while True:
        packet, peer = server.recvfrom(4096)
        try:
            name, end = name_of(packet)
            qtype = struct.unpack("!H", packet[end : end + 2])[0]
        except (IndexError, struct.error):
            continue
        log.write(f"{time.time():.3f} {name} {qtype}\n")
        question = packet[12 : end + 4]
        answer = b""
        if qtype == 1:  # A: the sinkhole; anything else has no record
            answer = b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 1, 4) + bytes([127, 0, 0, 2])
        header = packet[:2] + struct.pack("!HHHHH", 0x8180, 1, 1 if answer else 0, 0, 0)
        server.sendto(header + question + answer, peer)


if __name__ == "__main__":
    main()
