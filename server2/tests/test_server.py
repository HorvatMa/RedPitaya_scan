"""End-to-end test of build/rpsipm_server_fake with the viewer's protocol client.

    make && ~/.venv/base/bin/python tests/test_server.py
"""

import os
import socket
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "client", "viewer"))
from rpsipm.protocol import (T_CMD, T_STATUS, Client, CommandError, pack_msg,  # noqa: E402
                             parse_kv, recv_msg)

SERVER = os.path.join(HERE, "..", "build", "rpsipm_server_fake")
PORT = 5091


def wait_port(port, timeout=5.0):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def expect_err(c, cmd, code):
    try:
        c.command(cmd)
    except CommandError as exc:
        assert exc.code == code, f"{cmd!r}: code {exc.code}, expected {code}"
        return
    raise AssertionError(f"{cmd!r} unexpectedly succeeded")


def test_commands():
    c = Client("127.0.0.1", PORT)
    assert c.command("ping") == "pong"
    cfg = c.get_config()
    assert cfg["img.dtype"] == "float32"
    for k in ("slow.freq_actual", "fast.freq_actual", "img.width_actual", "img.height_actual", "acq.rate"):
        assert k in cfg, k
    assert float(cfg["fast.freq_actual"]) == 1000.0
    expect_err(c, "jump", 1)
    expect_err(c, "set nope 1", 2)
    expect_err(c, "set acq.dec 1", 2)                 # integrator needs >= 2
    expect_err(c, "set img.bidir sideways", 2)
    expect_err(c, "set img.roi 0.5 0.2 0 1", 2)
    c.set("slow.amp", "50")                            # accepted, but exceeds the DAC range ...
    expect_err(c, "start", 2)                          # ... which start reports
    c.set("slow.amp", "0.1217")
    c.close()
    print("commands ok")


def test_scan():
    frames, status = [], []
    c = Client("127.0.0.1", PORT, on_frame=frames.append, on_status=status.append)
    c.set("slow.freq", "10")
    cfg = c.get_config()
    w, h = int(cfg["img.width_actual"]), int(cfg["img.height_actual"])
    c.command("start")
    expect_err(c, "start", 3)
    expect_err(c, "set slow.freq 3", 3)
    time.sleep(2.5)

    assert len(frames) >= 15, f"only {len(frames)} frames in 2.5 s at 10 Hz"
    ids = [f.frame_id for f in frames]
    assert ids == list(range(ids[0], ids[0] + len(ids))), f"gaps in frame ids: {ids}"
    assert ids[0] == 0
    f = frames[-1]
    assert (f.width, f.height, f.n_images) == (w, h, 1), (f.width, f.height, w, h)
    assert np.allclose(f.roi, (0, 1, 0.02, 0.98), atol=1e-6)
    assert abs(f.slow_freq - 10.0) < 1e-3 and f.fast_freq == 1000.0
    finite = np.isfinite(f.data)
    assert finite.mean() > 0.95, f"too many empty pixels: {1 - finite.mean():.2%}"
    assert 1000 < np.nanmean(f.data) < 16000 and np.nanstd(f.data) > 300, (np.nanmean(f.data), np.nanstd(f.data))
    st = status[-1]
    assert st["state"] == "running" and st["hw"] == "fake"
    assert abs(float(st["rate_rec_s"]) - 2.5e6) < 0.25e6, st["rate_rec_s"]
    assert int(st["chunks_lost"]) == 0

    # live window change: applied from the next frame, auto size follows
    c.set("img.roi", "0.25 0.75 0.3 0.7")
    n0 = len(frames)
    time.sleep(1.0)
    new = [fr for fr in frames[n0:] if np.allclose(fr.roi, (0.25, 0.75, 0.3, 0.7), atol=1e-6)]
    assert new, "no frame with the new window"
    assert new[-1].height == int(h * 0.5) or abs(new[-1].height - h * 0.5) <= 1, (new[-1].height, h)
    ids = [fr.frame_id for fr in frames]
    assert ids == list(range(len(ids))), "window change must not break frame numbering"

    # live pixel-format change: same images, fewer bytes
    for name, code, bpp in (("uint16", 1, 2), ("uint8", 2, 1)):
        c.set("img.dtype", name)
        n0 = len(frames)
        time.sleep(0.6)
        enc = [fr for fr in frames[n0:] if fr.dtype == code]
        assert enc, f"no {name} frame"
        fr = enc[-1]
        assert fr.nbytes == 72 + fr.width * fr.height * bpp, (name, fr.nbytes)
        step = (fr.value_hi - fr.value_lo) / ((65535, 255)[code - 1] - 1)
        ref = frames[n0 - 1].data
        assert abs(np.nanmean(fr.data) - np.nanmean(ref)) < 0.05 * np.nanmean(ref) + step, name
        assert np.isnan(fr.data).mean() <= 0.05
    c.set("img.dtype", "float32")
    print("live pixel format ok: float32 -> uint16 -> uint8")

    c.command("stop")
    time.sleep(0.3)
    assert parse_kv(c.command("get status"))["state"] == "idle"
    np.save(os.path.join(HERE, "..", "build", "last_frame.npy"), frames[n0 - 1].data[0])
    c.set("img.roi", "0 1 0.02 0.98")
    c.set("slow.freq", "3")
    c.close()
    print(f"scan ok: {len(frames)} frames, {w}x{h}, then {new[-1].width}x{new[-1].height}")


def test_disconnect_stops():
    c = Client("127.0.0.1", PORT)
    c.command("start")
    time.sleep(0.5)
    c.close()                                          # no stop: the server must stop by itself
    time.sleep(0.5)
    c = Client("127.0.0.1", PORT)
    assert parse_kv(c.command("get status"))["state"] == "idle"
    c.close()
    print("disconnect stops scan ok")


def test_slow_client_drops():
    """A client that does not read: the server must drop frames instead of queueing them."""
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.connect(("127.0.0.1", PORT))
    for cmd in ("set slow.freq 20", "set img.width 600", "set img.height 400", "start"):
        s.sendall(pack_msg(T_CMD, cmd.encode()))
    time.sleep(4.0)                                    # read nothing; frames are 1 MB each
    st, t0 = None, time.monotonic()
    while time.monotonic() - t0 < 10:
        mtype, payload = recv_msg(s)
        if mtype == T_STATUS:
            st = parse_kv(payload.decode())
            if int(st["frames_dropped"]) > 0:
                break
    s.sendall(pack_msg(T_CMD, b"stop"))
    s.close()
    assert st and int(st["frames_dropped"]) > 0, st
    print(f"slow client: made {st['frames_made']}, sent {st['frames_sent']}, dropped {st['frames_dropped']}")
    c = Client("127.0.0.1", PORT)                      # restore defaults for the next test run
    for k, v in (("slow.freq", "3"), ("img.width", "auto"), ("img.height", "auto")):
        c.set(k, v)
    c.close()


if __name__ == "__main__":
    srv = subprocess.Popen([SERVER, "-p", str(PORT)], stdout=subprocess.DEVNULL)
    try:
        wait_port(PORT)
        test_commands()
        test_scan()
        test_disconnect_stops()
        test_slow_client_drops()
    finally:
        srv.terminate()
        rc = srv.wait(5)
    assert rc == 0, f"server exit code {rc}"
    print("all server tests passed (clean shutdown)")
