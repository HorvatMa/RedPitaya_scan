"""Smoke tests: protocol round trip, mock server session, headless viewer.

    QT_QPA_PLATFORM=offscreen ~/.venv/base/bin/python test_viewer.py [screenshot.png]
"""

import os
import socket
import subprocess
import sys
import tempfile
import time

import numpy as np

from rpsipm.protocol import (FRAME_HDR, Client, CommandError, pack_frame, parse_frame)

PORT = 5077


def test_frame_roundtrip():
    data = np.arange(2 * 3 * 4, dtype=np.float32).reshape(2, 3, 4)
    data[0, 1, 2] = np.nan
    msg = pack_frame(42, data, flags=1, n_records=1000, slow_freq=3.0, fast_freq=1000.0,
                     roi=(0.25, 0.75, 0.0, 0.5))
    f = parse_frame(msg[12:])
    assert len(msg) == 12 + FRAME_HDR.size + data.nbytes
    assert (f.frame_id, f.width, f.height, f.n_images, f.flags, f.n_empty) == (42, 4, 3, 2, 1, 1)
    assert f.roi == (0.25, 0.75, 0.0, 0.5)
    assert np.array_equal(np.nan_to_num(f.data, nan=-1), np.nan_to_num(data, nan=-1))
    print("frame round trip ok")


def wait_port(port, timeout=5.0):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("mock server did not start")


def test_session():
    frames, status = [], []
    c = Client("127.0.0.1", PORT, on_frame=frames.append, on_status=status.append)
    assert c.command("ping") == "pong"
    c.set("slow.freq", "10")
    try:
        c.set("img.bidir", "sideways")
        raise AssertionError("bad value accepted")
    except CommandError as exc:
        assert exc.code == 2
    cfg = c.get_config()
    assert cfg["slow.freq"] == "10" and int(cfg["img.width_actual"]) > 0
    c.command("start")
    time.sleep(1.5)
    try:
        c.set("slow.freq", "3")
        raise AssertionError("set while running accepted")
    except CommandError as exc:
        assert exc.code == 3
    c.set("img.roi", "0.25 0.75 0.1 0.6")            # live key: allowed while running
    assert c.get_config()["img.roi"] == "0.25 0.75 0.1 0.6"
    for bad in ("0.5 0.4 0 1", "0 1 0 1.5", "0 1 0"):
        try:
            c.set("img.roi", bad)
            raise AssertionError(f"bad roi {bad!r} accepted")
        except CommandError as exc:
            assert exc.code == 2
    c.command("stop")
    c.close()
    assert len(frames) >= 5, f"only {len(frames)} frames"
    assert frames[0].data.shape == (1, int(cfg["img.height_actual"]), int(cfg["img.width_actual"]))
    assert status and status[-1]["state"] in ("running", "idle")
    print(f"session ok: {len(frames)} frames, last {frames[-1].width}x{frames[-1].height}")


def test_gui(screenshot):
    from PyQt5 import QtCore, QtWidgets
    from viewer import Viewer

    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication(sys.argv)
    win = Viewer("127.0.0.1", PORT)
    win.show()
    win.toggle_connection()
    assert win.client is not None, "GUI did not connect"
    win.cfg_widgets["img.bidir"][1].setCurrentText("both")
    win.apply_config()
    win.img_sel.setCurrentIndex(2)
    win.cmap_box.setCurrentText("viridis")
    win.start_scan()
    t0 = time.monotonic()
    while time.monotonic() - t0 < 2.0:
        app.processEvents(QtCore.QEventLoop.AllEvents, 50)
        time.sleep(0.02)
    assert win.frame is not None and win.frame.n_images == 2, "no 2-image frame displayed"
    def pump(sec):
        t = time.monotonic()
        while time.monotonic() - t < sec:
            app.processEvents(QtCore.QEventLoop.AllEvents, 50)
            time.sleep(0.02)

    # crop: select the centre quarter of image 0 (single-image view)
    win.img_sel.setCurrentIndex(0)
    pump(0.6)
    f = win.frame
    win.view.sel_item.setRect(QtCore.QRectF(f.width / 4, f.height / 4, f.width / 2, f.height / 2))
    win.view.sel_item.show()
    win.crop_to_selection()
    pump(1.0)                                  # wait for a frame binned with the new window
    assert np.allclose(win.frame.roi, win.roi, atol=1e-6), (win.frame.roi, win.roi)
    x0, x1, y0, y1 = win.roi
    assert abs(x0 - 0.25) < 0.01 and abs(x1 - 0.75) < 0.01, win.roi
    assert abs(y0 - (0.02 + 0.24)) < 0.01 and abs(y1 - (0.02 + 0.72)) < 0.01, win.roi

    # zoom scan to a selection inside the cropped window, then back out
    sa, so, fa, fo = win._scan()
    pump(0.6)
    f = win.frame
    win.view.sel_item.setRect(QtCore.QRectF(0, 0, f.width / 2, f.height / 2))
    win.view.sel_item.show()
    win.zoom_to_selection()
    nsa, nso, nfa, nfo = win._scan()
    assert abs(nsa - sa * 0.25) < 1e-3 * sa, (sa, nsa)               # slow half of a half
    assert win.roi == (0.0, 1.0, 0.02, 0.98)
    pump(1.0)
    assert win.server_state == "running", "zoom must restart the scan"
    win.zoom_out()
    assert abs(win._scan()[0] - sa) < 1e-9 and abs(win.roi[0] - 0.25) < 1e-6
    win.full_field()
    assert win.roi == (0.0, 1.0, 0.02, 0.98)
    pump(1.0)
    win.img_sel.setCurrentIndex(2)
    pump(0.5)
    print("roi crop/zoom ok")

    if screenshot:
        win.grab().save(screenshot)
        print(f"screenshot -> {screenshot}")
    print("gui ok:", win.status_frame.text())
    win.close()


def test_drops(tmpdir):
    """Throttled link: the server drops frames, the viewer must notice, log and record the rest."""
    from PyQt5 import QtCore, QtWidgets
    from viewer import Viewer

    port = PORT + 1
    srv = subprocess.Popen([sys.executable, "mock_server.py", "--port", str(port), "--throttle", "0.5"],
                           stdout=subprocess.DEVNULL)
    try:
        wait_port(port)
        app = QtWidgets.QApplication.instance() or QtWidgets.QApplication(sys.argv)
        win = Viewer("127.0.0.1", port)
        win.toggle_connection()
        win.cfg_widgets["slow.freq"][1].setValue(10.0)        # 10 frames/s, ~150 kB each
        win.apply_config()
        win.recorder.start(tmpdir, win.server_cfg)
        win.start_scan()
        t0 = time.monotonic()
        while time.monotonic() - t0 < 5.0:
            app.processEvents(QtCore.QEventLoop.AllEvents, 50)
            time.sleep(0.02)
        win.stop_scan()
        t0 = time.monotonic()
        while time.monotonic() - t0 < 1.5:                      # last STATUS + recorder flush
            app.processEvents(QtCore.QEventLoop.AllEvents, 50)
            time.sleep(0.02)
        win.report_drops()
        st, _ = win.bridge.snapshot()
        log = win.log.toPlainText()
        saved = len([n for n in os.listdir(tmpdir) if n.endswith(".npz")])
        print(f"  received {st['received']}, link-dropped {st['link']}, server says "
              f"{win.server_dropped}, recorded {saved}")
        assert st["link"] > 0, "throttled link should drop frames"
        assert "DROPPED" in log, "drops not logged"
        assert win.server_dropped is not None and abs(win.server_dropped - st["link"]) <= 1, \
            "client gap count disagrees with the server counter"
        assert saved == st["received"], "recorder must save every received frame"
        win.close()
    finally:
        srv.terminate()
    print("drop accounting ok")


if __name__ == "__main__":
    test_frame_roundtrip()
    srv = subprocess.Popen([sys.executable, "mock_server.py", "--port", str(PORT)],
                           stdout=subprocess.DEVNULL)
    try:
        wait_port(PORT)
        test_session()
        time.sleep(0.3)
        test_gui(sys.argv[1] if len(sys.argv) > 1 else None)
    finally:
        srv.terminate()
    with tempfile.TemporaryDirectory() as d:
        test_drops(d)
    print("all viewer tests passed")
