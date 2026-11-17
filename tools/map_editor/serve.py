#!/usr/bin/env python3
"""The map editor's local server: the page, the exported maps, and map_ext.cfg.

    python3 tools/map_editor/serve.py        # then open http://localhost:8089

GET  /ext?map=N   that map's lines of map_ext.cfg
POST /ext?map=N   replaces that map's lines (other maps' lines are kept);
                  ?thor=1 also copies the file to the Thor's files/ (adb,
                  run-as; nothing else on the device is touched)

map_ext.cfg is written in the repo root, where the desktop game reads it
(src/kol_map_source.cpp); a running game picks a save up within half a second.
"""
import http.server, json, os, subprocess, sys, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CFG = os.environ.get("KOL_MAP_EXT_FILE", os.path.join(ROOT, "map_ext.cfg"))
PORT = int(os.environ.get("MAP_EDITOR_PORT", "8089"))
PACKAGE = "com.koldecomp.game"


def read_lines():
    try:
        with open(CFG) as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def map_of(line):
    parts = line.split()
    if len(parts) == 7 and parts[0] == "ov":   # an override: ov map layer row col entry orig
        return parts[1]
    return parts[0] if len(parts) == 5 and not line.startswith("#") else None


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=HERE, **k)

    def log_message(self, fmt, *args):
        if "/ext" in (args[0] if args else ""):
            sys.stderr.write("%s\n" % (fmt % args))

    def reply(self, code, body, ctype="text/plain"):
        data = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == "/ext":
            m = urllib.parse.parse_qs(u.query).get("map", [""])[0]
            self.reply(200, "\n".join(l for l in read_lines() if map_of(l) == m) + "\n")
            return
        if u.path == "/maps":
            names = sorted(n for n in os.listdir(os.path.join(HERE, "maps"))
                           if n.endswith(".json")) if os.path.isdir(os.path.join(HERE, "maps")) else []
            self.reply(200, json.dumps(names), "application/json")
            return
        super().do_GET()

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        m = q.get("map", [""])[0]
        if u.path != "/ext" or not m.isdigit():
            self.reply(400, "bad request")
            return
        body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode()
        mine = [l for l in body.splitlines() if map_of(l) == m]
        kept = [l for l in read_lines() if map_of(l) not in (None, m)]
        header = ("# map extensions: map layer row col entry(hex); overrides: "
                  "ov map layer row col entry orig(hex) -- tools/map_editor\n")
        tmp = CFG + ".tmp"
        with open(tmp, "w") as f:
            f.write(header + "".join(l + "\n" for l in kept + mine))
        os.replace(tmp, CFG)
        n_ov = sum(1 for l in mine if l.startswith("ov "))
        msg = f"saved {len(mine) - n_ov} cells, {n_ov} overrides"
        if q.get("thor", ["0"])[0] == "1":
            with open(CFG, "rb") as f:
                r = subprocess.run(["adb", "shell", f"run-as {PACKAGE} sh -c 'cat > files/map_ext.cfg'"],
                                   stdin=f, capture_output=True, timeout=20)
            msg += ", sent to the Thor" if r.returncode == 0 else \
                f", Thor copy FAILED: {(r.stderr or r.stdout).decode().strip()[:200]}"
        sys.stderr.write(f"map {m}: {msg} -> {CFG}\n")
        self.reply(200, msg)


if __name__ == "__main__":
    print(f"map editor on http://localhost:{PORT}  (writes {CFG})")
    http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
