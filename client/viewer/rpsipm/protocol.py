"""RedPitaya_SiPM server2 protocol v1 (see documentation/PROTOCOL.md)."""

import queue
import socket
import struct
import threading
from dataclasses import dataclass

import numpy as np

MAGIC = 0x49535052
VERSION = 1
T_CMD, T_REPLY, T_FRAME, T_STATUS = 1, 2, 3, 4

HDR = struct.Struct("<IHHI")
FRAME_HDR = struct.Struct("<IHHBBHQQIIIff4f")
assert HDR.size == 12 and FRAME_HDR.size == 64

FLAG_DATA_LOST = 1 << 0
FLAG_PARTIAL = 1 << 1


class ProtocolError(Exception):
    pass


class CommandError(Exception):
    def __init__(self, code, message):
        super().__init__(f"ERR {code} {message}")
        self.code = code
        self.message = message


@dataclass
class Frame:
    frame_id: int
    width: int
    height: int
    n_images: int
    flags: int
    t_start_ns: int
    t_end_ns: int
    n_records: int
    n_discarded: int
    n_empty: int
    slow_freq: float
    fast_freq: float
    roi: tuple                  # (x0, x1, y0, y1) window of this frame
    data: np.ndarray            # shape (n_images, height, width), float32

    def header_dict(self):
        return {k: v for k, v in self.__dict__.items() if k != "data"}


def pack_msg(mtype, payload):
    return HDR.pack(MAGIC, VERSION, mtype, len(payload)) + payload


def pack_frame(frame_id, data, flags=0, t_start_ns=0, t_end_ns=0, n_records=0,
               n_discarded=0, slow_freq=0.0, fast_freq=0.0, roi=(0.0, 1.0, 0.0, 1.0)):
    data = np.ascontiguousarray(data, dtype="<f4")
    if data.ndim == 2:
        data = data[None]
    n_img, h, w = data.shape
    n_empty = int(np.isnan(data).sum())
    hdr = FRAME_HDR.pack(frame_id, w, h, 0, n_img, flags, t_start_ns, t_end_ns,
                         n_records, n_discarded, n_empty, slow_freq, fast_freq, *roi)
    return pack_msg(T_FRAME, hdr + data.tobytes())


def parse_frame(payload):
    if len(payload) < FRAME_HDR.size:
        raise ProtocolError("short frame")
    (fid, w, h, dtype, n_img, flags, t0, t1, n_rec, n_disc, n_empty,
     f_slow, f_fast, *roi) = FRAME_HDR.unpack_from(payload)
    if dtype != 0:
        raise ProtocolError(f"unsupported dtype {dtype}")
    expected = FRAME_HDR.size + 4 * n_img * h * w
    if len(payload) != expected:
        raise ProtocolError(f"frame size {len(payload)} != {expected}")
    data = np.frombuffer(payload, "<f4", offset=FRAME_HDR.size).reshape(n_img, h, w)
    return Frame(fid, w, h, n_img, flags, t0, t1, n_rec, n_disc, n_empty, f_slow, f_fast,
                 tuple(roi), data)


def parse_kv(text):
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def recv_exact(sock, n):
    buf = bytearray(n)
    view = memoryview(buf)
    got = 0
    while got < n:
        k = sock.recv_into(view[got:], n - got)
        if k == 0:
            raise ConnectionError("connection closed")
        got += k
    return bytes(buf)


def recv_msg(sock):
    magic, version, mtype, length = HDR.unpack(recv_exact(sock, HDR.size))
    if magic != MAGIC:
        raise ProtocolError(f"bad magic 0x{magic:08x}")
    if version != VERSION:
        raise ProtocolError(f"unsupported version {version}")
    return mtype, recv_exact(sock, length) if length else b""


class Client:
    """Connection to the server. Frames and status go to callbacks on a reader thread."""

    def __init__(self, host, port=5000, on_frame=None, on_status=None, on_error=None, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(None)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.on_frame = on_frame
        self.on_status = on_status
        self.on_error = on_error
        self.timeout = timeout
        self._replies = queue.Queue()
        self._send_lock = threading.Lock()
        self._closed = False
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self):
        try:
            while True:
                mtype, payload = recv_msg(self.sock)
                if mtype == T_REPLY:
                    self._replies.put(payload.decode(errors="replace"))
                elif mtype == T_FRAME and self.on_frame:
                    self.on_frame(parse_frame(payload))
                elif mtype == T_STATUS and self.on_status:
                    self.on_status(parse_kv(payload.decode(errors="replace")))
        except Exception as exc:          # connection closed or protocol error
            self._replies.put(None)
            if not self._closed and self.on_error:
                self.on_error(exc)

    def command(self, text):
        """Send one command and wait for its reply. Returns the text after OK."""
        with self._send_lock:
            self.sock.sendall(pack_msg(T_CMD, text.encode()))
            try:
                reply = self._replies.get(timeout=self.timeout)
            except queue.Empty:
                raise TimeoutError(f"no reply to '{text}'") from None
        if reply is None:
            raise ConnectionError("connection closed")
        if reply.startswith("OK"):
            return reply[2:].lstrip(" \n")
        parts = reply.split(" ", 2)
        code = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else -1
        raise CommandError(code, parts[2] if len(parts) > 2 else reply)

    def set(self, key, value):
        return self.command(f"set {key} {value}")

    def get_config(self):
        return parse_kv(self.command("get config"))

    def close(self):
        self._closed = True
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()
