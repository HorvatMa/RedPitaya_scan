# RedPitaya_SiPM — FPGA reference manual

Reference for whoever (human or Claude) next touches the FPGA design of this project.
Written 2026-10-27 from reading the HDL, block design, IP configurations, build logs, device-tree
overlay, C server and Python client. **Nothing here was tested on hardware.** Statements marked
*(code reading)* follow from the source but were not checked against the running board.

Board: Red Pitaya STEMlab 125-14 (Zynq-7010, `xc7z010clg400-1`). Vivado 2020.2.

---

## 0. TL;DR

* **The deployed bitstream is built from `vivado/myproj/project_1/`**, which is **not tracked in git**.
  `vivado/myproj/project_1.runs/impl_1/rp_top.bit` is byte-identical to `fpga/rp_top.bit`
  (built 2025-06-09 13:43, Vivado 2020.2, top module `rp_top`). The project was originally created at
  `C:/Users/pinceta/Documents/RedPitaya_SiPM/fpga/tcl/myproj/`.
* The tree that *is* tracked in git (`fpga/project/`, last HDL commit 2025-02-28) is an **older,
  incompatible design** that does not match the current server or the device-tree overlay. Don't edit it
  thinking you are changing the deployed FPGA.
* What the FPGA does:
  1. **Generator:** two arbitrary waveform generators (OUT1, OUT2) that play int16 tables from BRAM,
     each with its own length and clock divider. They drive the galvo mirrors.
  2. **Acquisition:** every `DEC_FACTOR` clock cycles (125 MHz) it records one 64-bit sample
     `{int16 x, int16 y, uint32 integral}` into a 16384-deep FIFO:
     * x = value commanded on OUT1,
     * y = value commanded on OUT2, or the digitised IN2 when `INPUT_SEL = 1`,
     * integral = trapezoidal integral of IN1 (detector) over the sample period.
  3. **DMA:** it moves the FIFO contents into DDR at `0x1900_0000` with 16-beat AXI bursts through
     S_AXI_HP0. When enough data is in DDR it raises an interrupt (IRQ_F2P[1]). Linux reads the data
     through UIO.
* Software interface: an AXI4-Lite register block at `0x4000_0000` (GP0). The waveform BRAMs are
  written through the same block at offset `0x2000`.
* Main hazards (details in §11):
  - Writing `CTRL_REG` with bit 1 = 0 lets the DMA run past the 512 KiB buffer and corrupt DDR.
  - Waveform 2 holds at most **12288** samples. Samples 12288 and up alias onto the control
    registers.
  - Setting `DEC_FACTOR = 0` makes acquisition impossible to stop.
  - Timing is not met (WNS −4.45 ns).
  - Only full 32-bit writes are accepted. Narrower writes are silently ignored.

---

## 1. Repository map (what is what)

| Path | Status | Content |
|---|---|---|
| `vivado/myproj/project_1.xpr` | **DEPLOYED, untracked** | Vivado project that produced `fpga/rp_top.bit` |
| `vivado/myproj/project_1.srcs/sources_1/new/` | deployed | `rp_top.sv` (top), `rp_pll.sv`, `input_selector.v`, `generator_test.v`, `vector_split.v` |
| `vivado/myproj/project_1.srcs/sources_1/imports/src/` | deployed | `reg_ctrl_top.v`, `waveform_top.v`, `generate_top.v`, `acquire_top.v`, `integrator_top.v`, `AXI_HP_top.v`, `axi_control.sv`, `axi_s2mm.sv`, `oscilloscope.sv`, plus unused/empty `counter.sv`, `cnt.sv`, `generator.sv`, `gen.sv`, `osc.sv`, `generate.v`, `acquire.v` |
| `vivado/myproj/project_1.srcs/sources_1/bd/system/system.bd` | deployed | Block design (JSON), dated 2025-06-09 |
| `vivado/myproj/project_1.srcs/sources_1/ip/` | deployed | `blk_mem_gen_0`, `fifo_generator_64_64`, `reg_ctrl` (AXI BRAM ctrl), `FIFO_16_64` (unused now) |
| `vivado/myproj/project_1.srcs/constrs_1/imports/tcl/red_pitaya.xdc` | deployed | Pinout + clock constraints |
| `vivado/sources_1/`, `vivado/src/`, `vivado/ip/`, `vivado/system.tcl`, `vivado/red_pitaya.xdc` | stale snapshot (≈May 2025), untracked | Halfway between old and deployed. `system.tcl` lacks `input_selector`. `waveform_top` there leaves `bram_din`/`bram_clk` undriven. **Do not use.** |
| `fpga/project/` | old, tracked | Feb 2025 design: waveforms via GP1 + `axi_bram_ctrl`, 16-bit integrator, generator registers unreachable. Does **not** match the server. |
| `fpga/rp_top.bit` | tracked | Deployed bitstream (= `vivado/myproj/.../rp_top.bit`) |
| `fpga/dts/fpga.dtso`, `compile_dt.sh` | tracked | Device-tree overlay (matches the deployed design) |
| `scripts/apply_overlay.sh` | tracked | Loads the bitstream and overlay on the board |
| `server/` | C server on the RP (port 5000) | |
| `client/src/` | Python client (`image_test_2.py` = reference usage) | |
| `documentation/*.pdf` | Slovene docs (not reviewed for this manual) | |
| `PCB/` | KiCad `SHG_amp` amplifier board | unrelated to the FPGA |

> If you change the FPGA: work in `vivado/myproj/project_1.xpr`, and ask the user first to put it under
> version control (e.g. `write_project_tcl` + sources), because right now it exists only as an
> untracked directory.

---

## 2. Top-level architecture

```
                 adc_clk_i (125 MHz from ADC, LVDS)
                        │ IBUFDS
                        ▼
        ┌───────────── adc_clk_in ───────────────┐
        │                                        │
   rp_pll (PLLE2, in rp_top)               clk_wiz_0 (MMCM, in BD)
   dac_clk_1x 125 / 2x 250 / 2p 250@-45°    clk_125  ──► ALL PL logic, HP0, AXI slave side
   ──► DAC ODDRs                            clk_62_5 ──► M_AXI_GP0 + interconnect + proc_sys_reset

 IN1 adc_dat_i[0] ─►2 FF (clk_125)─► adc_data_ch1 ─► integrator_top ─────────────┐ integral (32b)
 IN2 adc_dat_i[1] ─►2 FF (clk_125)─► adc_data_ch2 ─► input_selector ─┐            │
                                                                      │ y (16b)   │
   reg_ctrl_top  (AXI4-Lite @0x4000_0000)                             │           │
     ├─ acq regs ──────────────► acquire_top (decimator → trig) ──────┼── trig ───┤
     ├─ gen regs ──► generate_top ──► dac_val_1 ─► OUT1 (dac_dat_a)  ─┼─ x (16b) ─┤
     │                 ▲   │       └► dac_val_2 ─► OUT2 (dac_dat_b) ──┘(if sel=0) │
     └─ wfm window ─► waveform_top (2× BRAM 32b-write/16b-read)                    ▼
                                                     AXI_HP_top: axi_control (FIFO 64b×16384,
                                                     burst/IRQ logic) + axi_s2mm (AXI master)
                                                          │ S_AXI_HP0          │ intr
                                                          ▼                    ▼
                                                     DDR @0x1900_0000     IRQ_F2P[1]
```

Block-design cells (deployed `system.bd`): `processing_system7`, `axi_interconnect_0` (1 master, GP0 →
`reg_ctrl_top_0`), `clk_wiz_0`, `proc_sys_reset`, `reg_ctrl_top_0`, `waveform_top_0`, `generate_top_0`,
`acquire_top_0`, `integrator_top_0`, `input_selector_0`, `AXI_HP_top_0`, `xlconcat_0` (16-bit IRQ_F2P),
`xlconcat_1` (8-bit LEDs), constants. `generator_test_0` and `vector_split_0` are instantiated, but their
outputs are unconnected, so synthesis removes them.

PS7 configuration: M_AXI_GP0 on, S_AXI_HP0 on (64-bit), fabric interrupts on, FCLK0..3 = 125/250/50/200 MHz
are exported to `rp_top` but not used by the user logic. `proc_sys_reset.ext_reset_in = FCLK_RESET2_N`,
`dcm_locked = clk_wiz_0.locked`. The resulting `peripheral_aresetn` is the `rst_n` of every custom block, and is
also exported as `rstn_out` → `rstn_0` in `rp_top` (DAC reset, PLL reset).

Address map (PS view):

| Segment | Base | Range | Notes |
|---|---|---|---|
| `reg_ctrl_top_0/s_axi_reg` | `0x4000_0000` | 256 MiB | IP decodes only 17 bits; `reg_ctrl_top` uses 16 → aliases every 64 KiB |
| HP0 → DDR (master `AXI_HP_top_0`) | `0x0000_0000` | 512 MiB | DMA target is hard-wired to `0x1900_0000` |

---

## 3. Clocks and resets

| Clock | Source | Used by |
|---|---|---|
| `adc_clk_in` 125 MHz | ADC LVDS clock (`adc_clk_i`) | input of both `rp_pll` and `clk_wiz_0` |
| `clk_125` | `clk_wiz_0` CLKOUT1 (MMCM ×8 ÷8) | all custom logic, AXI-Lite slave side, HP0, ADC input registers in `rp_top` |
| `clk_62_5` | `clk_wiz_0` CLKOUT2 (÷16) | M_AXI_GP0, interconnect S-side, `proc_sys_reset` |
| `dac_clk_1x/2x/2p` | `rp_pll` (PLLE2 ×8; ÷8, ÷4, ÷4 −45°) | DAC data/WRT/SEL/CLK ODDRs |

* Both clock generators run from the same ADC clock, so the domains are related. The
  `clk_125 → pll_dac_clk_1x` crossing (`dac_dat_*_o` registers in `rp_top`) is timed, and it **fails**
  (§10).
* `rp_pll` also has `clk_adc`, `clk_ser` and `clk_pdm` outputs. They are unconnected.
* `rstn_0` (from BD) resets the PLL (`RST = !rstn`) and drives `dac_rst <= ~rstn_0 | ~pll_locked`.
  While the PS holds reset, the DAC is held in reset.

---

## 4. Analog I/O conventions

### DAC (OUT1 = `dac_dat_a` = generator channel 1, OUT2 = `dac_dat_b` = channel 2)
* The generator produces signed 16-bit values. `rp_top` registers
  `dac_dat_x_o = {d[15], ~d[14:2]}` (Red Pitaya classic convention) on `dac_clk_1x`, so only the
  **top 14 bits** reach the DAC. The two channels are interleaved by an ODDR (`D1 = b`, `D2 = a`).
* The README states ±32767 ↔ ±1 V. The client scales with 32767. Not verified here.
* When `START_GEN = 0` both DACs output **0** (mid-scale). The galvos jump to centre when the generator
  stops.

### ADC (IN1 = `adc_dat_i[0]`, IN2 = `adc_dat_i[1]`)
* `rp_top` takes bits `[15:2]` (14 bits), registers them twice on `clk_125`, and passes them on
  **without format conversion** (`{r[13], r[12:0]}` is the identity).
* **IN1 → integrator:** uses `x = 16383 − r` (14-bit, as unsigned 0..16383).
* **IN2 → input_selector:** `y = {(8192 − r) mod 2^14, 2'b00}`, i.e. scaled to 16 bits, as int16.
* The code was written so that these maps give "more signal = larger number". Whether the raw ADC word
  is offset-binary or two's complement (and so whether these maps are linear across the full range)
  was **not verified**. Check with a DC sweep on hardware before trusting absolute values or the sign
  around mid-scale.

---

## 5. Module reference (deployed sources)

### 5.1 `reg_ctrl_top` (`imports/src/reg_ctrl_top.v`)
* Wraps the `reg_ctrl` IP: an AXI BRAM Controller, **AXI4-Lite**, 32-bit data, `MEM_DEPTH 32768`
  (17-bit byte address), single port. Its BRAM port (`en/we[3:0]/addr/wrdata/rddata`) is decoded as
  registers.
* `S_AXI_REG_ADDR_BITS = 16`: the IP's 17-bit `bram_addr_a` is truncated to 16 bits, so bit 16 is lost
  (the alias described in §11).
* A write is accepted only when `reg_en && reg_we == 4'hF`: **full 32-bit writes only.**
* Register read: `reg_rd_data` is registered on `clk` from `reg_addr` (1-cycle latency, which matches
  IP `READ_LATENCY = 1`). Addresses not in the read `case` return `wfm_read` (BRAM port-A output), not 0.
* The waveform write path is combinational from the IP's BRAM port:
  `set_wfm = rst_n & (reg_addr >= 0x2000) & full_write`,
  `wfm_addr = reg_addr[15:2] − 0x800` (14-bit word index), `reg_wr_data` → `waveform_top.bram_din`.
* `CTRL_REG[0]` self-clears one cycle after being written. Every other bit holds its written value.
* `req_int = CTRL_REG[1]` (also shown on LED4).

### 5.2 `waveform_top`
* Two `blk_mem_gen_0` instances: **True Dual Port**. Port A is 32-bit × 8192 (13-bit address, write +
  readback); port B is 16-bit × 16384 (14-bit address, generator read, `web = 0`). Always enabled,
  WRITE_FIRST, primitive output registers on. Both clocks = `clk_125`.
* Port A address = `wfm_addr[12:0]`. `wfm_addr[13]` selects the RAM: 0 → RAM 1, 1 → RAM 2. RAM 1 covers
  word index 0..8191, RAM 2 covers 8192..14335. Only 6144 RAM 2 words are reachable (§11).
* One 32-bit word w holds samples `2w` (bits [15:0]) and `2w+1` (bits [31:16]). This is the usual Xilinx
  asymmetric-port ordering, so a little-endian `int16[]` copied with 32-bit stores lands in the right
  order. (Not verified on hardware.)
* `bram_read` (port-A douta of the selected RAM) feeds back to `reg_ctrl_top` for readback. Readback
  latency is BRAM latency plus one register, which is more than the controller's assumed 1 cycle.
  **Waveform readback via AXI is probably stale/shifted. Don't rely on it** *(code reading)*.
* `n_write` and `bram_dout_valid` are leftovers. `bram_dout_valid` is connected but ignored by
  `generate_top`.

### 5.3 `generate_top`
Per channel i ∈ {1, 2}:
```
advance_i = (count_i >= decimation_i)
count_i   : 0 while !start_gen; else (advance ? 1 : count+1)
addr_i    : 0 while !start_gen; else if advance: (addr == len_i-1 ? 0 : addr+1)
dac_val_i = start_gen ? bram_dout_i : 0
```
* Step period = `max(decimation_i, 1)` cycles, so the sample rate is `125 MHz / max(dec,1)`.
* **Waveform frequency `f = 125 MHz / (dec · len)`.** Both are integers. Example from the client:
  len 3000 with a 1 kHz target gives `dec = int(125e6/3000/1000) = 41`, so the real frequency is
  **1016.3 Hz**. With a 3 Hz target, dec = 13888 and the real frequency is 3.0001 Hz.
* `len` register is 14 bits. Writing 16384 (the reset value) stores 0. With len = 0, `len−1` is computed
  at 32 bits and never matches, so the address wraps naturally at 16384. Lengths 1..16383 work as
  expected.
* Both channels start together on the rising edge of `START_GEN` (same register bit) from address 0.
  They are phase-locked at start and then run at their own rates.
* Output = BRAM port B (latency ≈ 1–2 cycles). Irrelevant in practice.

### 5.4 `acquire_top`
```
trig    = (dec_cnt >= cfg_dec)
dec_cnt : 0 while !start_acq; else (trig ? 1 : dec_cnt+1)
data_osc1 = adc_data_ch1 input  (BD: = dac_val_1, i.e. OUT1 commanded value)
data_osc2 = adc_data_ch2 input  (BD: = input_selector output)
data_detector = integrated_signal
```
* One `trig` pulse every `cfg_dec` cycles. Sample rate = `125 MHz / cfg_dec`. The first trig comes
  `cfg_dec+1` cycles after `START_ACQ`.
* `oscilloscope` is a pass-through.
* **`cfg_dec = 0` ⇒ `trig` is permanently 1, even when `START_ACQ = 0`** (because `0 >= 0`).

### 5.5 `integrator_top`
```
x            = {18'b0, 16383 - signal_in}          // IN1, 0..16383
activate     = (dec >= integrator_dec); dec: trig|activate ? 1 : dec+1
on trig:     integrated_signal <= 0; signal_old <= x
on activate: integrated_signal += (x + signal_old) >>> 1;  signal_old <= x
```
* Trapezoid rule on samples spaced `k = max(integrator_dec,1)` cycles apart. It is **not** multiplied by
  k, so the value is in units of ADC counts × (number of steps).
* With `integrator_dec = 1` (what the client uses), each sample period of `cfg_dec` cycles accumulates
  `cfg_dec − 1` trapezoids. The trig cycle itself only resets.
* The FIFO captures `integrated_signal` on the trig cycle, before the reset. So the stored value is the
  integral over the **preceding** period, while x/y are sampled **at the end** of that period. The pixel
  centre of the integral therefore lags the recorded position by about `cfg_dec/2` cycles. This matters
  for bidirectional scans.
* Overflow: the register is 32-bit signed and is sent as uint32. Wrap-around starts beyond
  ≈ 2³²/16383 ≈ 262 000 accumulations, i.e. about 2.1 ms per sample at `integrator_dec = 1`.

### 5.6 `input_selector`
`output = INPUT_SEL ? {8192 − IN2, 2'b0} : dac_val_2`. It feeds `data_osc2`, the y slot of the stream.
The server sets `INPUT_SEL = 1` at start-up (`server.c:126`), so **by default y = measured IN2**, not
the OUT2 command.

### 5.7 `AXI_HP_top` = `axi_control` + `axi_s2mm`
**FIFO** `fifo_generator_64_64`: common clock, 64-bit × 16384, First-Word-Fall-Through, sync reset,
15-bit `data_count`.
* `din = {integral[31:0], y[15:0], x[15:0]}`, `wr_en = trig`. If the FIFO is full, the sample is
  dropped silently.
* Memory layout of one record (little-endian): bytes 0–1 = x (int16), 2–3 = y (int16),
  4–7 = integral (uint32). This matches `Buff_data` in `server/src/app/oscilloscope.h` and the client
  `record_dtype`.

**Burst engine** (all on `clk_125`):
* `initiate_transfer = (fifo_cnt > FIFO_MIN_THRESH) && !busy && !request_interrupt`.
* Burst length is fixed at 16 beats (`transfer_rd_cnt` always 15, `awlen = 15`, `awsize = 8 B`, INCR,
  `awcache = 0011`). One burst = 16 samples = 128 B.
* `awaddr = 0x1900_0000 + addr_shift`. `addr_shift += 128` after each burst; `buffer_n += 16` per
  started burst.
* `cnt` counts W handshakes; `last = (cnt == 15)` = `wlast`.
* `axi_s2mm` raises AWVALID and WVALID together, drops WVALID after the last beat, waits for BRESP
  (`resp`), then accepts the next burst.

**Interrupt / buffer protocol**:
* `request_interrupt = CTRL_REG[1] && (buffer_n > thresh)`. When true, it blocks new bursts, and
  `intr` goes to 1 as soon as no burst is in flight. So when the IRQ fires, all `buffer_n` samples are
  already in DDR.
* Writing `CTRL_REG[0] = 1` (one-cycle pulse) does the following at once:
  - `intr ← 0`, `buffer_n ← 0`, `addr_shift ← 0` (the next data goes to the start of the buffer again);
  - `thresh ← {fifo_cnt[14:4], 4'b0} − 1`, i.e. the next chunk is about the size of the current FIFO
    backlog, rounded down to 16;
  - `full_o ← full` (shown on LED5).
* Reset: `thresh = 0` and `CTRL_REG = 2`, so the first IRQ comes after the first 16 samples.
* **Underflow quirk:** if the backlog is < 16 samples at clear time, `thresh` wraps to 32767. The next
  chunk is then 32768 samples (256 KiB, ≈ 13 ms at `cfg_dec = 50`). A fast reader therefore gets the
  *largest* chunks. The buffer stays inside 512 KiB either way.
* The largest possible chunk is 32768 samples = 256 KiB, which is below the 512 KiB UIO buffer map.

**Expected software loop** (as in `server/src/app/transfer.c::do_work`):
```
loop:
  write(fd_uio, 1)            # unmask UIO IRQ
  read(fd_uio)                # blocks until intr rising edge
  n = REG[BUFFER_N]           # 0x28, number of 8-byte records
  send(buffer[0 : n*8])       # buffer = UIO map 1 = DDR 0x1900_0000
  REG[CTRL_REG] = 3           # clear (bit0) + keep requesting (bit1)
```
* Data-loss budget: the FIFO holds 16384 samples, i.e. `16384 · cfg_dec / 125 MHz` (6.6 ms at
  `cfg_dec = 50`) of time between the IRQ and the clear.
* Throughput = `8 B · 125 MHz / cfg_dec`, e.g. 20 MB/s = 160 Mbit/s at `cfg_dec = 50`.

---

## 6. Register map (base `0x4000_0000`)

All registers are 32 bits wide. **Only full 32-bit writes are accepted.**

| Offset | Name | Access | Reset | Function |
|---|---|---|---|---|
| `0x0000` | `DEC_FACTOR` (`cfg_dec`) | RW | 1 | Acquisition period in 125 MHz cycles. **Never 0.** |
| `0x0004` | `START_ACQ` | RW [0] | 0 | 1 = run acquisition decimator. Does not flush the FIFO. |
| `0x0008` | `DEST_ADDR` | RW | 0 | **Unused** (DMA address is hard-wired) |
| `0x000C` | `BUFF_SIZE` | RW | 1024 | **Unused** |
| `0x0010` | `TRANSFER_RD_CNT` | R | 15 | Burst length − 1 (constant 15) |
| `0x0014` | `FIFO_MIN_THRESH` | RW [3:0] | 15 | Burst starts when FIFO count > thresh. **Keep at 15**: a burst always reads 16 entries, so a lower value risks reading an empty FIFO. |
| `0x0018`/`0x001C` | `FIFO_DOUT_1/2` | — | — | Not decoded any more (reads return junk) |
| `0x0020` | `CTRL_REG` | W | 0x2 | bit0 = clear IRQ / restart buffer (self-clearing); bit1 = request IRQ (**must stay 1**). Not readable (returns junk). |
| `0x0024` | `INTEGRATOR_DEC` | RW | 1 | Integrator step in cycles |
| `0x0028` | `BUFFER_N` | R | — | Records in the DDR buffer since the last clear |
| `0x002C` | `INPUT_SEL` | W [0] | 0 | y slot: 0 = OUT2 command, 1 = IN2. Not readable. |
| `0x1000` | `START_GEN` | RW [0] | 0 | 1 = run both generators (restart from sample 0 on each 0→1) |
| `0x1004` | `DEC_GEN_1` | RW | 1 | OUT1 step period (cycles) |
| `0x1008` | `DEC_GEN_2` | RW | 1 | OUT2 step period |
| `0x100C` | `WFM_LEN_1` | RW [13:0] | 0 (=16384) | OUT1 table length (1..16383, 0 = 16384) |
| `0x1010` | `WFM_LEN_2` | RW [13:0] | 0 (=16384) | OUT2 table length (≤ 12288 usable, §11) |
| `0x2000`–`0x9FFF` | waveform 1 | W (R unreliable) | — | 16384 × int16, two samples per 32-bit word |
| `0xA000`–`0xFFFF` | waveform 2 | W (R unreliable) | — | **12288** × int16 |
| `0x1_0000`–`0x1_FFFF` | alias of `0x0000`–`0xFFFF` | | | bit 16 dropped |

Software views of this map:
* `server/src/app/oscilloscope.h::Reg_map_osc`: offsets `0x00`–`0x2C`, through UIO `rp_acquire` map 0.
* `server/src/app/generator.h::Reg_map_gen`: offsets `0x1000`–`0x1010`, through UIO `rp_generate` map 0.
* `Waveform_container`: `waveform_1[16384]` at `0x2000`, `waveform_2[16384]` at `0xA000`.
* `server/src/app/settings.h::Reg_map` is an older, unused copy.

---

## 7. Interrupts and LEDs

* `IRQ_F2P[1]` = `AXI_HP_top.intr` (level, held until cleared). All other IRQ_F2P bits are 0.
  IRQ_F2P[1] is GIC ID 62 = SPI 30, which is why the overlay has `interrupts = <0 30 1>` on `rp_acquire`.
  The `rp_generate` node's `<0 31 1>` (IRQ_F2P[2]) never fires.
* LEDs (`led_o[7:0]` via `xlconcat_1`):

| LED | Signal |
|---|---|
| 0 | `START_GEN` |
| 1 | `START_ACQ` |
| 2 | `busy` (HP burst in flight) |
| 3 | `intr` |
| 4 | `CTRL_REG[1]` (request enabled) |
| 5 | FIFO-full flag latched at last clear → **data was lost** |
| 6 | `INPUT_SEL` |
| 7 | constant 1 (bitstream loaded) |

* Expansion connector (`exp_p_io`/`exp_n_io`): **not used** by the deployed design (only pull
  constraints). The old pulse counter (`counter.sv`, `rp_expansion_to_in`) is no longer instantiated.

---

## 8. Linux side: device tree, UIO, loading

The workspace only has the **overlay** `fpga/dts/fpga.dtso`. The base device tree is on the board (see §8.5).
Cross-checked against `vivado/myproj/project_1.gen/sources_1/bd/system/hw_handoff/system.hwh`
(address map: `reg_ctrl_top_0/reg0` = `0x4000_0000–0x4FFF_FFFF` on M_AXI_GP0, HP0 sees DDR
`0x0000_0000–0x1FFF_FFFF`; IRQ_F2P mode DIRECT, bit 1 marked LEVEL_HIGH).

### 8.1 What the overlay says
```dts
/dts-v1/; /plugin/;
/ { fragment@uio { target = <&amba_pl>; __overlay__ {
    rp_acquire: rp_acquire@50000000 {            // unit address is cosmetic (and wrong)
        compatible = "generic-uio";
        reg = <0x40000000 0x1000>, <0x19000000 0x80000>;
        reg-names = "regset", "buffer";
        interrupt-parent = <&intc>;
        interrupts = <0 30 1>;                   // SPI 30, rising edge
    };
    rp_generate: rp_generate@50100000 {
        compatible = "generic-uio";
        reg = <0x40001000 0x1000>, <0x40002000 0x10000>;
        reg-names = "regset", "waveform";
        interrupt-parent = <&intc>;
        interrupts = <0 31 1>;                   // SPI 31 — nothing drives it
    };
}; }; };
```

### 8.2 Physical address → Linux object

| Physical range | HW behind it | DT node / reg index | UIO map | Server pointer |
|---|---|---|---|---|
| `0x4000_0000–0x4000_0FFF` | `reg_ctrl_top` acq regs (`0x000–0x02C`) | `rp_acquire` reg[0] `regset` | `/dev/uioA` map0 (mmap offset 0) | `Acq.reg` (`Reg_map_osc`) |
| `0x1900_0000–0x1907_FFFF` | DDR, written by the FPGA's HP0 DMA | `rp_acquire` reg[1] `buffer` | `/dev/uioA` map1 (offset 1·4096) | `Acq.buff` |
| `0x4000_1000–0x4000_1FFF` | `reg_ctrl_top` gen regs (`0x1000–0x1010`) | `rp_generate` reg[0] `regset` | `/dev/uioB` map0 | `Gen.reg` (`Reg_map_gen`) |
| `0x4000_2000–0x4001_1FFF` | waveform window (`0x2000–0xFFFF`) **+ 8 KiB alias onto `0x0000–0x1FFF`** | `rp_generate` reg[1] `waveform` | `/dev/uioB` map1 | `Gen.waveform_container` |
| GIC ID 62 (SPI 30) | `IRQ_F2P[1]` = `AXI_HP_top.intr` | `rp_acquire` `interrupts` | `read()/write()` on `/dev/uioA` | `wait()` in `oscilloscope.c` |
| GIC ID 63 (SPI 31) | `IRQ_F2P[2]`, tied to 0 | `rp_generate` `interrupts` | never fires | unused |

* Both `regset`s are **the same AXI slave**. The split at `0x1000` is only for software. The rest of the
  256 MiB GP0 window (`0x4000_0000–0x4FFF_FFFF`) aliases that slave every 128 KiB (and the register
  decoder every 64 KiB). No other PL device exists.
* IRQ numbering: on Zynq-7000, `IRQ_F2P[7:0]` = GIC IDs 61–68 and `IRQ_F2P[15:8]` = 84–91. The DT
  number is the SPI index = ID − 32. So `IRQ_F2P[1]` → ID 62 → `<0 30 …>` ✓.
* The trigger type differs: the overlay says edge (`1` = IRQ_TYPE_EDGE_RISING), the .hwh says level
  high. Either works, because `intr` stays high until software clears it and drops before the next
  chunk. With edge, a missed re-enable cannot cause an interrupt storm.

### 8.3 How the kernel turns this into `/dev/uioN`
1. `apply_overlay.sh` first loads the bitstream (`/dev/xdevcfg`). **This order matters:** if the CPU
   touches `0x4000_0000` with no PL slave configured, the GP0 access hangs the bus or the system.
2. The `.dtbo` is written to configfs (`/sys/kernel/config/device-tree/overlays/<name>/dtbo`). The kernel
   resolves `&amba_pl` and `&intc` using the base tree's `__symbols__`, so the overlay must be compiled
   with `dtc -@` (`compile_dt.sh` does this).
3. `compatible = "generic-uio"` is claimed by **`uio_pdrv_genirq`** only if it was told to match that
   string (kernel cmdline `uio_pdrv_genirq.of_id=generic-uio` or the module parameter). That this is
   set on this RP image is **unverified**. If it isn't, no `/dev/uio*` appears.
4. The driver creates one UIO device per node. The `name` is the DT node name without the unit address
   (`rp_acquire`, `rp_generate`). Each `reg` entry becomes `maps/mapN` (`addr`, `size`, `offset`). The
   server's `uio_parser.c` walks `/sys/class/uio/*/name` and `maps/map*/` to find them by name. The
   `uioN` numbers themselves are not fixed.
5. The server mmaps map N with `offset = N · getpagesize()` (UIO convention). UIO maps physical memory
   **non-cached**. That is what makes the DMA buffer coherent: HP0 writes bypass the CPU caches, so
   no cache flush/invalidate is needed, but CPU reads of the buffer are slow.
6. IRQ: `uio_pdrv_genirq` disables the line in its handler. Userspace re-enables with `write(fd, 1)`
   and blocks in `read(fd)` until the next event (the event count is returned). `wait()` does exactly
   this.

### 8.4 Issues in the overlay
* Unit addresses `@50000000` / `@50100000` don't match `reg` (`0x40000000` / `0x40001000`), and
  `fragment@uio` has a non-hex unit address. dtc warns, but it's harmless.
* `#address-cells/#size-cells` inside the leaf nodes have no effect. What matters is that `amba_pl` in
  the base tree uses 1/1 (standard on Zynq, not verified on this image).
* The `waveform` map is 64 KiB starting at `0x4000_2000`, so its last 8 KiB alias the register block
  (§11.2).
* `rp_generate`'s interrupt is fictitious. It's harmless, but it can be removed.
* **The `buffer` region (`0x1900_0000`, 512 KiB at the 400 MiB mark of the 512 MiB DDR) is ordinary RAM
  unless the base tree reserves it.** UIO maps it without claiming it, so if Linux allocates pages
  there, the DMA silently corrupts them. This needs checking on the board (§8.5). If it isn't reserved,
  add a `reserved-memory` node (no-map) or boot with `mem=400M`.

### 8.5a Live board — checked 2026-10-27 over SSH (`root@rp-f0a360.local`, key auth)
Fresh boot, no overlay applied at the time. Copy of the live tree: run `dtc -I fs -O dts /sys/firmware/devicetree/base` again
(not stored in the repo).

* **OS:** Red Pitaya ecosystem 1.04-93661995d, build 28 (`/root/.version` = 1.08), Ubuntu 16.04.7, kernel
  `4.9.0-xilinx` (2022). `gcc`, `make` and `dtc` are on the board, so the server can be built there.
* **Kernel cmdline:** contains `uio_pdrv_genirq.of_id=generic-uio`, so `generic-uio` nodes do become
  `/dev/uioN`.
* **Overlays:** configfs is present at `/sys/kernel/config/device-tree/overlays/`. `__symbols__` contains
  `amba_pl` (`/amba_pl`, `#address-cells = #size-cells = 1`) and `intc`
  (`/amba/interrupt-controller@f8f01000`), so `fpga.dtbo` resolves.
* **Deployed files** in `/home/redpitaya/`:
  - `apply_overlay.sh`;
  - `fpga/fpga.bit`, md5-identical to the repo's `fpga/rp_top.bit`;
  - `fpga/fpga.dtbo`, `fpga/dts/fpga.dtso` (identical to the repo), `fpga/led.bit`;
  - `server/`, whose sources are identical to the repo working tree.
  - The server was not running.
* **Memory (base DT + `/proc/iomem`):**
  - `memory` node = `0x0000_0000–0x1DFF_FFFF` (480 MiB, all "System RAM"). `0x1E00_0000–0x1FFF_FFFF`
    (32 MiB) is **not given to Linux**.
  - `reserved-memory`: CMA pool 16 MiB (dynamic, at `0x1D00_0000`); `buffer@1000000` 1.5 MiB
    (`streaming_reserved`); `labuf@a000000` 32 MiB (`rprx_reserverd`, used by RP's logic analyser).
  - **⇒ The DMA target `0x1900_0000–0x1907_FFFF` is ordinary Linux RAM, not reserved.** The FPGA can
    overwrite pages the kernel has handed out. This is a real corruption risk, now confirmed.
* **The base DT already describes the *stock* RP FPGA:**
  - `api@40000000` (generic-uio → `uio0` "api", maps `hk/acq/asg/pid/ams/…` = 8 × 1 MiB from
    `0x4000_0000`). It overlaps our registers. Our nodes will appear as `uio1`/`uio2`, and the server
    looks them up by name, so this is fine in practice. UIO does not claim the address range
    exclusively (expected, not verified).
  - `xadc_wiz@83c00000`: the AXI-XADC driver is bound at boot. **Our bitstream has nothing at
    `0x83C0_0000` (GP1 disabled).** Anything that reads that device after our bitstream is loaded
    (e.g. `/sys/bus/iio` reads by RP web apps) may hang the bus. Don't run RP web apps alongside.
  - Four disabled `axi-dma` nodes. One of them lists SPI 30 too, but it is disabled, so there is no
    conflict.

**Options to fix the DMA buffer** (each needs approval):
1. Move the buffer to `0x1E00_0000` (outside Linux, 32 MiB free). This changes the constant in
   `axi_s2mm.sv` (FPGA rebuild, Vivado 2020.2 is not installed on this PC) and `rp_acquire` reg[1] in
   `fpga.dtso`. Cleanest, and allows a bigger buffer.
2. Reserve `0x1900_0000` in the base DT (`/opt/redpitaya/devicetree.dtb` on the read-only FAT boot
   partition, or `mem=`). No FPGA change, but it modifies the board's boot setup.
3. Reuse an already-reserved region (`labuf@a000000`). This also needs an FPGA rebuild, and it collides
   with RP's logic analyser.

### 8.5 To read the live tree on the board
```sh
ls /sys/firmware/devicetree/base/ | head; ls /sys/firmware/devicetree/base/__symbols__ | grep -E 'amba_pl|intc'
dtc -I fs -O dts /sys/firmware/devicetree/base > /tmp/live.dts     # full live tree (incl. applied overlay)
grep -A8 reserved-memory /tmp/live.dts; cat /proc/cmdline; grep -i -E 'System RAM|reserved' /proc/iomem
cat /proc/cmdline | tr ' ' '\n' | grep uio; lsmod | grep uio
for u in /sys/class/uio/uio*; do echo $u $(cat $u/name); grep . $u/maps/map*/{addr,size}; done
grep -E '^ *(62|63):' /proc/interrupts || grep -i uio /proc/interrupts
```

### 8.6 Load sequence
`scripts/apply_overlay.sh <name>` (run on the RP with `./fpga/fpga.bit` and
  `./fpga/fpga.dtbo` relative to the current directory):
  `rm -rf /sys/kernel/config/device-tree/overlays/*` → `cat fpga.bit > /dev/xdevcfg` → mkdir overlay dir →
  write `dtbo`. `/dev/xdevcfg` is the older Xilinx loader interface, so the script depends on the RP OS
  version in use.
* Build the overlay with `fpga/dts/compile_dt.sh` (`dtc -@ -O dtb -o fpga.dtbo fpga.dtso`). The repo
  stores the bitstream as `fpga/rp_top.bit`, but the script expects `fpga/fpga.bit`, so rename/copy it
  when deploying.

---

## 9. Host usage (server/client)

TCP port 5000. The command set is in the README and `server/src/app/parser.c`: `set_waveform ch N` (followed by a
length-prefixed big-endian int16 payload), `set_gen_decimation ch D`, `start_gen`, `stop_gen`,
`set_acq_decimation D`, `set_int_decimation D`, `start_acq`, `stop_acq`, `set_lim`, `set_res`,
`set_frame_time`, `send_buffer`, `exit`.
Channel index 0 = OUT1 / waveform 1, 1 = OUT2 / waveform 2.

* `set_waveform` memcpy()s the int16 table into the UIO waveform map. glibc memcpy normally uses ≥ 32-bit
  stores, but **any byte/halfword store is dropped by the FPGA**. With an odd N, the last sample is
  probably lost *(code reading)*.
* While acquiring, the server streams raw 8-byte records (`x, y, integral`) continuously.
  `client/src/image_test_2.py` is the reference consumer. It bins records into an image using
  `set_lim` limits.

**Physical connections: README and code disagree.** Check the cabling.
* README: OUT1 = y galvo (fast), OUT2 = x galvo, IN1 = y-galvo position sensor, IN2 = detector.
* FPGA + client: **IN1 is integrated** (so IN1 must be the detector). **IN2 replaces the OUT2 slot**
  when `INPUT_SEL = 1`. The client drives **OUT1 at 3 Hz (sawtooth) and OUT2 at 1 kHz (sine)**.
* Consistent reading of the code: OUT1 = slow axis, OUT2 = fast galvo, IN2 = fast galvo position
  sensor, IN1 = detector. The README is probably out of date.

---

## 10. Build status / timing (deployed run)

From `vivado/myproj/project_1.runs/impl_1/`:
* Utilisation: 1137 LUT (6.5 %), 1633 FF (4.6 %), **43.5 / 60 BRAM tiles (72.5 %)**. Most of it is the
  16384×64 FIFO plus 2×(8192×32) waveform RAMs.
* **Timing NOT met.** WNS −4.451 ns, TNS −413 ns, 180 failing endpoints:
  - `clk_125` intra-clock: worst path `reg_ctrl_top/start_gen_reg` → the `dac_val` mux → FIFO BRAM `din`.
    It has one LUT level but 10.7 ns of routing (high fanout across a BRAM-heavy die). Fix: register
    `dac_val_*` (and/or `data_in`) before the FIFO, or replicate/pipeline `start_gen`.
  - `clk_125 → pll_dac_clk_1x`: −2.68 ns on `dac_dat_a_o_reg[*]` in `rp_top`. Fix: generate the DAC data
    in the PLL domain, or drive the custom logic from `rp_pll` clocks, as in the stock RP design.
  - The board apparently works anyway. Expect occasional corrupt samples or DAC glitches, and do not
    treat this as a safe margin (temperature/board dependent).
* Many `set_false_path` constraints in the XDC refer to clocks that don't exist in this design
  (`clk_fpga_0`, `dac_clk_o`, `ser_clk`, `pdm_clk`, …), giving CRITICAL WARNING 12-4739. They are
  harmless but noisy.
* `dac_pwm_o` IOB warnings: the PWM outputs are tied to 0.

---

## 11. Known bugs and hazards (ranked)

1. **Runaway DMA if `CTRL_REG[1]` is cleared.** With bit 1 = 0 the interrupt request is never raised,
   bursts never stop, and `addr_shift` grows without bound. The DMA then writes past the 512 KiB buffer
   into arbitrary DDR, wrapping through the whole 32-bit space. `server/oscilloscope.c::clear_interrupt()`
   writes `0x1` and would trigger this. The loop uses `request_and_clear_interrupt()` (`0x3`), which is
   safe. **Always write 3, never 1 or 0.**
2. **Waveform-2 capacity / register aliasing.** The AXI-BRAM IP has a 17-bit address but
   `reg_ctrl_top` keeps 16 bits. Waveform 2 starts at `0xA000`, so only 12288 samples fit below
   `0x1_0000`. Samples 12288..16383 (`0x4001_0000+`) alias onto offsets `0x0000`–`0x1FFF` and overwrite
   the acquisition and generator registers *(code reading)*. Keep `len2 ≤ 12288`. Proper fix: widen
   `S_AXI_REG_ADDR_BITS` to 17 and move/resize the window, or put waveform 2 at `0x1_0000`.
3. **`DEC_FACTOR = 0`** gives a permanent `trig`: the FIFO fills even with `START_ACQ = 0`, and
   acquisition can't be stopped. Fix: `trig = start_acq && (dec_cnt >= cfg_dec)`, or clamp to ≥ 1 in
   software.
4. **WLAST relies on WREADY.** `last = (cnt == 15)`, but `cnt` is reset by `last` itself on the next
   clock whether or not the beat was accepted. If HP0 deasserts WREADY on the 16th beat, WLAST is lost,
   which is an AXI protocol violation (hang or garbage). It works in practice because the HP write FIFO
   is rarely full. Fix: reset `cnt` on `transfer && last` only.
5. **`FIFO_MIN_THRESH < 15`** allows a 16-beat burst to start with fewer than 16 entries in the FIFO, so
   the FIFO is read while empty. Leave it at the default 15.
6. **Timing failure** (§10).
7. **Threshold underflow** (§5.7): it gives large chunks to fast readers. Harmless for buffer size, but
   it increases latency (up to 32768 samples).
8. **Sample loss is silent.** FIFO overflow only shows on LED5, latched at clear time, and is not
   readable over AXI. With `cfg_dec = 50` the server must service each IRQ within about 6.6 ms.
9. **Position/integral skew** of `cfg_dec/2` cycles (§5.5), on top of the galvo lag the README mentions.
   The x slot is the *command* for OUT1, not the measured position.
10. **Generator frequency quantisation:** `dec` is an integer (§5.3).
11. `CTRL_REG`, `INPUT_SEL`, `FIFO_DOUT_*` and waveform readback return junk (they fall to the default
    `wfm_read` in the read mux).
12. `stop_acq` does not flush the FIFO or the pending DDR chunk. Leftover samples (< 16, plus anything
    not yet sent) appear at the start of the next run. The server's worker thread can stay blocked in
    `read()` after stop.
13. `START_GEN` and `START_ACQ` are separate writes, so there is no fixed phase between the first sample
    and the waveform start. This is fine because positions are recorded in every sample.

Old tree (`fpga/project`) only, for reference if someone revives it: `integrator_top` drives
`integrated_signal` from two always-blocks. Vivado kept the constant-0 driver (CRITICAL WARNING
Synth 8-6858/6859), so the output is always 0. The generator registers at `0x100000+` are unreachable
with a 12-bit BRAM address. `WAVEFORM_LEN_2_ADDR = 0x12` is misaligned. `generate_top` uses `advance_1`
for channel 2 and inverted compares. `waveform_border = 160000` exceeds the 14-bit address. `blk_mem`
`ena` is tied to 0.

---

## 12. How to modify safely

* **Add a register:** add a `localparam` below `0x2000` in `reg_ctrl_top.v`, an `always` write block
  (32-bit writes only), and a read-mux entry. Add the port in the BD (module reference → refresh), then
  add the field to the C struct (`Reg_map_osc`/`Reg_map_gen`) at the same offset. Regs in
  `0x0000`–`0x0FFF` are reachable through `rp_acquire`, regs in `0x1000`–`0x1FFF` through
  `rp_generate`.
* **Change the record format:** `axi_control.data_in` (64 bit). The FIFO IP width, the burst `awsize`,
  `addr_shift` (+128 = 16 × 8 B), `buffer_n` units, the server (`len = buff_n*8`) and the client
  `record_dtype` all depend on it.
* **Change the DMA buffer:** `axi_s2mm.sv` hard-codes `32'h1900_0000`. Keep it consistent with
  `fpga.dtso` (`rp_acquire` map 1) and make sure the region is reserved in Linux.
* **After any change:** re-run implementation, check `rp_top_timing_summary_routed.rpt`, copy
  `impl_1/rp_top.bit` to `fpga/`, and rebuild the dtbo if addresses changed.
* Ask the user before editing code (workspace rule). The deployed project is untracked, so suggest
  committing it first.
