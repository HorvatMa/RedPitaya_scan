"""Stand-in for server2 that speaks protocol v1 and streams synthetic frames.

    python mock_server.py [--port 5000]

It runs on the PC, so the viewer can be developed without the Red Pitaya.
Like the real server, a sender thread always sends the newest frame and drops frames the link
cannot keep up with; --throttle MBPS simulates a slow connection.
"""

import argparse
import math
import socket
import threading
import time

import numpy as np

from rpsipm.protocol import (DTYPE_NAMES, T_CMD, T_REPLY, T_STATUS, pack_frame, pack_msg,
                             recv_msg)

FPGA_HZ = 125e6

DEFAULTS = {
    "slow.shape": "sawtooth", "slow.rise": "0.975", "slow.freq": "3", "slow.amp": "1.3",
    "slow.offset": "0",
    "fast.shape": "sine", "fast.freq": "1000", "fast.amp": "2.4", "fast.offset": "0",
    "acq.dec": "50", "acq.int_dec": "1",
    "img.width": "auto", "img.height": "auto", "img.bidir": "sum", "img.roi": "0 1 0.02 0.98",
    "img.dtype": "float32",
    "cal.delay": "0.5", "cal.fast_gain": "1", "cal.fast_offset": "0",
}
CHOICES = {
    "slow.shape": {"sawtooth", "triangle"}, "fast.shape": {"sine", "triangle"},
    "img.bidir": {"sum", "forward", "backward", "both"},
    "img.dtype": {"float32", "uint16", "uint8"},
}
LIVE_KEYS = {"img.roi", "img.dtype"}


def parse_roi(text):
    """'x0 x1 y0 y1' -> tuple of floats; raises ValueError if invalid."""
    v = [float(t) for t in text.split()]
    if len(v) != 4 or not (0 <= v[0] < v[1] <= 1 and 0 <= v[2] < v[3] <= 1):
        raise ValueError("img.roi must be 'x0 x1 y0 y1' with 0 <= x0 < x1 <= 1, 0 <= y0 < y1 <= 1")
    return tuple(v)


def image_size(cfg):
    """Same rule as recon_auto_size() in server2."""
    rate = FPGA_HZ / int(cfg["acq.dec"])
    f_slow, f_fast = float(cfg["slow.freq"]), float(cfg["fast.freq"])
    x0, x1, y0, y1 = parse_roi(cfg["img.roi"])
    w = (rate / (2 * math.pi * f_fast) * (y1 - y0) if cfg["img.width"] == "auto"
         else int(cfg["img.width"]))
    h = (f_fast * float(cfg["slow.rise"]) / f_slow * (x1 - x0) if cfg["img.height"] == "auto"
         else int(cfg["img.height"]))
    return max(1, min(1920, int(w))), max(1, min(1080, int(h)))


class Session:
    def __init__(self, conn, throttle=0.0):
        self.conn = conn
        self.throttle = throttle * 1e6          # bytes/s, 0 = unlimited
        self.slot = None                        # newest frame waiting to be sent
        self.slot_cv = threading.Condition()
        self.frames_dropped = 0
        self.cfg = dict(DEFAULTS)
        self.running = threading.Event()
        self.alive = True
        self.send_lock = threading.Lock()
        self.frames_sent = 0
        self.run_id = 0
        self.t_start = time.monotonic()

    def send(self, data):
        with self.send_lock:
            if not self.throttle:
                self.conn.sendall(data)
                return
            for i in range(0, len(data), 65536):
                chunk = data[i:i + 65536]
                self.conn.sendall(chunk)
                time.sleep(len(chunk) / self.throttle)

    def reply(self, text):
        self.send(pack_msg(T_REPLY, text.encode()))

    def handle(self, line):
        parts = line.split()
        if not parts:
            return "ERR 1 empty command"
        cmd = parts[0]
        if cmd == "ping":
            return "OK pong"
        if cmd == "set":
            parts = line.split(None, 2)
            if len(parts) != 3:
                return "ERR 2 usage: set <key> <value>"
            key, val = parts[1], parts[2].strip()
            if key not in self.cfg:
                return f"ERR 2 unknown key {key}"
            if self.running.is_set() and key not in LIVE_KEYS:
                return "ERR 3 stop first"
            if key in CHOICES and val not in CHOICES[key]:
                return f"ERR 2 {key} must be one of {sorted(CHOICES[key])}"
            if key == "img.roi":
                try:
                    val = " ".join(f"{x:g}" for x in parse_roi(val))
                except ValueError as exc:
                    return f"ERR 2 {exc}"
            elif key not in CHOICES and val != "auto":
                try:
                    float(val)
                except ValueError:
                    return f"ERR 2 {key}: not a number"
            self.cfg[key] = val
            return "OK"
        if cmd == "get" and len(parts) == 2 and parts[1] == "config":
            w, h = image_size(self.cfg)
            extra = {"img.width_actual": w, "img.height_actual": h,
                     "slow.freq_actual": self.cfg["slow.freq"], "fast.freq_actual": self.cfg["fast.freq"]}
            lines = [f"{k}={v}" for k, v in {**self.cfg, **extra}.items()]
            return "OK\n" + "\n".join(lines)
        if cmd == "get" and len(parts) == 2 and parts[1] == "status":
            return "OK\n" + self.status_text()
        if cmd == "start":
            self.frames_sent = 0
            self.frames_dropped = 0
            self.run_id += 1                     # frame ids restart at 0 for every run
            self.running.set()
            return "OK"
        if cmd == "stop":
            self.running.clear()
            return "OK"
        return f"ERR 1 unknown command {cmd}"

    def status_text(self):
        state = "running" if self.running.is_set() else "idle"
        rate = FPGA_HZ / int(self.cfg["acq.dec"]) if self.running.is_set() else 0
        return (f"state={state}\nrate_rec_s={rate:.0f}\nframes_sent={self.frames_sent}\n"
                f"frames_dropped={self.frames_dropped}\nchunks_lost=0\nring_fill=0.01\ncpu=0.0\nmock=1")

    def frame_loop(self):
        fid, run = 0, -1
        while self.alive:
            if not self.running.wait(0.2):
                continue
            if run != self.run_id:
                run, fid = self.run_id, 0
            w, h = image_size(self.cfg)
            period = 1.0 / float(self.cfg["slow.freq"])
            t = time.monotonic() - self.t_start
            # sample fixed in absolute mirror position (volts), so crop/zoom behave like a real scan
            x0, x1, y0, y1 = parse_roi(self.cfg["img.roi"])
            sa, so = float(self.cfg["slow.amp"]), float(self.cfg["slow.offset"])
            fa, fo = float(self.cfg["fast.amp"]), float(self.cfg["fast.offset"])
            rows = x0 + (x1 - x0) * (np.arange(h) + 0.5) / h
            cols = y0 + (y1 - y0) * (np.arange(w) + 0.5) / w
            ys = so + sa * (2 * rows[:, None] - 1)          # slow position, V
            xs = fo + fa * (2 * cols[None, :] - 1)          # fast position, V
            img = (8000 + 3000 * np.sin(2 * np.pi * 1.2 * xs) * np.cos(2 * np.pi * 1.5 * ys)
                   + 4000 * np.exp(-((xs - 0.6 * math.cos(0.5 * t)) ** 2 + ys ** 2) / 0.02))
            img += np.random.default_rng(fid).normal(0, 150, img.shape)
            img = img.astype(np.float32)
            img[np.random.default_rng(fid + 1).random(img.shape) < 0.002] = np.nan
            data = img[None]
            if self.cfg["img.bidir"] == "both":
                data = np.stack([img, np.roll(img, 2, axis=1)])
            n_rec = int(FPGA_HZ / int(self.cfg["acq.dec"]) * period)
            t_ns = time.monotonic_ns()
            msg = pack_frame(fid, data, t_start_ns=t_ns - int(period * 1e9), t_end_ns=t_ns,
                             n_records=n_rec, n_discarded=n_rec // 40,
                             slow_freq=float(self.cfg["slow.freq"]),
                             fast_freq=float(self.cfg["fast.freq"]),
                             roi=(x0, x1, y0, y1), dtype=DTYPE_NAMES[self.cfg["img.dtype"]])
            with self.slot_cv:
                if self.slot is not None:
                    self.frames_dropped += 1      # previous frame never left: link too slow
                self.slot = msg
                self.slot_cv.notify()
            fid += 1                              # ids of dropped frames stay unused -> gaps
            time.sleep(period)

    def sender_loop(self):
        while self.alive:
            with self.slot_cv:
                while self.slot is None and self.alive:
                    self.slot_cv.wait(0.2)
                msg, self.slot = self.slot, None
            if msg is None:
                continue
            try:
                self.send(msg)
            except OSError:
                return
            self.frames_sent += 1

    def status_loop(self):
        while self.alive:
            time.sleep(1.0)
            try:
                self.send(pack_msg(T_STATUS, self.status_text().encode()))
            except OSError:
                return

    def run(self):
        threading.Thread(target=self.frame_loop, daemon=True).start()
        threading.Thread(target=self.sender_loop, daemon=True).start()
        threading.Thread(target=self.status_loop, daemon=True).start()
        try:
            while True:
                mtype, payload = recv_msg(self.conn)
                if mtype == T_CMD:
                    self.reply(self.handle(payload.decode(errors="replace")))
        except (ConnectionError, OSError):
            pass
        finally:
            self.alive = False
            self.running.clear()      # client disconnect stops the scan
            self.conn.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--throttle", type=float, default=0.0, metavar="MBPS",
                    help="limit the link to MBPS megabytes/s to provoke dropped frames")
    args = ap.parse_args()
    srv = socket.create_server((args.host, args.port), reuse_port=False)
    print(f"mock server on {args.host}:{args.port}")
    while True:
        conn, addr = srv.accept()
        print("client", addr)
        Session(conn, args.throttle).run()
        print("client gone")


if __name__ == "__main__":
    main()
