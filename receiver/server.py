#!/usr/bin/env python3
"""
BLACKBOX-Z telemetry receiver — stdlib only, runs on your laptop today and
on the LinuxONE runtime later, unchanged.

Run:    python server.py [port]        (default 5000)

Endpoints:
  POST /api/telemetry      node reading.
                           node1: id, ms, count, rms_vib, tilt,
                                  moisture_pct, leak, hash
                           node2: id, ms, count, sound_db, alarm, hash
                           -> 200 {"ok":true,"command":null}
                              400 on invalid JSON / missing "id"
  GET  /api/latest         last reading per node (JSON)
  GET  /api/history?n=50   last n readings (JSON)
  GET  /                   live status page (auto-refresh, real values only)
  GET  /health             liveness {"status":"ok",...}

Bind: 0.0.0.0 so LAN devices (the ESP32) can reach it. Windows Firewall:
allow Python inbound on TCP 5000 the first time it runs.
"""
import html as _html
import json
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
MAX_BODY = 64 * 1024
START = time.time()

_lock = threading.Lock()
_nodes = {}                      # node id -> latest record
_history = deque(maxlen=1000)    # all recent records, oldest first

METRIC_FIELDS = ("rms_vib", "tilt", "moisture_pct", "leak", "sound_db", "alarm")


def summarize(rec):
    parts = [f"{k}={rec[k]}" for k in METRIC_FIELDS if k in rec]
    return " ".join(parts) if parts else "(no metric fields)"


def page():
    with _lock:
        items = list(_nodes.items())
    if items:
        rows = "".join(
            "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>".format(
                _html.escape(str(nid)),
                _html.escape(summarize(rec)),
                _html.escape(str(rec.get("_recv_t", ""))),
                _html.escape(str(rec.get("ms", ""))),
            )
            for nid, rec in items
        )
        table = ("<table><tr><th>node</th><th>latest values</th>"
                 "<th>received</th><th>node ms</th></tr>" + rows + "</table>")
    else:
        table = ("<p>Waiting for data — no node has POSTed yet.<br>"
                 "Flash the firmware, set serverUrl to THIS machine's LAN IP, "
                 "and watch this page.</p>")
    return (
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<title>BLACKBOX-Z receiver</title>"
        "<meta http-equiv='refresh' content='2'>"
        "<style>body{font:14px/1.5 ui-monospace,Consolas,monospace;"
        "background:#0d1117;color:#d8dee9;margin:24px}"
        "table{border-collapse:collapse;min-width:640px}"
        "th,td{border:1px solid #30363d;padding:6px 10px;text-align:left}"
        "th{background:#161b22;color:#8b949e}"
        "h2 span{color:#3fb950}.s{color:#8b949e}</style></head><body>"
        "<h2>BLACKBOX-Z receiver <span>LIVE</span></h2>"
        "<p class='s'>POST /api/telemetry &middot; GET /api/latest "
        "&middot; GET /api/history?n=50</p>" + table + "</body></html>"
    )


class Handler(BaseHTTPRequestHandler):
    server_version = "BBZReceiver/1.0"

    def log_message(self, fmt, *args):
        sys.stdout.write("%s %s\n" % (time.strftime("%H:%M:%S"), fmt % args))

    def _send(self, code, obj, ctype="application/json"):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path in ("/", "/index.html"):
            self._send(200, page(), "text/html; charset=utf-8")
        elif u.path == "/api/latest":
            with _lock:
                snap = {k: dict(v) for k, v in _nodes.items()}
            self._send(200, {"ok": True, "nodes": snap})
        elif u.path == "/api/history":
            try:
                n = int(parse_qs(u.query).get("n", ["50"])[0])
            except (TypeError, ValueError):
                n = 50
            with _lock:
                items = list(_history)[-max(1, min(n, 1000)):]
            self._send(200, {"ok": True, "count": len(items), "data": items})
        elif u.path == "/health":
            with _lock:
                n = len(_nodes)
            self._send(200, {"status": "ok", "nodes": n,
                             "uptime_s": int(time.time() - START)})
        else:
            self._send(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        if urlparse(self.path).path != "/api/telemetry":
            self._send(404, {"ok": False, "error": "unknown endpoint"})
            return
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        if n <= 0 or n > MAX_BODY:
            self._send(400, {"ok": False, "error": "missing or oversized body"})
            return
        raw = self.rfile.read(n)
        try:
            rec = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self._send(400, {"ok": False, "error": "invalid JSON"})
            return
        if not isinstance(rec, dict) or not str(rec.get("id") or "").strip():
            self._send(400, {"ok": False, "error": "missing 'id'"})
            return
        rec["_recv_t"] = time.strftime("%Y-%m-%d %H:%M:%S")
        with _lock:
            _nodes[rec["id"]] = rec
            _history.append(rec)
        print("[%s] %-6s %s" % (rec["_recv_t"], rec["id"], summarize(rec)))
        # "command" is how the server can drive a node later
        # (e.g. {"command":"SHUTDOWN"} / {"command":"ALARM"}); none by default.
        self._send(200, {"ok": True, "command": None})


if __name__ == "__main__":
    httpd = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print("BLACKBOX-Z receiver listening on http://0.0.0.0:%d/" % PORT)
    print("Point the ESP32 serverUrl at THIS machine's LAN IP:")
    print("  http://<this-machine-ip>:%d/api/telemetry" % PORT)
    print("Status page: http://127.0.0.1:%d/   (Ctrl+C to stop)" % PORT)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
