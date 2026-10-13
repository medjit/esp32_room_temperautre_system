#!/usr/bin/env python3
"""
mock_server.py - serves web/index.html with FAKE data, to design the UI without a device.

It speaks the same API as the firmware (see temperature_server_src/web.h), including
the PIN check, so every screen and flow of the page can be tried on a PC.

Usage:
    python3 tools/mock_server.py [port]            # default port 8080
    open http://localhost:8080/

Scenarios (switch by opening http://localhost:8080/__scenario/<name>):
    normal    4 healthy nodes with DS18B20 sensors
    mixed     low battery, empty battery, charging, estimated temperature
    stuck     one node silent for hours, one still waiting for its first packet
    firstrun  no nodes, access-point mode, no Wi-Fi credentials, default PIN
    notime    like "normal" but without a wall clock (virtual time stamps)
    unknown   2 nodes + unknown node ids heard + default PIN

The PIN is 1234 (the firmware default).
"""

import json
import math
import random
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent.parent
PAGE = ROOT / "web" / "index.html"

VIRTUAL_BASE = 100_000_000
HIST_POINTS = 768
MAX_API_POINTS = 300
BOOT = time.time()

STATE = {
    "scenario": "normal",
    "pin": "1234",
    "room": "Living room",
    "nodes": [],
    "wifi_ssid": "HomeNet",
    "has_credentials": True,
    "mode": "sta",
    "unknown_ids": [],
    "fails": 0,
    "lock_until": 0.0,
}


# ---------------------------------------------------------------------------
# Fake data
# ---------------------------------------------------------------------------

def make_history(base_temp, amp, step_s, est=False, offset=0, silent_s=0, bat_start=3950, bat_drop=300):
    """One node's history: a daily temperature wave plus noise, newest sample `silent_s` ago."""
    now = time.time()
    end = now - silent_s
    pts = []
    for i in range(HIST_POINTS):
        t = end - (HIST_POINTS - 1 - i) * step_s
        day = math.sin((t % 86400) / 86400 * 2 * math.pi - 1.9)
        room = base_temp + amp * day + random.uniform(-0.12, 0.12)
        cpu = room + 6.5 + random.uniform(-0.2, 0.2)
        if i % 211 == 100:           # a missed cycle now and then -> gap in the graph
            continue
        bat = int(bat_start - bat_drop * i / HIST_POINTS + random.uniform(-6, 6))
        pts.append({"t": int(t), "v": int(round((cpu - offset if est else room) * 100)),
                    "s": 2 if est else 1, "c": int(round(cpu * 100)), "b": bat})
    return pts


def node(nid, name, corner, base, amp, **kw):
    """Build one fake node (settings + latest state + history)."""
    status = kw.pop("status", "ok")
    est = kw.pop("est", False)
    silent = kw.pop("silent_s", 0)
    sleep_s = kw.pop("sleep_s", 900)
    bat_state = kw.pop("bat_state", 0)
    bat_mv = kw.pop("bat_mv", 3920)
    charging = kw.pop("charging", False)
    flags = kw.pop("flags", 0)
    offset = kw.pop("offset", 650)
    waiting = kw.pop("waiting", False)

    hist = [] if waiting else make_history(base, amp, sleep_s, est=est, offset=offset / 100.0,
                                           silent_s=silent, bat_start=bat_mv + 120)
    last = hist[-1] if hist else None
    return {
        "id": nid, "name": name, "corner": corner, "status": status, "cpu_offset_cC": offset,
        "sleep_s": sleep_s, "stuck_after_s": int(sleep_s * 1.5), "rx": 0 if waiting else 480,
        "dups": 0 if waiting else 960, "crc_err": 0, "has_data": not waiting,
        "silent_s": silent, "est": est, "bat_state": bat_state, "bat_mv": bat_mv,
        "charging": charging, "flags": flags, "hist": hist, "last": last,
    }


def load_scenario(name):
    S = STATE
    S["scenario"] = name
    S["mode"], S["has_credentials"], S["unknown_ids"] = "sta", True, []
    S["pin"] = "1234" if name in ("firstrun", "unknown") else "4321"
    S["fails"], S["lock_until"] = 0, 0.0
    random.seed(7)

    if name == "firstrun":
        S["nodes"] = []
        S["mode"], S["has_credentials"] = "ap", False
    elif name == "mixed":
        S["nodes"] = [
            node(1, "Window", 0, 21.4, 1.6),
            node(2, "Bookshelf", 1, 20.8, 1.2, status="low", bat_state=1, bat_mv=3210, sleep_s=3600),
            node(3, "Sofa", 2, 22.1, 1.0, est=True, charging=True, bat_mv=4120, flags=0x01),
            node(4, "Door", 3, 19.9, 1.8, status="empty", bat_state=2, bat_mv=2980, sleep_s=18000, flags=0x10),
        ]
    elif name == "stuck":
        S["nodes"] = [
            node(1, "Window", 0, 21.4, 1.6),
            node(2, "Bookshelf", 1, 20.8, 1.2, status="stuck", silent_s=3 * 3600),
            node(3, "Sofa", 2, 22.1, 1.0),
            node(4, "Door", 3, 19.9, 1.8, status="waiting", waiting=True),
        ]
    elif name == "unknown":
        S["nodes"] = [node(1, "Window", 0, 21.4, 1.6), node(2, "Sofa", 3, 22.1, 1.0)]
        S["unknown_ids"] = [7, 9]
    else:  # normal, notime
        S["nodes"] = [
            node(1, "Window", 0, 21.4, 1.6),
            node(2, "Bookshelf", 1, 20.8, 1.2),
            node(3, "Sofa", 2, 22.6, 1.0),
            node(4, "Door", 3, 19.4, 1.8),
        ]


def time_valid():
    return STATE["scenario"] != "notime"


def to_stamp(real_t):
    """Real epoch, or virtual-clock seconds when the scenario has no wall clock."""
    return int(real_t) if time_valid() else VIRTUAL_BASE + int(real_t - BOOT) + 20000


def node_state(n, now):
    """The /api/state view of a node."""
    out = {k: n[k] for k in ("id", "name", "corner", "status", "cpu_offset_cC", "sleep_s",
                              "stuck_after_s", "rx", "dups", "crc_err", "has_data")}
    if n["has_data"]:
        last = n["last"]
        age = n["silent_s"] + int((now - BOOT) % 600)
        out.update({
            "age_s": age,
            "temp_cC": last["v"], "src": "est" if n["est"] else "ds",
            "cpu_cC": last["c"], "bat_mv": n["bat_mv"], "bat_state": n["bat_state"],
            "bat_pct": max(0, min(100, int((n["bat_mv"] - 3300) / 9))),
            "charging": n["charging"], "vbus_mv": 4980 if n["charging"] else 0,
            "rssi": -48 - n["id"] * 6, "flags": n["flags"], "node_fw": 1, "boot": 480,
        })
    return out


def state_json():
    now = time.time()
    S = STATE
    sta = S["mode"] == "sta"
    return {
        "server": {
            "fw": 1, "uptime_s": int(now - BOOT) + 86400 * 3, "heap_free": 151000, "heap_min": 128400,
            "time_valid": time_valid(), "epoch": int(now) if time_valid() else 0,
            "pin_default": S["pin"] == "1234", "max_nodes": 4, "hostname": "roomtemp.local",
            "wifi": {"mode": S["mode"], "connected": sta and S["has_credentials"],
                     "has_credentials": S["has_credentials"],
                     "ssid": S["wifi_ssid"] if sta else "", "ip": "192.168.1.20" if sta else "192.168.4.1",
                     "channel": 6 if sta else 1, "rssi": -58 if sta else 0, "ap_ssid": "RoomTemp-Setup"},
            "stats": {"frames": 5840, "accepted": 1940, "duplicates": 3880, "bad_len": 0,
                      "bad_ver": 0, "bad_crc": 3, "unknown": len(S["unknown_ids"]) * 3, "overflow": 0},
            "unknown_ids": S["unknown_ids"],
        },
        "room": S["room"],
        "ui": {"tmin": 15, "tmax": 30, "poll_s": 5},
        "nodes": [node_state(n, now) for n in S["nodes"]],
    }


def history_json(n, frm):
    """Same shape and downsampling as the firmware's /api/history."""
    pts = [p for p in n["hist"] if to_stamp(p["t"]) >= frm]
    step = max(1, math.ceil(len(pts) / MAX_API_POINTS))
    merged = []
    for i in range(0, len(pts), step):
        grp = pts[i:i + step]
        vs = [p["v"] for p in grp if p["v"] is not None]
        merged.append({
            "t": to_stamp(sum(p["t"] for p in grp) / len(grp)),
            "v": round(sum(vs) / len(vs)) if vs else None,
            "s": grp[-1]["s"], "c": round(sum(p["c"] for p in grp) / len(grp)),
            "b": round(sum(p["b"] for p in grp) / len(grp)),
        })
    return {
        "node": n["id"], "step": step, "sleep_s": n["sleep_s"], "time_valid": time_valid(),
        "now": to_stamp(time.time()),
        "t": [m["t"] for m in merged], "v": [m["v"] for m in merged], "s": [m["s"] for m in merged],
        "c": [m["c"] for m in merged], "b": [m["b"] for m in merged],
    }


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("mock: " + fmt % args + "\n")

    # ---- helpers ----
    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def error(self, code, msg):
        self.send_json({"ok": False, "error": msg}, code)

    def pin_ok(self):
        """Same lockout rules as the firmware: 5 wrong PINs -> 429 for 60 s."""
        S = STATE
        if S["lock_until"] and time.time() < S["lock_until"]:
            self.error(429, "too many wrong PINs, try again later")
            return False
        if self.headers.get("X-Pin", "") == S["pin"]:
            S["fails"] = 0
            return True
        S["fails"] += 1
        if S["fails"] >= 5:
            S["lock_until"] = time.time() + 60
        self.error(401, "wrong PIN")
        return False

    def form(self):
        n = int(self.headers.get("Content-Length") or 0)
        return {k: v[0] for k, v in parse_qs(self.rfile.read(n).decode()).items()}

    def find(self, nid):
        return next((n for n in STATE["nodes"] if n["id"] == nid), None)

    # ---- GET ----
    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}

        if u.path.startswith("/__scenario/"):
            load_scenario(u.path.rsplit("/", 1)[1])
            self.send_response(302)
            self.send_header("Location", "/")
            self.end_headers()
        elif u.path == "/":
            body = PAGE.read_bytes()                     # re-read each time: edit + refresh
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif u.path == "/api/state":
            self.send_json(state_json())
        elif u.path == "/api/history":
            n = self.find(int(q.get("node", -1)))
            if not n:
                return self.error(404, "unknown node")
            self.send_json(history_json(n, int(q.get("from", 0))))
        elif u.path == "/api/export.csv":
            n = self.find(int(q.get("node", -1)))
            if not n:
                return self.error(404, "unknown node")
            rows = ["ts,utc,room_C,source,cpu_C,battery_mV,battery_state,charging"]
            for p in n["hist"]:
                rows.append(f'{p["t"]},,{p["v"] / 100:.2f},{p["s"]},{p["c"] / 100:.2f},{p["b"]},0,0')
            body = "\n".join(rows).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/csv")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif u.path == "/api/wifi/scan":
            if not self.pin_ok():
                return
            time.sleep(0.4)
            self.send_json({"scanning": False, "networks": [
                {"ssid": "HomeNet", "rssi": -48, "channel": 6, "secure": True},
                {"ssid": "Neighbour-5G", "rssi": -71, "channel": 11, "secure": True},
                {"ssid": "CoffeeShop", "rssi": -80, "channel": 1, "secure": False},
            ]})
        else:
            self.error(404, "not found")

    # ---- POST ----
    def do_POST(self):
        u = urlparse(self.path)
        if not self.pin_ok():
            return
        f = self.form()
        S = STATE

        if u.path == "/api/auth":
            return self.send_json({"ok": True})
        if u.path == "/api/wifi":
            S["wifi_ssid"], S["has_credentials"], S["mode"] = f.get("ssid", ""), True, "sta"
            return self.send_json({"ok": True})
        if u.path == "/api/room":
            S["room"] = f.get("name", S["room"])
            return self.send_json({"ok": True})
        if u.path == "/api/pin":
            S["pin"] = f.get("new", S["pin"])
            return self.send_json({"ok": True})
        if u.path in ("/api/restart", "/api/history/clear"):
            return self.send_json({"ok": True})
        if u.path == "/api/factory-reset":
            if f.get("confirm") != "RESET":
                return self.error(400, "confirmation missing")
            load_scenario("firstrun")
            return self.send_json({"ok": True})

        n = self.find(int(f.get("id", -1)))
        if not n:
            return self.error(404, "unknown node")
        if u.path == "/api/node/delete":
            S["nodes"].remove(n)
            return self.send_json({"ok": True})
        if u.path == "/api/node":
            if "name" in f:
                n["name"] = f["name"][:20]
            if "offset" in f:
                n["cpu_offset_cC"] = int(f["offset"])
            if "corner" in f:
                c = int(f["corner"])
                for o in S["nodes"]:
                    if o is not n and o["corner"] == c:
                        o["corner"] = n["corner"]
                n["corner"] = c
            return self.send_json({"ok": True})
        self.error(404, "not found")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    load_scenario("normal")
    print(f"mock server on http://localhost:{port}/   (scenarios: /__scenario/<name>)")
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
