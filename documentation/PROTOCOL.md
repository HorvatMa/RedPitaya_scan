# RedPitaya_SiPM server2 — TCP protocol v1

One TCP connection (default port 5000), one client at a time. All binary fields are **little-endian**.
Reference implementation of the PC side: `client/viewer/rpsipm/protocol.py`.

## Message header (12 bytes)
| Offset | Type | Field | Value |
|---|---|---|---|
| 0 | u32 | magic | `0x49535052` (bytes `R P S I`) |
| 4 | u16 | version | 1 |
| 6 | u16 | type | 1 CMD, 2 REPLY, 3 FRAME, 4 STATUS |
| 8 | u32 | length | payload bytes following the header |

## CMD (PC → RP) / REPLY (RP → PC)
- The payload is ASCII text, one command per message. Every CMD gets exactly one REPLY, in order.
- A REPLY is `OK` or `OK <text>`, or `ERR <code> <message>`. Multi-line OK replies use `\n` separators.

| Command | Reply |
|---|---|
| `ping` | `OK pong` |
| `set <key> <value>` | `OK` / `ERR 2 ...`. Allowed only while not running, except keys marked *live*. The value is the rest of the line, so it may contain spaces. |
| `get config` | `OK` + one `key=value` per line, including derived values (`*.freq_actual`, `img.width_actual`, …) |
| `get status` | `OK` + `key=value` lines (same content as STATUS) |
| `start` | `OK` once acquisition is running |
| `stop` | `OK` once stopped (generator off) |

Error codes:

| Code | Meaning |
|---|---|
| 1 | unknown command |
| 2 | bad key/value |
| 3 | wrong state |
| 4 | hardware error |
| 5 | internal error |

Keys (volts are at the galvo/driver side):

| Key | Type | Default |
|---|---|---|
| `slow.shape` | `sawtooth` \| `triangle` | sawtooth |
| `slow.rise` | 0..1 | 0.975 |
| `slow.freq` | Hz | 3 |
| `slow.amp`, `slow.offset` | V | — |
| `fast.shape` | `sine` \| `triangle` | sine |
| `fast.freq` | Hz | 1000 |
| `fast.amp`, `fast.offset` | V | — |
| `acq.dec` | int ≥ 1 | 50 |
| `acq.int_dec` | int ≥ 1 | 1 |
| `img.width`, `img.height` | int or `auto` | auto |
| `img.bidir` | `sum` \| `forward` \| `backward` \| `both` | sum |
| `img.dtype` | `float32` \| `uint16` \| `uint8`: pixel format on the wire. *live*: applied to the next frame sent. | float32 |
| `img.roi` | `x0 x1 y0 y1`: image window as fractions (0..1) of the full commanded range, slow (x, rows) then fast (y, columns); `x0 < x1`, `y0 < y1`. *live*: applied from the next frame. Auto width/height scale with the window. | `0 1 0.02 0.98` |
| `cal.delay` | samples | 0.5 |
| `cal.fast_gain`, `cal.fast_offset` | sensor → command calibration | — |

## FRAME (RP → PC)
Payload = 72-byte frame header followed by `n_images × height × width` pixels in the format `dtype`, row-major. Image 0 is
the sum or forward image; image 1 is the backward image (only with `img.bidir both`). NaN = pixel without
samples.

**Frame ids and dropping:**
- `frame_id` is assigned when a frame is reconstructed and restarts at 0 on every `start`.
- The server's sender always sends the newest finished frame. If the link is slower than the frame
  rate, frames are dropped on the server (counted in STATUS `frames_dropped`), and their ids never
  appear.
- A gap in `frame_id` therefore means frames were dropped because the link or the client was too slow.
- TCP itself never loses data.
- The `DATA_LOST` flag is different: acquisition records were lost *inside* that frame (FPGA FIFO,
  ring buffer).

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | frame_id |
| 4 | u16 | width |
| 6 | u16 | height |
| 8 | u8 | dtype: 0 = float32, 1 = uint16, 2 = uint8 |
| 9 | u8 | n_images |
| 10 | u16 | flags (bit0 DATA_LOST, bit1 PARTIAL) |
| 12 | u64 | t_start_ns (CLOCK_MONOTONIC on the RP) |
| 20 | u64 | t_end_ns |
| 28 | u32 | n_records |
| 32 | u32 | n_discarded |
| 36 | u32 | n_empty |
| 40 | f32 | slow_freq_actual (Hz) |
| 44 | f32 | fast_freq_actual (Hz) |
| 48 | 4 × f32 | roi `x0 x1 y0 y1` this frame was binned with (see `img.roi`) |
| 64 | f32 | value_lo: smallest finite pixel value of the frame (NaN if none) |
| 68 | f32 | value_hi: largest finite pixel value |

**Integer pixel formats** (uint16, uint8; max = 65535 or 255):
- code 0 = empty pixel (NaN);
- a finite value v becomes `1 + round((v − lo) / (hi − lo) · (max − 1))`;
- decode with `v = lo + (code − 1) · (hi − lo) / (max − 1)`;
- `lo`/`hi` are per frame, so values stay quantitative within one step of `(hi − lo)/(max − 1)`.

Bytes per frame = 72 + n_images·W·H·(4|2|1). For orientation, on the measured board link (100 Mbit/s, about
11.8 MB/s): 1920×1080 runs at about 1.4 fps in float32, 2.8 in uint16 and 5.7 in uint8.

## STATUS (RP → PC)
Sent about once per second, unsolicited. ASCII `key=value` lines. Keys include `state`
(`idle`/`running`/`error`), `rate_rec_s`, `frames_sent`, `frames_dropped`, `chunks_lost`, `ring_fill`
and `cpu`. Clients must ignore unknown keys.
