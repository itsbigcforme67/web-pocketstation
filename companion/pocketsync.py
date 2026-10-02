#!/usr/bin/env python3
# PocketSync - Copyright (C) 2026 the Web PocketStation authors.
# Free software under the GNU General Public License, version 3 or later; see LICENSE. No warranty.
"""
PocketSync - DuckStation <-> Web PocketStation companion (Linux, Python 3.8+, no dependencies).

* Watches DuckStation's memory card folder (and any extra folders you add).
* Keeps a history of every version of every card (so nothing is ever lost).
* Serves the Web PocketStation app + a small JSON API to your phone over Wi-Fi/Tailscale.
* Lets the phone send changed cards back (with conflict detection and automatic backups).

Run:  python3 pocketsync.py            (then open the printed address on your phone)
Help: python3 pocketsync.py --help
"""
import argparse
import hashlib
import http.server
import json
import os
import queue
import re
import shutil
import socket
import socketserver
import subprocess
import sys
import threading
import time
import urllib.parse
from pathlib import Path

VERSION = "1.0.0"
CARD_SIZE = 0x20000
# container formats: extension-independent detection by size + magic
HEADERS = [
    (0, b"MC"),            # raw .mcd/.mcr/.ps/.mc
    (3904, b"123-456-STD"),  # .gme (InterAct DexDrive)
    (64, b"VgsM"),         # .mem/.vgs (Connectix VGS)
]
CARD_EXTS = {".mcd", ".mcr", ".gme", ".mem", ".vgs", ".mc", ".ps", ".ddf", ".psm", ".mci", ".bin", ".srm"}
HOME = Path.home()
DEFAULT_DATA = Path(os.environ.get("XDG_DATA_HOME", HOME / ".local/share")) / "pocketsync"
DUCKSTATION_DIRS = [
    Path(os.environ.get("XDG_DATA_HOME", HOME / ".local/share")) / "duckstation/memcards",
    HOME / ".var/app/org.duckstation.DuckStation/data/duckstation/memcards",   # Flatpak
    HOME / ".config/duckstation/memcards",                                     # older builds
]


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def card_header_len(data: bytes):
    """Return the container header length if `data` is a memory card image, else None."""
    for hlen, magic in HEADERS:
        if len(data) == CARD_SIZE + hlen:
            if hlen == 0 or data[: len(magic)] == magic:
                return hlen
    return None


def sha1(b: bytes) -> str:
    return hashlib.sha1(b).hexdigest()


def slug(s: str) -> str:
    s = re.sub(r"[^A-Za-z0-9._-]+", "-", s).strip("-")
    return s[:80] or "card"


class Card:
    def __init__(self, cid, path: Path, source):
        self.id = cid
        self.path = path
        self.source = source
        self.mtime = 0.0
        self.size = 0
        self.sha1 = ""
        self.header_len = 0

    def info(self):
        return {"id": self.id, "name": self.path.stem, "file": str(self.path), "source": self.source,
                "mtime": self.mtime, "sha1": self.sha1}

    def read_raw(self) -> bytes:
        data = self.path.read_bytes()
        hlen = card_header_len(data)
        if hlen is None:
            raise ValueError("not a memory card image")
        self.header_len = hlen
        return data[hlen:]


class Library:
    def __init__(self, folders, data_dir: Path, keep_history=40):
        self.folders = folders            # list of (Path, label)
        self.data_dir = data_dir
        self.history_dir = data_dir / "history"
        self.keep = keep_history
        self.cards = {}                   # id -> Card
        self.lock = threading.RLock()
        self.listeners = set()
        self.history_dir.mkdir(parents=True, exist_ok=True)

    # ---------- discovery ----------
    def scan(self):
        changed = []
        seen = set()
        with self.lock:
            for folder, label in self.folders:
                if not folder.is_dir():
                    continue
                for p in sorted(folder.iterdir()):
                    if not p.is_file() or p.suffix.lower() not in CARD_EXTS:
                        continue
                    try:
                        st = p.stat()
                    except OSError:
                        continue
                    if st.st_size - CARD_SIZE not in (0, 3904, 64):
                        continue
                    cid = slug(f"{label}-{p.stem}")
                    # keep ids unique if two folders contain the same name
                    base, n = cid, 2
                    while cid in seen:
                        cid, n = f"{base}-{n}", n + 1
                    seen.add(cid)
                    card = self.cards.get(cid)
                    if card is None or card.path != p:
                        card = Card(cid, p, label)
                        self.cards[cid] = card
                    if card.mtime != st.st_mtime or card.size != st.st_size:
                        try:
                            raw = card.read_raw()
                        except (OSError, ValueError):
                            continue
                        new_hash = sha1(raw)
                        card.mtime, card.size = st.st_mtime, st.st_size
                        if new_hash != card.sha1:
                            first = card.sha1 == ""
                            card.sha1 = new_hash
                            self.snapshot(card, raw)
                            if not first:
                                log(f"card changed: {p.name}")
                            changed.append(cid)
            for cid in list(self.cards):
                if cid not in seen:
                    del self.cards[cid]
                    changed.append(cid)
        return changed

    def snapshot(self, card: Card, raw: bytes, tag="pc"):
        d = self.history_dir / card.id
        d.mkdir(parents=True, exist_ok=True)
        # don't store duplicates of the newest snapshot
        existing = sorted(d.glob("*.mcd"))
        h = sha1(raw)
        if existing and sha1(existing[-1].read_bytes()) == h:
            return
        name = time.strftime("%Y%m%d-%H%M%S") + f"-{tag}.mcd"
        (d / name).write_bytes(raw)
        for old in existing[: max(0, len(existing) + 1 - self.keep)]:
            try:
                old.unlink()
            except OSError:
                pass

    def history(self, cid):
        d = self.history_dir / cid
        if not d.is_dir():
            return []
        return [{"name": f.name, "size": f.stat().st_size, "mtime": f.stat().st_mtime} for f in sorted(d.glob("*.mcd"), reverse=True)]

    def write_back(self, cid, raw: bytes, base_sha1: str, force=False):
        with self.lock:
            card = self.cards.get(cid)
            if card is None:
                return 404, {"error": "unknown card"}
            current = card.read_raw()
            cur_hash = sha1(current)
            if not force and base_sha1 and base_sha1 != cur_hash:
                return 409, {"error": "card changed on the PC since the phone last synced", "sha1": cur_hash}
            # backup the PC version, then write atomically, keeping any container header (e.g. .gme)
            self.snapshot(card, current, tag="before-phone")
            full = card.path.read_bytes()
            out = full[: card.header_len] + raw
            tmp = card.path.with_name("." + card.path.name + ".pocketsync.tmp")
            tmp.write_bytes(out)
            try:
                shutil.copymode(card.path, tmp)
            except OSError:
                pass
            os.replace(tmp, card.path)
            st = card.path.stat()
            card.mtime, card.size, card.sha1 = st.st_mtime, st.st_size, sha1(raw)
            self.snapshot(card, raw, tag="phone")
            log(f"phone -> PC: wrote {card.path.name}")
            return 200, {"ok": True, "sha1": card.sha1}

    # ---------- change notifications (Server-Sent Events) ----------
    def notify(self, event, data):
        for q in list(self.listeners):
            try:
                q.put_nowait((event, data))
            except queue.Full:
                pass

    def watch_forever(self, interval=2.0):
        while True:
            try:
                changed = self.scan()
                if changed:
                    self.notify("cards", {"changed": changed})
            except Exception as e:  # keep watching no matter what
                log("watch error:", e)
            time.sleep(interval)


def duckstation_running():
    try:
        out = subprocess.run(["pgrep", "-i", "duckstation"], capture_output=True, text=True, timeout=2)
        return any(l.strip() and int(l) != os.getpid() for l in out.stdout.split())
    except Exception:
        return False


def local_addresses():
    addrs = set()
    try:
        out = subprocess.run(["ip", "-o", "-4", "addr", "show"], capture_output=True, text=True, timeout=2).stdout
        for m in re.finditer(r"inet (\d+\.\d+\.\d+\.\d+)", out):
            if not m.group(1).startswith("127."):
                addrs.add(m.group(1))
    except Exception:
        pass
    if not addrs:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("10.255.255.255", 1))
            addrs.add(s.getsockname()[0])
            s.close()
        except Exception:
            pass
    return sorted(addrs)


def make_handler(cfg, lib: Library):
    web_root = cfg.web_dir.resolve()
    mime = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8",
            ".svg": "image/svg+xml", ".png": "image/png", ".webmanifest": "application/manifest+json", ".json": "application/json"}

    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "PocketSync/" + VERSION
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            if cfg.verbose:
                log(self.address_string(), fmt % args)

        # ---- helpers
        def send(self, code, body=b"", ctype="application/json", extra=None):
            if isinstance(body, (dict, list)):
                body = json.dumps(body).encode()
            elif isinstance(body, str):
                body = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def authed(self, query):
            if not cfg.token:
                return True
            tok = self.headers.get("X-Token") or (query.get("token") or [""])[0]
            return tok == cfg.token

        def route(self):
            u = urllib.parse.urlsplit(self.path)
            return urllib.parse.unquote(u.path), urllib.parse.parse_qs(u.query)

        # ---- GET
        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            path, q = self.route()
            if path.startswith("/api/"):
                if not self.authed(q):
                    return self.send(401, {"error": "token required"})
                return self.api_get(path, q)
            return self.static(path)

        def static(self, path):
            if path in ("", "/"):
                path = "/index.html"
            target = (web_root / path.lstrip("/")).resolve()
            if web_root not in target.parents and target != web_root or not target.is_file():
                return self.send(404, "not found", "text/plain")
            self.send(200, target.read_bytes(), mime.get(target.suffix, "application/octet-stream"))

        def api_get(self, path, q):
            parts = [p for p in path.split("/") if p][1:]  # drop "api"
            if parts == ["status"]:
                return self.send(200, {"version": VERSION, "folders": [str(f) for f, _ in lib.folders],
                                       "bios": bool(cfg.bios and Path(cfg.bios).is_file()), "duckstationRunning": duckstation_running()})
            if parts == ["cards"]:
                with lib.lock:
                    cards = [c.info() for c in lib.cards.values() if c.sha1]
                return self.send(200, {"host": socket.gethostname(), "cards": sorted(cards, key=lambda c: c["name"].lower()),
                                       "duckstationRunning": duckstation_running()})
            if len(parts) >= 3 and parts[0] == "cards":
                card = lib.cards.get(parts[1])
                if not card:
                    return self.send(404, {"error": "unknown card"})
                if parts[2] == "data":
                    try:
                        return self.send(200, card.read_raw(), "application/octet-stream")
                    except (OSError, ValueError) as e:
                        return self.send(500, {"error": str(e)})
                if parts[2] == "history":
                    if len(parts) == 3:
                        return self.send(200, lib.history(card.id))
                    f = (lib.history_dir / card.id / parts[3]).resolve()
                    if f.parent == (lib.history_dir / card.id).resolve() and f.is_file():
                        return self.send(200, f.read_bytes(), "application/octet-stream",
                                         {"Content-Disposition": f'attachment; filename="{card.path.stem}-{f.name}"'})
                    return self.send(404, {"error": "no such snapshot"})
            if parts == ["bios"]:
                if cfg.bios and Path(cfg.bios).is_file() and Path(cfg.bios).stat().st_size == 0x4000:
                    return self.send(200, Path(cfg.bios).read_bytes(), "application/octet-stream")
                return self.send(404, {"error": "no BIOS configured on the PC"})
            if parts == ["events"]:
                return self.events()
            return self.send(404, {"error": "unknown endpoint"})

        def events(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            qq = queue.Queue(maxsize=50)
            lib.listeners.add(qq)
            try:
                self.wfile.write(b": hello\n\n")
                self.wfile.flush()
                while True:
                    try:
                        ev, data = qq.get(timeout=20)
                        self.wfile.write(f"event: {ev}\ndata: {json.dumps(data)}\n\n".encode())
                    except queue.Empty:
                        self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                lib.listeners.discard(qq)
                self.close_connection = True

        # ---- PUT (phone -> PC)
        def do_PUT(self):
            path, q = self.route()
            if not self.authed(q):
                return self.send(401, {"error": "token required"})
            parts = [p for p in path.split("/") if p][1:]
            if len(parts) == 3 and parts[0] == "cards" and parts[2] == "data":
                n = int(self.headers.get("Content-Length") or 0)
                if n != CARD_SIZE:
                    return self.send(400, {"error": "expected a 128 KiB card image"})
                raw = self.rfile.read(n)
                if raw[:2] != b"MC":
                    return self.send(400, {"error": "not a formatted memory card"})
                code, body = lib.write_back(parts[1], raw, self.headers.get("X-Base-Sha1", ""), force=q.get("force") == ["1"])
                if code == 200:
                    lib.notify("cards", {"changed": [parts[1]]})
                    body["duckstationRunning"] = duckstation_running()
                return self.send(code, body)
            return self.send(404, {"error": "unknown endpoint"})

    return Handler


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def find_bios(cfg):
    if cfg.bios:
        return cfg.bios
    for d in (DEFAULT_DATA / "bios", Path(__file__).resolve().parent / "bios"):
        if d.is_dir():
            for f in sorted(d.iterdir()):
                if f.is_file() and f.stat().st_size == 0x4000:
                    return str(f)
    return None


def main():
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="Sync DuckStation memory cards to Web PocketStation on your phone.")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--host", default="0.0.0.0", help="address to listen on (default: all interfaces)")
    ap.add_argument("--memcards", action="append", default=[], help="extra memory card folder (repeatable)")
    ap.add_argument("--data-dir", default=str(DEFAULT_DATA), help="where history and imported cards live")
    ap.add_argument("--web-dir", default=str(here.parent / "web" if (here.parent / "web").is_dir() else here / "web"))
    ap.add_argument("--bios", help="PocketStation BIOS (16 KB). Optional: the phone can load it itself")
    ap.add_argument("--token", default=os.environ.get("POCKETSYNC_TOKEN", ""), help="require this access token (recommended outside your home network)")
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between folder scans")
    ap.add_argument("--verbose", action="store_true")
    cfg = ap.parse_args()
    cfg.web_dir = Path(cfg.web_dir)
    data_dir = Path(cfg.data_dir)
    (data_dir / "cards").mkdir(parents=True, exist_ok=True)
    (data_dir / "bios").mkdir(parents=True, exist_ok=True)
    cfg.bios = find_bios(cfg)

    folders = []
    for d in DUCKSTATION_DIRS:
        if d.is_dir():
            folders.append((d, "duckstation"))
    for d in cfg.memcards:
        folders.append((Path(d).expanduser(), slug(Path(d).name)))
    folders.append((data_dir / "cards", "extra"))
    if not (cfg.web_dir / "index.html").is_file():
        sys.exit(f"web app not found in {cfg.web_dir} (use --web-dir)")

    lib = Library(folders, data_dir)
    lib.scan()
    threading.Thread(target=lib.watch_forever, args=(cfg.interval,), daemon=True).start()

    httpd = Server((cfg.host, cfg.port), make_handler(cfg, lib))
    print(f"\nPocketSync {VERSION}: watching")
    for f, label in folders:
        print(f"   {'✓' if f.is_dir() else '✗'} {f}  ({label})")
    print(f"\n{len(lib.cards)} memory card(s) found.  BIOS on PC: {cfg.bios or 'none (load it on the phone instead)'}")
    print("\nOpen one of these on your phone (same Wi-Fi, or Tailscale):")
    tok = f"/?token={urllib.parse.quote(cfg.token)}" if cfg.token else ""
    for a in local_addresses():
        print(f"   http://{a}:{cfg.port}{tok}")
    print(f"   http://localhost:{cfg.port}{tok}  (this PC)")
    if not cfg.token:
        print("\nTip: add --token SOMESECRET if this PC is on a network you don't trust.")
    print("Press Ctrl+C to stop.\n", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("bye")


if __name__ == "__main__":
    main()
