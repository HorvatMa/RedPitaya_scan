# RedPitaya_SiPM — new server: design outline (draft v0.1, 2026-10-27)

Status: **v0.3.** Decisions are in §10–12, the fast-sensor resolution limit is in §13. Implemented and tested on the PC with the
fake hardware backend: reconstruction core, config/volts layer, threads, frame pool and TCP server
(`server2/`, `make && python tests/test_server.py`). The board backend `hw_uio.c` (M1) is still missing. Related: [SERVER_TODO.md](SERVER_TODO.md),
[FPGA_MANUAL.md](FPGA_MANUAL.md).

## 1. Goal
A single C program running on the Red Pitaya that:
1. configures the FPGA (scan waveforms, decimations, input selector);
2. reads the sample stream (slow-axis command, fast-axis sensor, detector integral) through UIO and the
   interrupt;
3. reconstructs images on the board;
4. sends **only finished images** (plus status) to one PC client over TCP.

Scan model:
- Slow axis = commanded value; the galvo is assumed to follow it exactly.
- Fast axis = measured sensor value (IN2, `INPUT_SEL = 1`).
- Detector = IN1 integral.

## 2. Folder layout (proposed folder `server2/`, name TBC)
```
server2/
├── Makefile              # `make` (board), `make host` (PC build with fake HW), `make test`
├── README.md
├── src/
│   ├── main.c            # args, config load, signals, thread start/stop
│   ├── app.c/.h          # state machine IDLE → CONFIGURED → RUNNING, owns everything
│   ├── hw/
│   │   ├── regs.h        # register offsets + bit definitions (single source of truth)
│   │   ├── uio.c/.h      # find /dev/uioN by name, mmap maps, IRQ enable/wait (poll + timeout)
│   │   ├── fpga.c/.h     # SAFE API on top of regs (all FPGA rules enforced here)
│   │   └── fpga_fake.c   # host-only backend: synthetic records + fake IRQ (for PC dev/tests)
│   ├── acq/
│   │   └── acq_thread.c  # IRQ → copy chunk → ack (CTRL=3) → ring buffer; stats
│   ├── recon/
│   │   ├── scan.c/.h     # waveform generation + frequency quantisation (dec·len)
│   │   ├── mapper.c/.h   # record → (row, col, value): calibration, delay, direction
│   │   ├── frame.c/.h    # accumulators, frame boundary detection, finalise, frame pool
│   │   └── recon_thread.c
│   ├── net/
│   │   ├── proto.c/.h    # message framing, encode/decode, command parser
│   │   └── server.c/.h   # listen/accept (1 client), control loop, sender thread
│   └── util/
│       ├── ring.c/.h     # lock-free SPSC ring buffer (records)
│       ├── log.c/.h, timeutil.h, config.c/.h (key = value file)
└── tests/
    ├── test_mapper.c, test_frame.c, test_proto.c, test_scan.c
    ├── sim_records.c     # synthetic stream generator (also used by fpga_fake)
    └── client_min.py     # minimal Python client / viewer (moves to client/ later)
```
The reconstruction, protocol and utility code is plain C and doesn't depend on the hardware, so it also
builds on the PC. That lets most of the development and testing happen without the board.

Target language: C11 (gnu11) with pthreads. Check the gcc version on the board (Ubuntu 16.04 → probably
gcc 5.x).

## 3. Threads and data flow
```
             TCP client
               ▲   │ commands
     frames,   │   ▼
     status ┌──────────────┐   config / start / stop    ┌──────────────┐
            │ net: control │ ─────────────────────────► │     app      │
            │   + sender   │ ◄── frame pool (triple) ── │ state machine│
            └──────────────┘                            └──────┬───────┘
                    ▲                                          │ owns
            finished frames                                    ▼
            ┌───────┴────────┐      SPSC ring of records  ┌──────────────┐
            │  recon thread  │ ◄───────────────────────── │  acq thread  │ ◄── IRQ 62 (/dev/uio)
            │ map → bin →    │                            │ copy chunk,  │ ◄── DDR buffer 0x0A00_0000
            │ frame boundary │                            │ ack CTRL=3   │
            └────────────────┘                            └──────────────┘
```
| Thread | Job | Priority |
|---|---|---|
| **acq** | `poll(uio_fd, 100 ms)` → read `BUFFER_N` → memcpy `N·8 B` from the uncached buffer into the ring → write `CTRL = 3` → re-enable the IRQ. Counts chunks, records, ring overflows and time-to-ack. | high (SCHED_FIFO optional) |
| **recon** | Pops records, maps them to pixels, accumulates, detects frame ends, publishes frames. | normal |
| **sender** | Sends the newest finished frame. If the client is slow, older frames are dropped and counted. | normal |
| **control** (main) | Accepts one client, parses commands, replies OK/ERR, sends status every 1 s. | normal |

- **Why a ring buffer between acq and recon:** acknowledging quickly matters, because the FPGA FIFO only
  covers `16384·dec/125 MHz` (6.6 ms at dec 50). Reconstruction may briefly lag without losing data.
- **Ring size:** e.g. 4–8 M records (32–64 MB). The board has about 370 MB free.

## 4. Hardware layer — rules enforced in `fpga.c`
- Find `rp_acquire` and `rp_generate` by name. Map0/map1 of each are mmapped with offset `n·pagesize`.
- The control register is **only ever written as 3** (there is no API that can write 0 or 1).
- `DEC_FACTOR`, `DEC_GEN_x` and `INTEGRATOR_DEC` must each be ≥ 1.
- `FIFO_MIN_THRESH` is never touched (stays 15). `INPUT_SEL = 1` on open.
- Waveforms:
  - ch1 ≤ 16384 samples, ch2 ≤ **12288** samples;
  - packed into 32-bit words (two int16 per word) and written word by word, never with memcpy;
  - an even length is enforced, or padded.
- Start: write waveforms and generator registers → `START_GEN` → wait for 1 slow period (optional) →
  set acquisition registers → `START_ACQ`.
- Stop:
  - `START_ACQ = 0`;
  - discard the pending chunk and the FIFO remainder (wait for the IRQ with a timeout, ack, discard);
  - `START_GEN = 0`, after optionally ramping the waveform to the centre, since stopping the generator
    otherwise makes the galvos jump to centre.
- The blocking IRQ wait has a timeout, so stopping never hangs.

## 5. Reconstruction
### 5.1 Inputs per record
`x = slow command (int16)`, `y = fast sensor (int16)`, `v = integral (uint32)`, with sample period
`T = DEC_FACTOR / 125 MHz`.

### 5.2 Mapping (per sample, integer/fixed-point arithmetic)
- **Slow axis → row**
  - `row = (x − x_min)·H / (x_max − x_min)`;
  - `x_min/x_max` = the slow waveform's commanded range on the image part of the scan;
  - samples on the slow flyback (x decreasing for a sawtooth) are discarded.
- **Fast axis → column**
  - `y_cal = a·y + b` converts sensor readings to command units (calibrated in Phase 6 of the TODO);
  - `col = (y_cal − y_min)·W / (y_max − y_min)`, where `y_min/y_max` = the commanded fast range, with
    an optional margin;
  - samples outside the range are discarded and counted.
- **Delay**
  - The detector value `v[i]` is paired with the position `pos[i − d]`, d configurable (fractional,
    by linear interpolation);
  - default d = 0.5: the FPGA integral covers the period *before* the sample, while the positions are
    taken at its end.
- **Value:** `v / (DEC_FACTOR − 1)` gives the mean detector level per sample (with `INTEGRATOR_DEC = 1`).
- **Direction:**
  - the sign of the fast sensor's derivative (smoothed over a few samples) gives forward/backward;
  - modes: `sum` (default), `forward`, `backward`, `both` (two images per frame, for alignment
    debugging).

### 5.3 Binning and frames
- Per pixel: `sum` (int64 or double) + `count` (uint32). On finalise, `img = sum/count` as float32, NaN
  where count = 0.
- **Frame end:**
  - the slow command wraps (a jump of more than half the range) or reaches its end;
  - cross-check against the expected number of samples per frame from the scan parameters;
  - the first, partial frame after start is discarded.
- Frame pool: 3 buffers (writing / ready / sending). Handover is a pointer swap under a mutex.

### 5.4 Performance budget (to be measured early)
- At `DEC_FACTOR = 50`: 2.5 M records/s, i.e. about 260 CPU cycles per record on one A9 core
  (667 MHz).
- Aim: integer-only inner loop, precomputed scale factors, no division per sample.
- If it's too slow: bigger `DEC_FACTOR`, NEON, or splitting the work across the second core.

## 6. TCP protocol (single connection, port 5000)
Every message starts with a header (little-endian):
```c
struct msg_hdr { uint32_t magic; /* 'RPSI' */ uint16_t version; uint16_t type; uint32_t length; };
```
| Type | Direction | Payload |
|---|---|---|
| `CMD` | PC → RP | ASCII command line, e.g. `set slow.freq 3`, `start`, `stop`, `get status`, `get config` |
| `REPLY` | RP → PC | `OK [text]` or `ERR <code> <message>` (one reply per command) |
| `FRAME` | RP → PC | `frame_hdr` + pixel data |
| `STATUS` | RP → PC | key=value text: state, records/s, dropped chunks/frames, ring fill, CPU, time-to-ack |

```c
struct frame_hdr {
  uint32_t frame_id;   uint16_t width, height;
  uint8_t  dtype;      /* 0 = float32 */   uint8_t  n_images; /* 1, or 2 for fwd+bwd */
  uint16_t flags;      /* DROPPED_DATA, PARTIAL, ... */
  uint64_t t_start_ns, t_end_ns;   uint32_t n_records, n_discarded, n_empty_pixels;
  float    slow_freq_actual, fast_freq_actual;
  float    roi[4];      /* x0 x1 y0 y1 the frame was binned with */
};   /* 64 bytes, see PROTOCOL.md */
```
Text commands make debugging easy (you can talk to the server with `nc`). Frames are binary to keep them
small.

## 7. Configuration (settable while IDLE; `start` validates and applies)
| Key | Meaning | Example |
|---|---|---|
| `slow.shape` | `sawtooth` (rise fraction) / `triangle` | sawtooth, 0.975 |
| `slow.amp`, `slow.offset`, `slow.freq` | command units (int16) or volts (TBD) | |
| `fast.shape`, `fast.amp`, `fast.offset`, `fast.freq` | sine / triangle | sine, 1 kHz |
| `wfm.len1`, `wfm.len2` | waveform lengths (otherwise chosen automatically for an exact frequency) | |
| `acq.dec`, `acq.int_dec` | acquisition and integrator decimation | 50, 1 |
| `img.width`, `img.height` | resolution | 512 × 512 |
| `img.bidir` | sum / forward / backward / both | sum |
| `cal.fast_gain`, `cal.fast_offset` | sensor → command calibration | 1.0, 0 |
| `cal.delay` | position–detector delay in samples (fractional) | 0.5 |
| `img.roi` | image window `x0 x1 y0 y1`, fractions of the full slow/fast range; live (applied at the next frame). Maps to `recon_cfg.x_lo..x_hi`, `y_lo..y_hi`; auto size scales with it. | 0 1 0.02 0.98 |

Defaults come from a config file (`/home/redpitaya/server2/server.conf`). The `get config` reply lists
the values actually used, including the quantised frequencies.

## 8. Error handling and robustness
- `SIGPIPE` is ignored. A client disconnect stops the scan safely (configurable) and the server waits for
  the next client.
- `SIGINT`/`SIGTERM`: stop acquisition, then the generator, then unmap and exit.
- IRQ timeout watchdog: if acquisition is running and no IRQ arrives for longer than expected, the
  status reports an error.
- Every hardware rule violation is an `ERR` reply, never a silent clamp.

## 9. Milestones
1. **M1 — hardware layer + CLI tool** (`rpsipm-ctl`): open the UIOs, read and write registers, load a
   waveform, grab one chunk, dump it. Checks the overlay, UIO and IRQ on the board.
2. **M2 — acq thread alone:** measure the maximum sustainable rate and time-to-ack; no data loss at
   the target `acq.dec`.
3. **M3 — recon on the PC with synthetic data** + unit tests (mapper, frame splitting, direction, delay).
4. **M4 — integration:** frames over TCP + minimal Python viewer; first run with `INPUT_SEL = 0`
   loopback.
5. **M5 — electrical tests** (IN1 pattern, fast sensor) and the calibration routine.
6. **M6 — deployment:** start-up script, docs.

## 10. Decisions (Martin, 2026-10-27)
1. Folder `server2/`. The old `server/` stays untouched as a reference.
2. **Config in volts.** Internally everything is int16 counts. The conversion happens only in the config
   layer, using per-channel scale factors (see §11).
3. Bidirectional default = **sum** both directions.
4. Pixels as **float32 mean**. The per-pixel count image is optional (off by default).
5. **A few frames per second.** Resolution up to 1080p, or whatever 1 Gbit/s allows. The real limit is
   the sample rate, see §12.
6. **A client disconnect stops the scan.**

## 11. Volt conversion (to confirm against hardware)
- RP outputs: ±32767 counts ≈ ±1 V at the RP connector (README). The RP inputs are assumed to work the
  same way, at ±1 V (LV jumper).
- The client `image_test_2.py` also divides by `factor = 10.75` (slow, OUT1) and `20` (fast sensor) when
  computing limits. That looks like an external amplifier/driver gain and a sensor scale.
  **Martin: confirm what these are.**
- Proposed keys:
  - `out1.v_per_count`, `out2.v_per_count`, `in2.v_per_count`: the RP-level scale, default 1/32767;
  - `slow.ext_gain` (default 10.75) and `fast.ext_gain` (default 20): the external gain;
  - all user-facing amplitudes and offsets in **volts at the galvo/driver side**. These are converted to
    counts once at `start`, and the result is reported back in `get config`.

## 12. Resolution vs. samples
The number of samples per frame limits what one frame can contain, more than the network does:

| Quantity | Formula | Example (`acq.dec = 50`, slow 3 Hz, fast 1 kHz, bidirectional) |
|---|---|---|
| sample rate | 125 MHz / `acq.dec` | 2.5 MS/s |
| samples per frame | rate / f_slow | 833 k |
| lines per frame | 2·f_fast / f_slow (bidir) | 667 |
| samples per line | rate / (2·f_fast) | 1250 (bunched at the turn-around points, because the fast axis is a sine) |

- So a 1920×1080 image (2.07 Mpx) would be mostly empty at these settings. The vertical resolution can't
  exceed the number of lines per frame.
- Network: 1920×1080×4 B = 8.3 MB/frame. At 3 fps that is 25 MB/s ≈ 200 Mbit/s, fine for 1 Gbit/s. The
  network is not the bottleneck.
- Plan:
  - `img.width`/`img.height` default to `auto`: height = lines per frame (one slow-axis sweep), width ≈
    samples per line × a usable fraction, capped at 1920×1080;
  - the user can override with fixed values (empty pixels = NaN, optional fill/interpolation later);
  - more pixels need a lower `acq.dec` (more samples; limited by CPU, see §5.4) or a slower slow axis.

## 13. Fast-axis resolution is limited by the position-sensor ADC (found 2026-10-27)
The fast position comes from IN2, a 14-bit ADC. `input_selector.v` shifts it left by 2, so the sensor
value moves in steps of **4 counts**.

**With the assumptions from `image_test_2.py`** (IN2 ±1 V, sensor scale 20 V/V, OUT gain 10.75; all
unverified):
- one sensor step = 4/32767 × 20 V ≈ **2.4 mV of galvo position**;
- the default fast scan (±0.12 V, 96 % window) spans 0.23 V, which is only **~94 distinct sensor
  values**;
- so a column narrower than one step stays empty in some lines (vertical NaN stripes).

`config_derive()` therefore caps the auto width at `fast.sensor_levels` (reported in `get config`).
Zooming the fast axis makes it worse (×2 zoom → 47 levels).

Ways to get more fast-axis resolution (decision needed; not implemented):
1. **Model-based position:** the fast drive is a known sine/triangle at a known frequency. Fit amplitude,
   phase and offset to the sensor over one or more periods (thousands of samples), and use the fitted,
   continuous position. Sub-LSB precision, robust to noise, and still a measurement of the real galvo.
2. **More analog gain into IN2** (smaller sensor scale), so the ±1 V range covers only the scanned
   range. Hardware change.
3. **Commanded fast position + measured lag/amplitude** (like the slow axis). Simplest, but it gives up
   the point of measuring the fast galvo.
