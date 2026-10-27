# RedPitaya_SiPM — new server: TODO

Created 2026-10-27. Background on the FPGA and device tree: [FPGA_MANUAL.md](FPGA_MANUAL.md).

## Decisions (from Martin, 2026-10-27)
- **The image is reconstructed on the Red Pitaya.** Only finished images go over TCP (no raw stream for
  normal use).
- **Complete rewrite of the server in C**, aiming for clean, modular code. The old `server/` stays as a
  reference until the new one works.
- **Scan model**
  - **Slow axis:** always generated. The galvo is *assumed to follow the command exactly*, so its
    position = commanded value (x slot of the FPGA record = OUT1 command).
  - **Fast axis:** the *actual* position of the fast galvo is measured by its position sensor
    (y slot, `INPUT_SEL = 1` → IN2). The fast galvo is also driven by the generator (OUT2).
  - **Detector:** scalar (photodiode/SiPM), integrated by the FPGA (IN1 → integral slot).
- **Test setup:** SSH access to the board is still to be checked by Martin. **The board cannot image
  anything**, so all tests use electrical signals (loopback, function generator, galvo sensor) and
  synthetic data. Real imaging tests are done by Martin.

Legend: **(M)** = needs Martin / hardware, otherwise Claude. Ask before modifying existing code
(workspace rule). The new server goes in a new folder (proposed `server2/`, to be confirmed).

---

## Progress log
- 2026-10-27: Design in [SERVER_DESIGN.md](SERVER_DESIGN.md) (v0.2). `server2/` created with the
  reconstruction core:
  - `src/recon/scan.c`: waveforms and exact-frequency choice of len/dec;
  - `src/recon/recon.c`: delay, slow turn detection, fast direction, binning, frames;
  - an FPGA-faithful simulator and tests (`make test`, all passing), plus a benchmark (`make bench`:
    65 Mrec/s on the PC, **board number still needed**).
- 2026-10-27: Network layer done in `server2`:
  - `config.c` (all PROTOCOL keys, volts → counts), `app.c` (threads, ring buffer, frame pool with
    dropping), `net/` (TCP, STATUS every 1 s) and the fake backend `hw_fake.c`;
  - `tests/test_server.py` passes: commands/errors, scan at 10 Hz, live ROI, a disconnect stops the scan,
    a slow client gets frames dropped, clean shutdown;
  - the viewer works against the real C server;
  - finding: the fast-sensor ADC limits the fast resolution to ~94 px at the default scan
    (SERVER_DESIGN §13).
- 2026-10-27: Benchmarks on the board (`make bench`, gcc 9.4):
  - first version: 1.3 Mrec/s (-O2) / 1.7 Mrec/s (-O3 NEON) at 1080p, too slow for `acq.dec = 50`
    (2.5 Mrec/s);
  - after the inner-loop optimisation (float history, precomputed constant delay interpolation), with
    identical test results: **4.3 Mrec/s at 94×324, 4.1 at 381×324, 3.2 at 1920×1080**, on one core;
  - the Makefile adds the Cortex-A9 flags automatically on the board. Unit tests pass on the board.
- 2026-10-27: **The board's Ethernet links at 100 Mbit/s** (`link up (100/Full)`); measured TCP
  board → PC is 94 Mbit/s = 11.8 MB/s. The Zynq GEM supports 1 Gbit, so check the cable (Cat5e/6),
  switch port or wall outlet **(M)**. Added `img.dtype` = float32|uint16|uint8 (live) with per-frame
  scaling, so 1080p fits about 5.7 fps in uint8 at 100 Mbit.
- 2026-10-27: Boot-DT reservation attempt (no-map, 512 KiB at `0x1900_0000`) → the kernel didn't boot.
  Martin restored `devicetree.dtb.orig`. Suspected cause: no-map on a non-1/2 MiB-aligned region.
  Retry without no-map, using the serial console.

## Phase 0 — Prerequisites
- [x] SSH access works (`root@rp-f0a360.local`, key auth, 2026-10-27).
- [x] Board checks, results in FPGA_MANUAL §8.5a:
  - `uio_pdrv_genirq.of_id=generic-uio` is set, configfs overlays work, and `amba_pl`/`intc` symbols
    are present;
  - the deployed bitstream, dtbo and server on the board match the repo;
  - **the DMA buffer `0x1900_0000` is NOT reserved (it is ordinary Linux RAM).** `0x1E00_0000–0x1FFF_FFFF`
    is outside Linux.
- [ ] **(M) Decide the DMA buffer fix** (FPGA_MANUAL §8.5a options):
  1. move it to `0x1E00_0000` (needs Vivado 2020.2 for a rebuild; not on this PC);
  2. reserve `0x1900_0000` in the boot DT;
  3. reuse `labuf`.
  Until then, keep acquisition runs short and treat any board instability as suspect.
- [ ] **(M)** Approve applying the overlay on the board (`apply_overlay.sh`), so the UIO nodes and
      interrupt 62 can be verified. It loads the bitstream and sets the DACs to 0 V, so disconnect the
      galvos or make sure they're safe.
- [x] Toolchain: build on the board (`gcc`, `make`, `dtc` available; RP OS 1.04, Ubuntu 16.04,
      kernel 4.9). Check `gcc --version` before using C11 features.
- [ ] Don't run RP web apps with our bitstream loaded (the base DT's XADC driver points at
      `0x83C0_0000`, where nothing exists in our design).
- [ ] **(M)** Put `vivado/myproj` (the source of the deployed bitstream) under git.
- [ ] **(M)** Confirm the cabling, and fix the README if it's wrong. Current understanding:
  - IN1 = detector, IN2 = fast-galvo position sensor;
  - OUT1 = slow galvo, OUT2 = fast galvo.
- [ ] On the board: DC sweep on IN1/IN2 to check the ADC number format, sign and scale (FPGA_MANUAL §4).

## Phase 1 — Specification (write before coding; Martin reviews)
- [ ] **Threads:**
  - control (TCP commands);
  - acquisition (wait for the interrupt → copy the chunk → acknowledge);
  - reconstruction (binning → frames);
  - sender (frames → TCP).
- [ ] **Queues between the threads:** a lock-free ring buffer for raw chunks, double/triple buffering for
      frames.
- [ ] **TCP protocol** (versioned, length-prefixed, little-endian, documented in a `PROTOCOL.md`):
  - commands as text or binary with a reply `OK`/`ERR <code> <msg>` (the old server sends no replies);
  - a separate data channel (or tagged messages) for frames;
  - a frame message: header (magic, version, frame number, width, height, data type, timestamp,
    number of samples, dropped/overflow flags, scan parameters) followed by pixel data;
  - a `status` command: running state, throughput, dropped data, FIFO-full flag (LED5 is not
    readable, so this may need an FPGA register, see Phase 7).
- [ ] **Configuration parameters:**
  - slow-axis waveform (shape, amplitude, offset, frequency);
  - fast-axis waveform;
  - acquisition and integrator decimation;
  - resolution (W×H);
  - image limits / calibration;
  - fast-axis delay;
  - pixel data type (e.g. float32 mean, or uint16 scaled).
- [ ] **Generator frequency quantisation:** choose the waveform length and divider so that
      f = 125 MHz/(dec·len) hits the target exactly. Report the actual frequency.

## Phase 2 — Hardware layer (`hw/`)
- [ ] UIO discovery by name (`rp_acquire`, `rp_generate`) via `/sys/class/uio`, plus mmap of
      map0 and map1 for each.
- [ ] A typed register API with built-in safety rules:
  - the control register is always written as `3` (never 0/1, which would let the DMA overrun RAM);
  - `DEC_FACTOR ≥ 1`, `DEC_GEN ≥ 1`, `INTEGRATOR_DEC ≥ 1`;
  - waveform 1 ≤ 16384 and waveform 2 ≤ **12288** samples (aliasing above that);
  - waveforms written only with full 32-bit stores (pack two int16 per word, no memcpy);
  - `FIFO_MIN_THRESH` left at 15;
  - `INPUT_SEL = 1` set at start-up.
- [ ] Clean start/stop:
  - start = generator first, then acquisition;
  - stop = acquisition off, drain/discard the FIFO and the pending chunk, generator off (OUT goes
    to 0 V, so the galvos jump to centre; consider ramping to centre first).
- [ ] The interrupt-wait loop must not hang on stop (poll with a timeout or an eventfd to wake it).

## Phase 3 — Acquisition thread
- [ ] Interrupt → read `BUFFER_N` → copy `BUFFER_N × 8 B` from the uncached buffer into the ring buffer
      → write control = 3 → re-enable the interrupt. Keep the time to acknowledge short: the FIFO only
      covers `16384·dec/125 MHz` (6.6 ms at dec 50).
- [ ] Measure: samples/s, chunk sizes, time to acknowledge. Count ring-buffer overflows (dropped
      chunks), and report them in frame headers.
- [ ] Check that copying the uncached memory is fast enough at the target sample rate. If not, use a
      larger decimation or NEON copies.

## Phase 4 — Reconstruction thread
- [ ] Unpack each record: x = slow command (int16), y = fast sensor (int16), v = integral (uint32).
- [ ] **Coordinate mapping:**
  - slow axis: command → row index via the known waveform limits (exact, by assumption);
  - fast axis: sensor value → column index via a calibration (offset/gain, configurable; see the
    calibration step below);
  - a fast-axis assignment of `fast_delay` samples between sensor and detector. This covers the
    half-sample skew inside the FPGA plus the electronic delay. Start at about 0.5 sample and make it
    configurable.
- [ ] **Bidirectional fast scan:** optionally separate forward and backward sweeps, using the sign of
      the derivative of the sensor signal. Sum them or keep them as separate images, for debugging
      misalignment.
- [ ] **Binning:** a sum array (double or int64) and a count array (uint32) per pixel. Output pixel =
      sum/count (NaN or 0 when count = 0, configurable). Optionally normalise by the integration length
      (`DEC_FACTOR`).
- [ ] **Frame boundaries:** one frame per slow-axis period. Detect the flyback/wrap of the slow
      command (a large jump), with sample counting as a cross-check. Discard the partial first frame
      after start.
- [ ] Swap to the next frame buffer and hand the finished frame to the sender. If the sender is busy,
      drop the oldest frame and count it.
- [ ] Measure CPU load at the target sample rate (Cortex-A9). Optimise with integer coordinate maths,
      precomputed scale factors and no float division per sample.

## Phase 5 — Sender / control threads
- [ ] TCP server: one client, reconnect handled cleanly, and the server must not crash on disconnect
      (SIGPIPE).
- [ ] Command parser for the protocol in Phase 1. Validate all inputs and reply OK/ERR.
- [ ] Frame sending: header followed by data, sent non-blocking or from its own thread so a slow network
      doesn't stall reconstruction.

## Phase 6 — Calibration without imaging
- [ ] **Fast-axis sensor ↔ command:** at low fast-axis frequency (where the galvo should follow the
      command), fit sensor = a·command + b. Use it to express sensor readings in command units, so both
      axes share the same coordinate system. Check the assumption against the frequency dependence
      **(M: confirm validity)**.
- [ ] **Position–detector delay:** without imaging, feed a known signal into IN1 that is synchronous to
      OUT2 (e.g. loop OUT2 → IN1). Fit the delay so that forward and backward sweeps line up. The final
      tuning is done by Martin on a real sample **(M)**.

## Phase 7 — FPGA improvements (optional; each needs Martin's approval)
- [ ] Readable registers for dropped-sample and FIFO-full counters, plus a sample counter.
- [ ] Optional: a frame-sync flag in the record (e.g. at slow-axis address 0) for exact frame boundaries.
- [ ] Fix the FPGA hazards:
  - `DEC_FACTOR = 0` makes acquisition impossible to stop;
  - the burst-end handshake can be lost if the memory port stalls;
  - the 17/16-bit address aliasing;
  - the interrupt-threshold wrap-around.
- [ ] Fix the timing violations (WNS −4.45 ns).

## Phase 8 — PC client (minimal)
- [ ] Python client:
  - connect and send the configuration;
  - receive frames and display them live (matplotlib/pyqtgraph);
  - save frames with metadata (`.npz` or HDF5).
- [ ] Reuse waveform helpers from `client/src/image_test_2.py` where sensible.

## Phase 9 — Testing (no imaging possible on this board)
- [ ] **Offline on the PC:**
  - a synthetic record generator (sawtooth slow command, sine fast sensor with optional delay/noise,
    a synthetic "sample" pattern for the detector);
  - unit tests for mapping, binning, frame splitting, forward/backward separation and the protocol;
  - build the reconstruction core so it also compiles on the PC.
- [ ] **On the board, electrically:**
  - `INPUT_SEL = 0` loopback: the fast position equals the OUT2 command, so geometry and frame logic
    can be checked exactly;
  - IN1 driven from OUT2 or a function generator, giving a known stripe/gradient pattern in the image;
  - the real fast galvo with its sensor connected, if available **(M)**.
- [ ] **Stress and endurance:** maximum sample rate without loss, CPU load, a 1 h run, repeated
      start/stop, client disconnect/reconnect.
- [ ] **(M)** Real imaging test on the microscope setup.

## Phase 10 — Deployment and docs
- [ ] Makefile (`-O2 -Wall -Wextra`), optionally a cross-compile target.
- [ ] Start-up script: apply the overlay (bitstream + dtbo), then start the server. Optionally a systemd
      unit **(M: approve)**.
- [ ] Update README, `PROTOCOL.md`, and `FPGA_MANUAL.md` if the FPGA changes.
