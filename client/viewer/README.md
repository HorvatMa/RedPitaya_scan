# RedPitaya SiPM viewer

PyQt5 live viewer for `server2` (protocol: `documentation/PROTOCOL.md`). Needs PyQt5, numpy and
(optionally, for colormaps) matplotlib. All three are in `~/.venv/base`.

```sh
cd client/viewer
~/.venv/base/bin/python viewer.py --host rp-f0a360.local     # real board (once server2 runs there)

~/.venv/base/bin/python mock_server.py &                     # or: test without the board
~/.venv/base/bin/python viewer.py --host 127.0.0.1
```

## Using it
- **Scan panel:** edit the values, then **Apply** to send only the changed keys. The server's actual
  values (exact frequencies, image size) appear below the panel. Changes are accepted only while
  stopped.
- **Pixel format** (`img.dtype`): float32, uint16 or uint8 on the wire. It can be changed while
  running. Integer frames are decoded back to real values (precision: one step of the frame's
  value range). The status bar shows the format, the bytes per frame and the received MB/s.
- **Start / Stop** control the scan. Disconnecting also stops it (server rule).
- **Display:** colormap, auto (0.5–99.5 % percentile) or manual levels. With `Bidirectional = both`
  you can view forward, backward, side by side, or their difference (useful for tuning
  `Delay`). Empty pixels (NaN) are shown in magenta.
- **Image window** (`img.roi`, fractions of the full scan range, slow = rows, fast = columns):
  - **Select on image**, then drag a rectangle;
  - **Crop to selection** is a software window: same galvo scan, fewer samples per pixel, works while
    running;
  - **Zoom scan to selection** changes the galvo amplitude/offset so the scan covers the rectangle at
    full sample density. It restarts the scan;
  - **Zoom out** steps back through the zoom history, **Full field** returns to the first scan and the
    full window;
  - the window can also be typed in and sent with **Apply window**. Its extent in volts is shown below
    the fields;
  - each frame carries the window it was binned with, so selections are always converted with the
    right window.
- Mouse: wheel = zoom, drag = pan, double-click = fit. The status bar shows the pixel value under the
  cursor.
- **Dropped frames** are logged once per second, and totals are shown in the status bar (red when
  data is missing):
  - `link`: frame-id gaps, i.e. the server dropped frames because the network or the viewer was too
    slow. Cross-checked with the server's `frames_dropped` counter;
  - `display`: frames received but replaced before they could be drawn (they are still recorded);
  - `rec`: frames the recorder could not save in time (disk too slow);
  - `data-lost`: frames the server flagged because acquisition data was lost inside the frame.
- `mock_server.py --throttle 0.5` limits the mock link to 0.5 MB/s to try this out.
- **Save frame** writes `.npz` (`data`, plus `header` and `config` as JSON). **Record** saves *every
  received* frame into a folder (uncompressed, on a separate thread), not only the displayed ones.

Load a saved frame:
```python
import json, numpy as np
z = np.load("frame_000012.npz"); img = z["data"][0]; hdr = json.loads(str(z["header"]))
```

## Tests
`QT_QPA_PLATFORM=offscreen ~/.venv/base/bin/python test_viewer.py` runs the protocol round trip,
a mock-server session and a headless GUI run.
