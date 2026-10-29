#!/usr/bin/env python3
"""Waits for the next Frame Capture of the running game (Ctrl+Shift+F5, ini or debug server) and exits once it is
written, printing the new capture's lines from GTAIV.EFLC.FusionFix.FrameCapture.log: run in the background, its exit
tells whoever started it that there is a capture to read.

It asks FusionFix's debug server (DebugServerPort, through the SSH tunnel) for the capture files every few seconds,
one short request at a time, and waits while the game is not reachable. A capture is new when the log grows past
what it held at the start; the files are complete when the log has stopped growing for a moment.

    python3 -I tools/frame_capture_watch.py [--port 5051] [--every 5] [--timeout 0]
"""
import argparse
import base64
import json
import socket
import sys
import time

LOG = "GTAIV.EFLC.FusionFix.FrameCapture.log"


def call(port, cmd, **args):
    with socket.create_connection(("127.0.0.1", port), timeout=20) as sock, sock.makefile("rb") as file:
        sock.sendall((json.dumps(dict(args, cmd=cmd, id=1)) + "\n").encode())
        reply = json.loads(file.readline())
    if not reply.get("ok"):
        raise RuntimeError(reply.get("error", "unknown error"))
    return reply


def log_size(port):
    """The capture log's size, 0 when there is none yet, None when the game is not reachable."""
    try:
        files = call(port, "files", filter="FrameCapture.log")["files"]
    except (OSError, RuntimeError, ValueError):
        return None
    return next((int(f["size"]) for f in files if f["name"] == LOG), 0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=5051)
    parser.add_argument("--every", type=float, default=5.0)
    parser.add_argument("--timeout", type=float, default=0.0, help="seconds to give up after, 0 never")
    args = parser.parse_args()

    start = time.monotonic()
    baseline = None
    while True:
        size = log_size(args.port)
        if size is not None:
            if baseline is None or size < baseline:
                baseline = size     # first contact, or a new run of the game emptied the log
            elif size > baseline:
                # Written: wait until it stops growing (a capture writes its pictures, then the log)
                while True:
                    time.sleep(2)
                    later = log_size(args.port)
                    if later is None or later == size:
                        break
                    size = later
                text = base64.b64decode(call(args.port, "file", name=LOG)["data"]).decode("utf-8", "replace")
                new = text.encode()[baseline:].decode("utf-8", "replace") if len(text.encode()) >= baseline else text
                heads = [line for line in new.splitlines() if "capture " in line and ("files FrameCapture." in line or "draws in" in line)]
                print("new frame capture:")
                print("\n".join(heads) if heads else new[-2000:])
                return 0
        if args.timeout and time.monotonic() - start > args.timeout:
            print("no new capture")
            return 1
        time.sleep(args.every)


if __name__ == "__main__":
    sys.exit(main())
