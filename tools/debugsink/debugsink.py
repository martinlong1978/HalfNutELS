#!/usr/bin/env python3
"""Receiving end for the ELS motion-trace capture.

Stdlib only - no Flask, no pip install. Run it on the machine named in the
device's "Debug capture URL" setting and leave it running; each POST becomes one
timestamped .csv file plus a one-line summary on stdout.

    python3 debugsink.py                     # 0.0.0.0:8088, ./captures
    python3 debugsink.py --port 9000 --dir /srv/els-captures

The device sends the body with Transfer-Encoding: chunked (its row count is
known but its exact byte count is not, and buffering 200 KB on the device to
measure it would need a second allocation the size of the trace). http.server
does NOT de-chunk for you, so this does it explicitly - that is the one piece of
protocol work here.
"""

import argparse
import datetime
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_BODY = 32 * 1024 * 1024  # a full trace is ~215 KB; this is just a sanity cap


def _read_chunked(rfile, limit):
    """Reassemble a chunked request body."""
    out = bytearray()
    while True:
        line = rfile.readline(64)
        if not line:
            raise ValueError("truncated chunk header")
        line = line.strip()
        if not line:
            continue
        try:
            size = int(line.split(b";")[0], 16)
        except ValueError:
            raise ValueError("bad chunk size %r" % line)
        if size == 0:
            # Consume the (possibly empty) trailer section.
            while True:
                trailer = rfile.readline(1024)
                if not trailer or trailer in (b"\r\n", b"\n"):
                    break
            break
        if len(out) + size > limit:
            raise ValueError("body too large")
        chunk = rfile.read(size)
        if len(chunk) != size:
            raise ValueError("truncated chunk body")
        out += chunk
        rfile.read(2)  # the CRLF after each chunk
    return bytes(out)


def read_body(handler):
    encoding = handler.headers.get("Transfer-Encoding", "").lower()
    if "chunked" in encoding:
        return _read_chunked(handler.rfile, MAX_BODY)
    length = int(handler.headers.get("Content-Length", 0) or 0)
    if length > MAX_BODY:
        raise ValueError("body too large")
    return handler.rfile.read(length)


def safe(text, fallback):
    """Filename-safe fragment: the headers come off the wire, so don't trust them."""
    if not text:
        return fallback
    cleaned = re.sub(r"[^A-Za-z0-9._-]", "", text)
    return cleaned[:32] or fallback


def summarise(body):
    """(rows, seconds, reversals) from the CSV body, for the console line."""
    lines = body.decode("utf-8", "replace").splitlines()
    rows = [ln for ln in lines[1:] if ln.strip()]
    seconds = 0.0
    reversals = 0
    previous_direction = None
    first_time = None
    last_time = None
    for row in rows:
        fields = row.split(",")
        if len(fields) < 8:
            continue
        try:
            time_us = int(fields[0])
            direction = int(fields[7])
        except ValueError:
            continue
        if first_time is None:
            first_time = time_us
        last_time = time_us
        if previous_direction is not None and direction != previous_direction:
            reversals += 1
        previous_direction = direction
    if first_time is not None and last_time is not None:
        seconds = (last_time - first_time) / 1e6
    return len(rows), seconds, reversals


class Handler(BaseHTTPRequestHandler):
    server_version = "ELSDebugSink/1.0"
    protocol_version = "HTTP/1.1"
    out_dir = "captures"

    def log_message(self, fmt, *args):
        pass  # the summary line below is the log

    def _reply(self, code, text):
        payload = (text + "\n").encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        # A convenience so "is it up?" can be answered from a phone.
        self._reply(200, "ELS debug sink is running. POST captures here.")

    def do_POST(self):
        try:
            body = read_body(self)
        except Exception as exc:  # noqa: BLE001 - report anything and stay up
            self._reply(400, "bad request: %s" % exc)
            return
        if not body:
            self._reply(400, "empty body")
            return

        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        device = safe(self.headers.get("X-ELS-Device"), "unknown")
        version = self.headers.get("X-ELS-Version") or "?"
        name = "capture-%s-%s.csv" % (stamp, device)
        path = os.path.join(self.out_dir, name)

        os.makedirs(self.out_dir, exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(body)

        rows, seconds, reversals = summarise(body)
        print(
            "%s  %s  %d rows  %.1f s  %d direction reversals  fw %s  (%d bytes)"
            % (stamp, name, rows, seconds, reversals, version, len(body)),
            flush=True,
        )
        self._reply(200, "stored %s (%d rows)" % (name, rows))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8088)
    parser.add_argument("--dir", default="captures", help="where to write captures")
    args = parser.parse_args()

    Handler.out_dir = args.dir
    os.makedirs(args.dir, exist_ok=True)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(
        "ELS debug sink listening on http://%s:%d/  ->  %s"
        % (args.host, args.port, os.path.abspath(args.dir)),
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
