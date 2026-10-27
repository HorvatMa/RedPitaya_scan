"""Live image viewer for the RedPitaya_SiPM server2.

    ~/.venv/base/bin/python viewer.py [--host rp-f0a360.local] [--port 5000]

Test without the board: run `python mock_server.py` and connect to 127.0.0.1.
"""

import argparse
import json
import os
import queue
import sys
import threading
import time

import numpy as np
from PyQt5 import QtCore, QtGui, QtWidgets

from rpsipm.protocol import FLAG_DATA_LOST, Client, CommandError

# (key, label, kind, kwargs); kind: float | int | size | choice
CONFIG_FIELDS = [
    ("slow.freq", "Slow freq [Hz]", "float", dict(lo=0.01, hi=1000, dec=3)),
    ("slow.amp", "Slow amp [V]", "float", dict(lo=0, hi=20, dec=4)),
    ("slow.offset", "Slow offset [V]", "float", dict(lo=-20, hi=20, dec=4)),
    ("slow.rise", "Slow rise fraction", "float", dict(lo=0.5, hi=1.0, dec=3)),
    ("slow.shape", "Slow shape", "choice", dict(items=["sawtooth", "triangle"])),
    ("fast.freq", "Fast freq [Hz]", "float", dict(lo=0.1, hi=10000, dec=2)),
    ("fast.amp", "Fast amp [V]", "float", dict(lo=0, hi=20, dec=4)),
    ("fast.offset", "Fast offset [V]", "float", dict(lo=-20, hi=20, dec=4)),
    ("fast.shape", "Fast shape", "choice", dict(items=["sine", "triangle"])),
    ("acq.dec", "Acq. decimation", "int", dict(lo=1, hi=1_000_000)),
    ("img.width", "Width [px]", "size", dict(hi=1920)),
    ("img.height", "Height [px]", "size", dict(hi=1080)),
    ("img.bidir", "Bidirectional", "choice", dict(items=["sum", "forward", "backward", "both"])),
    ("img.dtype", "Pixel format", "choice", dict(items=["float32", "uint16", "uint8"])),
    ("cal.delay", "Delay [samples]", "float", dict(lo=-60, hi=60, dec=2)),
]

DEFAULT_ROI = (0.0, 1.0, 0.02, 0.98)       # x0 x1 (slow, rows), y0 y1 (fast, columns)


def parse_roi(text):
    v = tuple(float(t) for t in text.split())
    return v if len(v) == 4 else DEFAULT_ROI


def roi_text(roi):
    return " ".join(f"{v:.6g}" for v in roi)


def colormap_lut(name):
    if name == "gray":
        g = np.arange(256, dtype=np.uint8)
        return np.stack([g, g, g], axis=1)
    try:
        from matplotlib import colormaps
        return (colormaps[name](np.linspace(0, 1, 256))[:, :3] * 255).astype(np.uint8)
    except Exception:
        return colormap_lut("gray")


class Recorder:
    """Writes every received frame to disk on its own thread (not only the displayed ones)."""

    def __init__(self, maxlen=64):
        self._q = queue.Queue(maxsize=maxlen)
        self.folder = None
        self.config = {}
        self.written = 0
        self.dropped = 0               # disk too slow: queue full
        threading.Thread(target=self._run, daemon=True).start()

    def start(self, folder, config):
        self.folder, self.config = folder, dict(config)
        self.written = self.dropped = 0

    def stop(self):
        self.folder = None

    def put(self, frame):
        if self.folder is None:
            return
        try:
            self._q.put_nowait((self.folder, self.config, frame))
        except queue.Full:
            self.dropped += 1

    def _run(self):
        while True:
            folder, config, f = self._q.get()
            write_npz(os.path.join(folder, f"frame_{f.frame_id:06d}.npz"), f, config, compress=False)
            self.written += 1


def write_npz(path, f, config, compress=True):
    save = np.savez_compressed if compress else np.savez
    save(path, data=f.data, header=json.dumps(f.header_dict()), config=json.dumps(config))


class Bridge(QtCore.QObject):
    """Moves callbacks from the network thread into the GUI thread (latest frame only).

    Also keeps drop statistics. Frame ids are assigned at reconstruction on the server and
    restart at 0 every run, so a gap in the ids means the server dropped frames because the
    link (or this client) did not keep up. Frames replaced before the GUI drew them are
    counted as "display skipped"; they are still recorded.
    """
    frame = QtCore.pyqtSignal()
    status = QtCore.pyqtSignal(dict)
    error = QtCore.pyqtSignal(str)

    def __init__(self, recorder):
        super().__init__()
        self._mutex = QtCore.QMutex()
        self._latest = None
        self.recorder = recorder
        self.reset_stats()

    def reset_stats(self):
        with QtCore.QMutexLocker(self._mutex):
            self.received = 0
            self.bytes = 0
            self.link_dropped = 0          # missing frame ids
            self.display_skipped = 0
            self.data_lost = 0             # frames flagged DATA_LOST by the server
            self.gaps = []                 # recent (first, last) missing ids
            self.last_id = None

    def push_frame(self, frame):
        with QtCore.QMutexLocker(self._mutex):
            fid = frame.frame_id
            if self.last_id is not None and fid > self.last_id + 1:
                self.link_dropped += fid - self.last_id - 1
                self.gaps = (self.gaps + [(self.last_id + 1, fid - 1)])[-20:]
            self.last_id = fid             # fid < last_id: a new run started, ids restarted
            self.received += 1
            if frame.flags & FLAG_DATA_LOST:
                self.data_lost += 1
            self.bytes += frame.nbytes
            pending = self._latest is not None
            if pending:
                self.display_skipped += 1
            self._latest = frame
        self.recorder.put(frame)
        if not pending:
            self.frame.emit()

    def snapshot(self):
        with QtCore.QMutexLocker(self._mutex):
            gaps, self.gaps = self.gaps, []
            return dict(link=self.link_dropped, display=self.display_skipped,
                        data_lost=self.data_lost, received=self.received, bytes=self.bytes), gaps

    def take_frame(self):
        with QtCore.QMutexLocker(self._mutex):
            f, self._latest = self._latest, None
            return f


class ImageView(QtWidgets.QGraphicsView):
    hover = QtCore.pyqtSignal(int, int)
    selected = QtCore.pyqtSignal(QtCore.QRectF)     # scene (pixel) coordinates

    def __init__(self):
        super().__init__()
        self.setScene(QtWidgets.QGraphicsScene(self))
        self.item = QtWidgets.QGraphicsPixmapItem()
        self.item.setTransformationMode(QtCore.Qt.FastTransformation)
        self.scene().addItem(self.item)
        self.setBackgroundBrush(QtGui.QColor(40, 40, 40))
        self.setDragMode(QtWidgets.QGraphicsView.ScrollHandDrag)
        self.setTransformationAnchor(QtWidgets.QGraphicsView.AnchorUnderMouse)
        self.setMouseTracking(True)
        self._fit = True
        self._size = None
        self._selecting = False
        self._sel_start = None
        self.sel_item = QtWidgets.QGraphicsRectItem()
        pen = QtGui.QPen(QtGui.QColor(255, 80, 80), 0)
        pen.setCosmetic(True)
        pen.setWidth(2)
        self.sel_item.setPen(pen)
        self.sel_item.setZValue(1)
        self.sel_item.hide()
        self.scene().addItem(self.sel_item)

    def set_select_mode(self, on):
        self._selecting = on
        self.setDragMode(QtWidgets.QGraphicsView.NoDrag if on else QtWidgets.QGraphicsView.ScrollHandDrag)
        self.viewport().setCursor(QtCore.Qt.CrossCursor if on else QtCore.Qt.OpenHandCursor)

    def clear_selection(self):
        self.sel_item.hide()

    def selection(self):
        return self.sel_item.rect() if self.sel_item.isVisible() else None

    def mousePressEvent(self, e):
        if self._selecting and e.button() == QtCore.Qt.LeftButton:
            self._sel_start = self.mapToScene(e.pos())
            self.sel_item.setRect(QtCore.QRectF(self._sel_start, self._sel_start))
            self.sel_item.show()
            return
        super().mousePressEvent(e)

    def mouseReleaseEvent(self, e):
        if self._selecting and self._sel_start is not None:
            rect = QtCore.QRectF(self._sel_start, self.mapToScene(e.pos())).normalized()
            rect = rect.intersected(self.scene().sceneRect())
            self._sel_start = None
            if rect.width() >= 2 and rect.height() >= 2:
                self.sel_item.setRect(rect)
                self.selected.emit(rect)
            else:
                self.sel_item.hide()
            return
        super().mouseReleaseEvent(e)

    def set_image(self, qimg):
        self.item.setPixmap(QtGui.QPixmap.fromImage(qimg))
        size = (qimg.width(), qimg.height())
        if size != self._size:
            self._size = size
            self.scene().setSceneRect(0, 0, *size)
            self._fit = True
        if self._fit:
            self.fitInView(self.item, QtCore.Qt.KeepAspectRatio)

    def resizeEvent(self, e):
        super().resizeEvent(e)
        if self._fit:
            self.fitInView(self.item, QtCore.Qt.KeepAspectRatio)

    def wheelEvent(self, e):
        self._fit = False
        f = 1.25 if e.angleDelta().y() > 0 else 0.8
        self.scale(f, f)

    def mouseDoubleClickEvent(self, e):
        self._fit = True
        self.fitInView(self.item, QtCore.Qt.KeepAspectRatio)

    def mouseMoveEvent(self, e):
        super().mouseMoveEvent(e)
        p = self.mapToScene(e.pos())
        if self._selecting and self._sel_start is not None:
            self.sel_item.setRect(QtCore.QRectF(self._sel_start, p).normalized())
        self.hover.emit(int(p.x()), int(p.y()))


class Viewer(QtWidgets.QMainWindow):
    def __init__(self, host, port):
        super().__init__()
        self.setWindowTitle("RedPitaya SiPM viewer")
        self.client = None
        self.recorder = Recorder()
        self.bridge = Bridge(self.recorder)
        self.bridge.frame.connect(self.on_frame)
        self.bridge.status.connect(self.on_status)
        self.bridge.error.connect(self.on_net_error)
        self.frame = None          # last Frame
        self.shown = None          # 2-D array currently displayed
        self.lut = colormap_lut("gray")
        self.reported = {}         # drop counters already written to the log
        self._rx_last = (time.monotonic(), 0)
        self.server_dropped = None
        self.server_cfg = {}
        self.server_state = "idle"
        self.roi = DEFAULT_ROI
        self.zoom_stack = []       # previous (slow.amp, slow.offset, fast.amp, fast.offset, roi)
        self.fps_times = []

        self.view = ImageView()
        self.view.hover.connect(self.on_hover)
        self.setCentralWidget(self.view)

        self._build_connection_bar(host, port)
        self._build_config_dock()
        self._build_roi_dock()
        self._build_display_dock()
        self._build_log_dock()
        self.status_frame = QtWidgets.QLabel("no frame")
        self.status_server = QtWidgets.QLabel("not connected")
        self.status_hover = QtWidgets.QLabel("")
        sb = self.statusBar()
        sb.addWidget(self.status_frame, 1)
        sb.addWidget(self.status_hover)
        sb.addPermanentWidget(self.status_server)
        self.status_drops = QtWidgets.QLabel("")
        sb.addPermanentWidget(self.status_drops)
        self._set_connected(False)
        self.drop_timer = QtCore.QTimer(self)
        self.drop_timer.timeout.connect(self.report_drops)
        self.drop_timer.start(1000)
        self.resize(1300, 850)

    # ---------------------------------------------------------------- UI
    def _build_connection_bar(self, host, port):
        tb = self.addToolBar("connection")
        tb.setMovable(False)
        self.host_edit = QtWidgets.QLineEdit(host)
        self.host_edit.setFixedWidth(180)
        self.port_spin = QtWidgets.QSpinBox()
        self.port_spin.setRange(1, 65535)
        self.port_spin.setValue(port)
        self.btn_connect = QtWidgets.QPushButton("Connect")
        self.btn_connect.clicked.connect(self.toggle_connection)
        for w in (QtWidgets.QLabel(" Host "), self.host_edit, QtWidgets.QLabel(" Port "),
                  self.port_spin, self.btn_connect):
            tb.addWidget(w)
        tb.addSeparator()
        self.btn_start = QtWidgets.QPushButton("Start")
        self.btn_stop = QtWidgets.QPushButton("Stop")
        self.btn_start.clicked.connect(self.start_scan)
        self.btn_stop.clicked.connect(self.stop_scan)
        tb.addWidget(self.btn_start)
        tb.addWidget(self.btn_stop)
        tb.addSeparator()
        self.btn_save = QtWidgets.QPushButton("Save frame")
        self.btn_save.clicked.connect(self.save_frame)
        self.btn_record = QtWidgets.QPushButton("Record")
        self.btn_record.setCheckable(True)
        self.btn_record.toggled.connect(self.toggle_record)
        tb.addWidget(self.btn_save)
        tb.addWidget(self.btn_record)

    def _build_config_dock(self):
        w = QtWidgets.QWidget()
        form = QtWidgets.QFormLayout(w)
        self.cfg_widgets = {}
        for key, label, kind, kw in CONFIG_FIELDS:
            if kind == "float":
                ed = QtWidgets.QDoubleSpinBox()
                ed.setRange(kw["lo"], kw["hi"])
                ed.setDecimals(kw["dec"])
            elif kind == "int":
                ed = QtWidgets.QSpinBox()
                ed.setRange(kw["lo"], kw["hi"])
            elif kind == "size":
                ed = QtWidgets.QSpinBox()
                ed.setRange(0, kw["hi"])
                ed.setSpecialValueText("auto")
            else:
                ed = QtWidgets.QComboBox()
                ed.addItems(kw["items"])
            self.cfg_widgets[key] = (kind, ed)
            form.addRow(label, ed)
        self.btn_apply = QtWidgets.QPushButton("Apply")
        self.btn_apply.clicked.connect(self.apply_config)
        self.btn_reload = QtWidgets.QPushButton("Read from server")
        self.btn_reload.clicked.connect(self.load_config)
        form.addRow(self.btn_apply)
        form.addRow(self.btn_reload)
        self.actual_label = QtWidgets.QLabel("")
        self.actual_label.setWordWrap(True)
        form.addRow(self.actual_label)
        dock = QtWidgets.QDockWidget("Scan", self)
        dock.setWidget(w)
        self.addDockWidget(QtCore.Qt.LeftDockWidgetArea, dock)

    def _build_roi_dock(self):
        w = QtWidgets.QWidget()
        grid = QtWidgets.QGridLayout(w)
        self.roi_spins = []
        for i, (label, row, col) in enumerate([("Slow (rows) from", 0, 0), ("to", 0, 2),
                                               ("Fast (cols) from", 1, 0), ("to", 1, 2)]):
            sp = QtWidgets.QDoubleSpinBox()
            sp.setRange(0.0, 1.0)
            sp.setDecimals(4)
            sp.setSingleStep(0.01)
            grid.addWidget(QtWidgets.QLabel(label), row, col)
            grid.addWidget(sp, row, col + 1)
            self.roi_spins.append(sp)
        self.roi_volts = QtWidgets.QLabel("")
        grid.addWidget(self.roi_volts, 2, 0, 1, 4)

        self.btn_select = QtWidgets.QPushButton("Select on image")
        self.btn_select.setCheckable(True)
        self.btn_select.toggled.connect(self.view.set_select_mode)
        self.btn_roi_apply = QtWidgets.QPushButton("Apply window")
        self.btn_roi_apply.clicked.connect(lambda: self.set_roi(tuple(s.value() for s in self.roi_spins)))
        self.btn_crop = QtWidgets.QPushButton("Crop to selection")
        self.btn_crop.setToolTip("Software window: same galvo scan, fewer samples per pixel. Works while running.")
        self.btn_crop.clicked.connect(self.crop_to_selection)
        self.btn_zoom = QtWidgets.QPushButton("Zoom scan to selection")
        self.btn_zoom.setToolTip("Changes galvo amplitude/offset so the scan covers the selection "
                                 "(full sample density). Restarts the scan.")
        self.btn_zoom.clicked.connect(self.zoom_to_selection)
        self.btn_zoom_out = QtWidgets.QPushButton("Zoom out")
        self.btn_zoom_out.clicked.connect(self.zoom_out)
        self.btn_full = QtWidgets.QPushButton("Full field")
        self.btn_full.clicked.connect(self.full_field)
        for i, b in enumerate((self.btn_select, self.btn_roi_apply, self.btn_crop, self.btn_zoom,
                               self.btn_zoom_out, self.btn_full)):
            grid.addWidget(b, 3 + i // 2, 2 * (i % 2), 1, 2)
        self.view.selected.connect(self.on_selected)
        dock = QtWidgets.QDockWidget("Image window", self)
        dock.setWidget(w)
        self.addDockWidget(QtCore.Qt.LeftDockWidgetArea, dock)

    def _build_display_dock(self):
        w = QtWidgets.QWidget()
        form = QtWidgets.QFormLayout(w)
        self.cmap_box = QtWidgets.QComboBox()
        self.cmap_box.addItems(["gray", "viridis", "inferno", "magma", "cividis"])
        self.cmap_box.currentTextChanged.connect(self.set_cmap)
        self.auto_levels = QtWidgets.QCheckBox("auto (0.5–99.5 %)")
        self.auto_levels.setChecked(True)
        self.lo_spin = QtWidgets.QDoubleSpinBox()
        self.hi_spin = QtWidgets.QDoubleSpinBox()
        for s in (self.lo_spin, self.hi_spin):
            s.setRange(-1e9, 1e9)
            s.setDecimals(1)
            s.valueChanged.connect(self.redraw)
        self.hi_spin.setValue(16383)
        self.auto_levels.toggled.connect(self.redraw)
        self.img_sel = QtWidgets.QComboBox()
        self.img_sel.addItems(["image 0 (sum / forward)", "image 1 (backward)", "side by side",
                               "difference 0 − 1"])
        self.img_sel.currentIndexChanged.connect(self.redraw)
        form.addRow("Colormap", self.cmap_box)
        form.addRow("Levels", self.auto_levels)
        form.addRow("Min", self.lo_spin)
        form.addRow("Max", self.hi_spin)
        form.addRow("Show", self.img_sel)
        dock = QtWidgets.QDockWidget("Display", self)
        dock.setWidget(w)
        self.addDockWidget(QtCore.Qt.LeftDockWidgetArea, dock)

    def _build_log_dock(self):
        self.log = QtWidgets.QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(500)
        dock = QtWidgets.QDockWidget("Log", self)
        dock.setWidget(self.log)
        self.addDockWidget(QtCore.Qt.BottomDockWidgetArea, dock)

    def log_msg(self, text):
        self.log.appendPlainText(time.strftime("%H:%M:%S ") + text)

    def _set_connected(self, on):
        self.btn_connect.setText("Disconnect" if on else "Connect")
        for b in (self.btn_start, self.btn_stop, self.btn_apply, self.btn_reload, self.btn_roi_apply,
                  self.btn_crop, self.btn_zoom, self.btn_zoom_out, self.btn_full):
            b.setEnabled(on)
        if not on:
            self.status_server.setText("not connected")

    # ---------------------------------------------------------------- network
    def toggle_connection(self):
        if self.client:
            self.disconnect()
            return
        host, port = self.host_edit.text().strip(), self.port_spin.value()
        try:
            self.client = Client(host, port,
                                 on_frame=self.bridge.push_frame,
                                 on_status=self.bridge.status.emit,
                                 on_error=lambda e: self.bridge.error.emit(str(e)))
            self.client.command("ping")
        except Exception as exc:
            self.client = None
            self.log_msg(f"connect {host}:{port} failed: {exc}")
            return
        self.log_msg(f"connected to {host}:{port}")
        self._set_connected(True)
        self.load_config()

    def disconnect(self):
        if self.client:
            self.client.close()
            self.client = None
        self._set_connected(False)
        self.log_msg("disconnected (the server stops the scan)")

    def send(self, cmd):
        if not self.client:
            return None
        try:
            out = self.client.command(cmd)
            self.log_msg(f"> {cmd}: OK {out}".rstrip())
            return out
        except CommandError as exc:
            self.log_msg(f"> {cmd}: {exc}")
        except Exception as exc:
            self.log_msg(f"> {cmd}: {exc}")
            self.disconnect()
        return None

    def load_config(self):
        if not self.client:
            return
        try:
            cfg = self.client.get_config()
        except Exception as exc:
            self.log_msg(f"get config: {exc}")
            return
        self.server_cfg = cfg
        for key, (kind, ed) in self.cfg_widgets.items():
            if key not in cfg:
                continue
            val = cfg[key]
            ed.blockSignals(True)
            if kind == "choice":
                ed.setCurrentText(val)
            elif kind == "size":
                ed.setValue(0 if val == "auto" else int(float(val)))
            elif kind == "int":
                ed.setValue(int(float(val)))
            else:
                ed.setValue(float(val))
            ed.blockSignals(False)
        actual = [f"{k} = {v}" for k, v in cfg.items() if k.endswith("_actual")]
        self.actual_label.setText("\n".join(actual))
        self.roi = parse_roi(cfg.get("img.roi", roi_text(DEFAULT_ROI)))
        for sp, v in zip(self.roi_spins, self.roi):
            sp.blockSignals(True)
            sp.setValue(v)
            sp.blockSignals(False)
        self._update_roi_volts()

    def apply_config(self):
        for key, (kind, ed) in self.cfg_widgets.items():
            if kind == "choice":
                val = ed.currentText()
            elif kind == "size":
                val = "auto" if ed.value() == 0 else str(ed.value())
            elif kind == "int":
                val = str(ed.value())
            else:
                val = repr(ed.value())
            if self.server_cfg.get(key) != val:
                self.send(f"set {key} {val}")
        self.load_config()

    def start_scan(self):
        self.fps_times.clear()
        self.report_drops()                       # flush counts of the previous run
        self.bridge.reset_stats()
        self.reported = {}
        self.server_dropped = None
        if self.send("start") is not None:
            self.server_state = "running"

    def stop_scan(self):
        if self.send("stop") is not None:
            self.server_state = "idle"

    def on_net_error(self, msg):
        self.log_msg(f"connection lost: {msg}")
        self.client = None
        self._set_connected(False)

    # ---------------------------------------------------------------- image window
    def _scan(self):
        c = self.server_cfg
        return tuple(float(c.get(k, 0)) for k in ("slow.amp", "slow.offset", "fast.amp", "fast.offset"))

    def _update_roi_volts(self):
        sa, so, fa, fo = self._scan()
        x0, x1, y0, y1 = self.roi
        self.roi_volts.setText(f"slow {so + sa * (2 * x0 - 1):+.4f} … {so + sa * (2 * x1 - 1):+.4f} V   "
                               f"fast {fo + fa * (2 * y0 - 1):+.4f} … {fo + fa * (2 * y1 - 1):+.4f} V")

    def _selection_fractions(self):
        """Selected rectangle as (x0, x1, y0, y1) fractions of the full scan range, or None."""
        rect, f = self.view.selection(), self.frame
        if rect is None or f is None:
            self.log_msg("select a rectangle on the image first (\"Select on image\")")
            return None
        left = rect.left()
        if f.n_images == 2 and self.img_sel.currentIndex() == 2 and left >= f.width:
            left -= f.width + max(2, f.width // 50)          # selection in the right-hand image
        c0 = min(max(left / f.width, 0.0), 1.0)
        c1 = min(max((left + rect.width()) / f.width, 0.0), 1.0)
        r0 = min(max(rect.top() / f.height, 0.0), 1.0)
        r1 = min(max(rect.bottom() / f.height, 0.0), 1.0)
        x0, x1, y0, y1 = f.roi            # window of the displayed frame, not the requested one
        return (x0 + r0 * (x1 - x0), x0 + r1 * (x1 - x0), y0 + c0 * (y1 - y0), y0 + c1 * (y1 - y0))

    def on_selected(self, rect):
        sel = self._selection_fractions()
        if sel:
            sa, so, fa, fo = self._scan()
            self.log_msg(f"selection: slow {sel[0]:.4f}–{sel[1]:.4f}, fast {sel[2]:.4f}–{sel[3]:.4f} "
                         f"(slow {so + sa * (2 * sel[0] - 1):+.4f}…{so + sa * (2 * sel[1] - 1):+.4f} V, "
                         f"fast {fo + fa * (2 * sel[2] - 1):+.4f}…{fo + fa * (2 * sel[3] - 1):+.4f} V)")

    def set_roi(self, roi):
        if self.send(f"set img.roi {roi_text(roi)}") is not None:
            self.view.clear_selection()
            self.load_config()

    def crop_to_selection(self):
        sel = self._selection_fractions()
        if sel:
            self.set_roi(sel)

    def _apply_scan(self, slow_amp, slow_off, fast_amp, fast_off, roi):
        """Set galvo scan + window; restarts the scan if it was running."""
        was_running = self.server_state == "running"
        if was_running:
            self.send("stop")
        ok = all(self.send(f"set {k} {v!r}") is not None for k, v in
                 (("slow.amp", slow_amp), ("slow.offset", slow_off),
                  ("fast.amp", fast_amp), ("fast.offset", fast_off)))
        ok = ok and self.send(f"set img.roi {roi_text(roi)}") is not None
        self.view.clear_selection()
        self.load_config()
        if was_running and ok:
            self.start_scan()

    def zoom_to_selection(self):
        sel = self._selection_fractions()
        if not sel:
            return
        sa, so, fa, fo = self._scan()
        self.zoom_stack.append((sa, so, fa, fo, self.roi))
        x0, x1, y0, y1 = sel
        d = DEFAULT_ROI

        # new scan range such that the default window maps exactly onto the selection
        def new_axis(amp, off, a, b, w0, w1):
            lo, hi = off - amp + 2 * amp * a, off - amp + 2 * amp * b    # selection in volts
            full = (hi - lo) / (w1 - w0)
            start = lo - w0 * full
            return full / 2, start + full / 2

        nsa, nso = new_axis(sa, so, x0, x1, d[0], d[1])
        nfa, nfo = new_axis(fa, fo, y0, y1, d[2], d[3])
        self.log_msg(f"zoom: slow {nso:+.4f}±{nsa:.4f} V, fast {nfo:+.4f}±{nfa:.4f} V")
        self._apply_scan(nsa, nso, nfa, nfo, DEFAULT_ROI)

    def zoom_out(self):
        if not self.zoom_stack:
            self.log_msg("no previous zoom level")
            return
        self._apply_scan(*self.zoom_stack.pop())

    def full_field(self):
        if self.zoom_stack:
            first = self.zoom_stack[0]
            self.zoom_stack.clear()
            self._apply_scan(*first[:4], DEFAULT_ROI)
        else:
            self.set_roi(DEFAULT_ROI)

    def on_status(self, st):
        self.server_state = st.get("state", self.server_state)
        if "frames_dropped" in st:
            n = int(st["frames_dropped"])
            if self.server_dropped is not None and n > self.server_dropped:
                self.log_msg(f"server: {n - self.server_dropped} frame(s) dropped (link too slow), "
                             f"{n} this run")
            self.server_dropped = n
        txt = (f"{st.get('state', '?')} | {float(st.get('rate_rec_s', 0)) / 1e6:.2f} Mrec/s | "
               f"sent {st.get('frames_sent', '?')} dropped {st.get('frames_dropped', '?')} | "
               f"lost chunks {st.get('chunks_lost', '?')}")
        if st.get("mock") == "1":
            txt += " | MOCK"
        self.status_server.setText(txt)

    # ---------------------------------------------------------------- drop accounting
    def report_drops(self):
        """Once per second: log new drops and show totals in the status bar."""
        st, gaps = self.bridge.snapshot()
        st["recorder"] = self.recorder.dropped
        new = {k: st[k] - self.reported.get(k, 0) for k in ("link", "display", "data_lost", "recorder")}
        if new["link"]:
            ids = ", ".join(f"{a}" if a == b else f"{a}–{b}" for a, b in gaps)
            self.log_msg(f"DROPPED {new['link']} frame(s) on the way (ids {ids}): link or client too slow")
        if new["recorder"]:
            self.log_msg(f"RECORDING: {new['recorder']} frame(s) not saved (disk too slow)")
        if new["data_lost"]:
            self.log_msg(f"{new['data_lost']} frame(s) flagged DATA_LOST by the server "
                         f"(acquisition data missing inside the frame)")
        if new["display"]:
            self.log_msg(f"{new['display']} frame(s) not displayed (viewer busy; still recorded)")
        now = time.monotonic()
        rate = ((st["bytes"] - self._rx_last[1]) / (now - self._rx_last[0]) / 1e6
                if st["bytes"] >= self._rx_last[1] and now > self._rx_last[0] else 0.0)
        self._rx_last = (now, st["bytes"])
        self.reported = st
        total = st["received"] + st["link"]
        self.status_drops.setText(
            f"rx {rate:.2f} MB/s | dropped: link {st['link']}/{total} | display {st['display']} | "
            f"rec {st['recorder']}"
            + (f" | data-lost {st['data_lost']}" if st["data_lost"] else ""))
        self.status_drops.setStyleSheet("color: #c00" if st["link"] or st["recorder"] else "")

    # ---------------------------------------------------------------- frames
    def on_frame(self):
        f = self.bridge.take_frame()
        if f is None:
            return
        self.frame = f
        now = time.monotonic()
        self.fps_times = [t for t in self.fps_times if now - t < 3.0] + [now]
        fps = (len(self.fps_times) - 1) / (self.fps_times[-1] - self.fps_times[0]) \
            if len(self.fps_times) > 1 else 0.0
        npix = f.width * f.height * f.n_images
        lost = " | DATA LOST" if f.flags & FLAG_DATA_LOST else ""
        fmt = {0: "f32", 1: "u16", 2: "u8"}.get(f.dtype, "?")
        self.status_frame.setText(
            f"frame {f.frame_id} | {f.width}×{f.height}×{f.n_images} {fmt} "
            f"{f.nbytes / 1e6:.2f} MB | {fps:.1f} fps | "
            f"empty {100 * f.n_empty / max(npix, 1):.1f} % | records {f.n_records}{lost}")
        self.redraw()

    def _select(self):
        f = self.frame
        sel = self.img_sel.currentIndex()
        if f.n_images < 2 or sel == 0:
            return f.data[0]
        if sel == 1:
            return f.data[1]
        if sel == 2:
            gap = np.full((f.height, max(2, f.width // 50)), np.nan, np.float32)
            return np.hstack([f.data[0], gap, f.data[1]])
        return f.data[0] - f.data[1]

    def redraw(self):
        if self.frame is None:
            return
        img = self._select()
        self.shown = img
        finite = img[np.isfinite(img)]
        if self.auto_levels.isChecked() and finite.size:
            lo, hi = np.percentile(finite, [0.5, 99.5])
            for s, v in ((self.lo_spin, lo), (self.hi_spin, hi)):
                s.blockSignals(True)
                s.setValue(float(v))
                s.blockSignals(False)
        else:
            lo, hi = self.lo_spin.value(), self.hi_spin.value()
        scale = 255.0 / (hi - lo) if hi > lo else 0.0
        idx = np.nan_to_num((img - lo) * scale, nan=0.0)
        idx = np.clip(idx, 0, 255).astype(np.uint8)
        rgb = self.lut[idx]
        rgb[~np.isfinite(img)] = (255, 0, 255)          # empty pixels in magenta
        rgb = np.ascontiguousarray(rgb)
        h, w = img.shape
        qimg = QtGui.QImage(rgb.data, w, h, 3 * w, QtGui.QImage.Format_RGB888).copy()
        self.view.set_image(qimg)

    def set_cmap(self, name):
        self.lut = colormap_lut(name)
        self.redraw()

    def on_hover(self, x, y):
        img = self.shown
        if img is None or not (0 <= y < img.shape[0] and 0 <= x < img.shape[1]):
            self.status_hover.setText("")
            return
        self.status_hover.setText(f"(row {y}, col {x}) = {img[y, x]:.1f}")

    # ---------------------------------------------------------------- saving
    def save_frame(self):
        if self.frame is None:
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "Save frame", f"frame_{self.frame.frame_id:06d}.npz", "NumPy (*.npz)")
        if path:
            write_npz(path, self.frame, self.server_cfg)
            self.log_msg(f"saved {path}")

    def toggle_record(self, on):
        if on:
            d = QtWidgets.QFileDialog.getExistingDirectory(self, "Record frames into folder")
            if not d:
                self.btn_record.setChecked(False)
                return
            self.recorder.start(d, self.server_cfg)
            self.log_msg(f"recording into {d} (every received frame)")
        else:
            if self.recorder.folder:
                self.log_msg(f"recording stopped: {self.recorder.written} written, "
                             f"{self.recorder.dropped} not saved")
            self.recorder.stop()

    def closeEvent(self, e):
        self.disconnect()
        super().closeEvent(e)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="rp-f0a360.local")
    ap.add_argument("--port", type=int, default=5000)
    args = ap.parse_args()
    app = QtWidgets.QApplication(sys.argv)
    win = Viewer(args.host, args.port)
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
