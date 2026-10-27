# RedPitaya_SiPM — project context for Claude

The workspace rules in `Work/CLAUDE.md` apply here too: ask when unsure, don't change existing code
without approval, commit only when asked, and get approval before editing any CLAUDE.md. This file
adds project-specific context. Last updated 2026-10-27.

## What this is
A galvo-mirror scanner on a **Red Pitaya STEMlab 125-14** (Zynq-7010). Two galvos scan a sample; a
scalar detector (photodiode/SiPM) is acquired synchronously with the galvo positions. **Images are
reconstructed on the Red Pitaya and only images are sent over TCP** to a PC viewer.

- Repo: private **`HorvatMa/RedPitaya_scan`** (SSH remote, branch `main`), fresh history from 2026-10-27.
  - The old repo's `.git` is kept locally as `.git-old-RedPitaya_SiPM/` (gitignored; old remote
    `martinhorvat/RedPitaya_SiPM`). Don't delete it.
  - Vivado build outputs are gitignored (`*.runs/ *.cache/ *.gen/ *.sim/ *.ip_user_files/`).
- Notes vault: project hub `Work/doc/redpitaya-scanner/Red Pitaya scanner.md` (TODO + journal links).
  Journal entries go in `Work/doc/journal/...` and are linked from that hub.

## Read first (detailed docs, kept up to date)
| File | Content |
|---|---|
| `documentation/FPGA_MANUAL.md` | Deployed FPGA design, register map, data path, device tree, live board checks (§8.5a), hazards (§11) |
| `documentation/SERVER_DESIGN.md` | server2 architecture, decisions (§10), volts conversion (§11), resolution vs samples (§12), fast-sensor resolution limit (§13) |
| `documentation/PROTOCOL.md` | TCP protocol v1 (commands, keys, 72-byte frame header, pixel formats) |
| `documentation/SERVER_TODO.md` | Phased TODO and **progress log** (benchmarks, findings) |
| `client/viewer/README.md` | Viewer usage |

## Layout — which tree is real
- **Deployed FPGA source = `vivado/myproj/project_1.xpr`** (Vivado 2020.2, top `rp_top`).
  `vivado/myproj/project_1.runs/impl_1/rp_top.bit` == `fpga/rp_top.bit` == `/home/redpitaya/fpga/fpga.bit` on the board.
  - `fpga/project/` is an older, incompatible design (Feb 2025). `vivado/sources_1`, `vivado/src` and
    `vivado/system.tcl` are a stale intermediate snapshot. **Don't edit these thinking you change the hardware.**
  - Vivado is **not installed** on this PC.
- `fpga/dts/fpga.dtso`: the overlay with UIO nodes `rp_acquire` and `rp_generate`. `scripts/apply_overlay.sh`
  loads the bitstream through `/dev/xdevcfg`, then the dtbo.
- `server/`: old C server (reference only; don't modify).
- **`server2/`: the new C server** (written 2026-10-27):
  - `src/recon/`: `scan.c` (waveforms, exact-frequency len/dec choice) and `recon.c` (delay pairing,
    slow-turn frame splitting, fast direction, binning; float inner loop);
  - `src/config.c`: all protocol keys, volts → counts, image geometry, auto size capped at the sensor levels;
  - `src/app.c`: state machine, acquisition thread → SPSC ring (4 M records) → reconstruction thread
    → 3-slot frame pool that drops old frames;
  - `src/net/`: TCP server on port 5000, one client, STATUS every 1 s;
  - `src/hw/`: `hw_fake.c` + `sim.c` (FPGA-faithful simulator, runs anywhere) and `hw_uio.c`
    (the real FPGA via UIO).
- `client/viewer/`: PyQt5 viewer (`viewer.py`), protocol module `rpsipm/protocol.py`, `mock_server.py`
  (`--throttle MBPS`), `test_viewer.py`.
- `client/src/`: the old Python client (`image_test_2.py` is the source of the default scan values and gains).

## Build / test
```sh
cd server2 && make            # build/rpsipm_server_fake and build/rpsipm_server (uio); Cortex-A9 flags added on the board
make test                     # reconstruction unit tests
make bench                    # throughput (board: 4.1-4.3 Mrec/s typical size, 3.2 at 1080p; acq.dec=50 needs 2.5)
~/.venv/base/bin/python tests/test_server.py                                    # end-to-end with the fake server
cd ../client/viewer && QT_QPA_PLATFORM=offscreen ~/.venv/base/bin/python test_viewer.py
~/.venv/base/bin/python viewer.py --host 127.0.0.1                              # with mock_server.py or rpsipm_server_fake
```
Python: `~/.venv/base` (PyQt5, numpy, matplotlib). On the board: gcc 9.4, Python 3.5.

## The board
- `ssh root@rp-f0a360.local` (key auth; mDNS sometimes doesn't resolve, the IP was 141.255.216.211).
  RP OS 1.04 (Ubuntu 16.04, kernel 4.9).
- The dev copy of server2 lives in `/root/server2_dev/` (copy with `tar | ssh`, then build there).
  Deployed files are in `/home/redpitaya/` (`apply_overlay.sh`, `fpga/`, old `server/`).
- Boot files are on the read-only FAT partition `/opt/redpitaya` (`devicetree.dtb`, `u-boot.scr`).
  The backup of the original dtb is in `documentation/board_backup/`.
- **A boot-DT change with `no-map` (reserve 512 KiB at `0x1900_0000`) stopped the kernel from booting**;
  it was restored from the SD card. Don't change boot files without approval. If retrying: no `no-map`,
  serial console connected.
- Ethernet links at **100 Mbit/s** (measured 11.8 MB/s); the Zynq supports 1 Gbit, so the cable/port
  is suspect.
- Don't run the RP web apps with our bitstream loaded (the base DT's XADC driver points at `0x83C0_0000`,
  which doesn't exist in our design).
- **Loading the bitstream or starting a scan drives OUT1/OUT2: make sure the galvos are disconnected or
  safe, and ask Martin first.**

## Hardware rules (enforced in `hw_uio.c`; never bypass)
- `CTRL` (0x20) is only ever written as **3**. Writing 0/1 disables the interrupt request and the DMA
  runs past its 512 KiB buffer into Linux RAM.
- Waveform 2 holds **≤ 12288 samples**; longer tables alias onto the control registers.
  Waveforms are written as full 32-bit words only (narrower writes are silently ignored).
- `DEC_FACTOR ≥ 2` (0 makes acquisition impossible to stop), `FIFO_MIN_THRESH` stays 15.
- The DMA buffer `0x1900_0000` is **not reserved** in Linux (ordinary RAM): keep hardware runs short and
  reboot afterwards until this is fixed.

## Decisions (Martin, 2026-10-27)
- Reconstruct on the RP, send only images; complete clean C rewrite (`server2/`; the old `server/` stays).
- Slow axis = commanded value (galvo assumed to follow). Fast axis = measured sensor (IN2, `INPUT_SEL = 1`).
- Config in **volts at the galvo/driver side**; gains `hw.slow_gain` / `hw.fast_gain` = 10.75 and
  `hw.sensor_gain` = 20 come from the old client (meaning **unconfirmed**). ±1 V ↔ ±32767 counts (unverified).
- Bidirectional default = sum; pixels float32 by default, `img.dtype` float32/uint16/uint8 (live, per-frame scaling).
- A client disconnect stops the scan. `img.roi` is the image window (fractions; live).
  The viewer offers crop / zoom-scan / zoom-out / full field.
- Frame ids are assigned at reconstruction and restart at 0 every run, so a gap means frames dropped
  on a slow link.

## Assumed wiring (from the code; README says otherwise — confirm with Martin)
OUT1 = slow galvo, OUT2 = fast galvo, IN1 = detector (integrated as 16383 − raw), IN2 = fast-galvo
position sensor (`(8192 − raw) << 2`). The ADC number format and polarity are unverified.

## Status and next steps (2026-10-27)
- Done:
  - FPGA analysis and docs;
  - server2 complete with the fake backend; all tests pass on the PC and on the board;
  - `hw_uio.c` written; it builds on the board and refuses to run without the overlay;
  - the viewer works against the C server.
- **Next: electrical loopback test** (`server2/tests/hw_loopback.py`), steps in the project hub.
  Galvos disconnected; OUT1 → IN1; signal generator (sine ~500 Hz, ±0.4 V, High-Z) or OUT2 → IN2.
  Expected: a vertical gradient. The fit of pixel vs OUT1 voltage gives the IN1 polarity, scale and
  ADC linearity. Needs Martin's go and an applied overlay; reboot afterwards.
- Open decisions:
  - the DMA buffer fix (boot-DT reservation without `no-map`, or move the buffer to `0x1E00_0000`,
    which needs an FPGA rebuild);
  - the **fast-axis resolution**: the 14-bit sensor gives only ~94 levels at the default scan.
    Options: sine fit to the sensor (recommended), more IN2 gain, or the commanded position;
  - the meaning of the gains 10.75 / 20; the cabling.
- FPGA issues known but not fixed (need a Vivado rebuild):
  - timing not met (WNS −4.45 ns);
  - WLAST depends on WREADY;
  - `DEC_FACTOR = 0` makes acquisition impossible to stop;
  - 16/17-bit address alias;
  - interrupt-threshold underflow.
