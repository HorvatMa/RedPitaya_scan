"""Electrical loopback test of the real hardware (no galvos connected).

Wiring:
    OUT1 (slow sawtooth) -> IN1 (detector)
    fast position:  --fast generator : signal generator -> IN2  (sine/triangle, within +-1 V)
                    --fast out2      : OUT2 -> IN2  (exact reference, no generator needed)

The detector then sees the slow voltage itself, so every row of the image should be uniform
and the row value should be a straight line in the OUT1 voltage. The fit gives the polarity,
scale and offset of IN1 and shows whether the ADC conversion in the FPGA is linear.

    # on the board: cd /home/redpitaya && ./apply_overlay.sh rp && /root/server2_dev/build/rpsipm_server
    ~/.venv/base/bin/python tests/hw_loopback.py --fast generator --fast-amp 0.4 --fast-freq 500
"""

import argparse
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "client", "viewer"))
from rpsipm.protocol import Client, parse_kv  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description="hardware loopback test")
    ap.add_argument("--host", default="rp-f0a360.local")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--fast", choices=["generator", "out2"], default="generator")
    ap.add_argument("--fast-amp", type=float, default=0.4, help="fast signal peak amplitude at IN2 [V]")
    ap.add_argument("--fast-offset", type=float, default=0.0, help="fast signal offset at IN2 [V]")
    ap.add_argument("--fast-freq", type=float, default=500.0, help="fast signal frequency [Hz]")
    ap.add_argument("--slow-amp", type=float, default=0.8, help="OUT1 sawtooth peak amplitude [V]")
    ap.add_argument("--slow-freq", type=float, default=3.0)
    ap.add_argument("--frames", type=int, default=5)
    ap.add_argument("--out", default=os.path.join(HERE, "..", "build", "hw_loopback"))
    args = ap.parse_args()

    frames, status = [], []
    c = Client(args.host, args.port, on_frame=frames.append, on_status=status.append, timeout=10)
    c.command("stop")
    settings = {
        # all gains 1: volts in the config are volts at the RP connectors
        "hw.slow_gain": 1, "hw.fast_gain": 1, "hw.sensor_gain": 1,
        "cal.fast_gain": 1, "cal.fast_offset": 0, "cal.delay": 0.5,
        "slow.shape": "sawtooth", "slow.rise": 0.975, "slow.freq": args.slow_freq,
        "slow.amp": args.slow_amp, "slow.offset": 0,
        "fast.shape": "sine", "fast.freq": args.fast_freq,
        "fast.amp": args.fast_amp, "fast.offset": args.fast_offset,
        "acq.dec": 50, "acq.int_dec": 1,
        "img.width": "auto", "img.height": "auto", "img.bidir": "both",
        "img.roi": "0 1 0.02 0.98", "img.dtype": "float32",
    }
    for k, v in settings.items():
        c.set(k, v)
    cfg = c.get_config()
    print("config:", {k: cfg[k] for k in ("slow.freq_actual", "fast.freq_actual", "img.width_actual",
                                          "img.height_actual", "fast.sensor_levels")})
    c.command("start")
    t0 = time.monotonic()
    while len(frames) < args.frames and time.monotonic() - t0 < 10 + args.frames / args.slow_freq * 2:
        time.sleep(0.1)
    st = parse_kv(c.command("get status"))
    c.command("stop")
    c.close()
    print("status:", {k: st.get(k) for k in ("state", "hw", "rate_rec_s", "chunks_lost", "frames_made",
                                             "frames_sent", "frames_dropped")})
    if not frames:
        print("FAIL: no frames received")
        return 1

    f = frames[-1]
    img, bwd = f.data[0], f.data[1] if f.n_images > 1 else None
    h, w = img.shape
    empty = np.isnan(img).mean()

    # row value vs OUT1 voltage of that row (rows span the slow window linearly)
    x0, x1 = f.roi[0], f.roi[1]
    v_row = -args.slow_amp + 2 * args.slow_amp * (x0 + (x1 - x0) * (np.arange(h) + 0.5) / h)
    row = np.nanmean(img, axis=1)
    ok = np.isfinite(row)
    slope, icpt = np.polyfit(v_row[ok], row[ok], 1)
    resid = row[ok] - (icpt + slope * v_row[ok])
    r2 = 1 - resid.var() / row[ok].var()
    row_spread = np.nanmedian(np.nanstd(img, axis=1))           # non-uniformity along a row
    step = abs(slope) * 2 * args.slow_amp / h                    # expected change per row
    fb = np.nanmean(np.abs(img - bwd)) if bwd is not None else float("nan")

    print(f"frame {f.frame_id}: {w}x{h}, empty {empty:.1%}, flags {f.flags}")
    print(f"IN1 vs OUT1: pixel = {icpt:.1f} + {slope:.1f} * V   (R^2 = {r2:.5f}, "
          f"max residual {np.abs(resid).max():.1f} counts)")
    print(f"  polarity: {'more voltage -> larger pixel' if slope > 0 else 'INVERTED (more voltage -> smaller pixel)'}")
    print(f"  per-row non-uniformity (median std along a row): {row_spread:.1f} counts "
          f"(row-to-row step {step:.1f})")
    print(f"  forward vs backward mean |difference|: {fb:.1f} counts")

    os.makedirs(args.out, exist_ok=True)
    np.savez(os.path.join(args.out, "loopback.npz"), data=f.data, v_row=v_row, row=row,
             header=json.dumps(f.header_dict()), config=json.dumps(cfg))
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(1, 3, figsize=(14, 4))
        ax[0].imshow(img, aspect="auto", cmap="viridis")
        ax[0].set_title("image (forward/sum)")
        ax[1].plot(v_row, row, ".", ms=3, label="row mean")
        ax[1].plot(v_row, icpt + slope * v_row, "-", lw=1, label="linear fit")
        ax[1].set_xlabel("OUT1 voltage of the row [V]")
        ax[1].set_ylabel("pixel value")
        ax[1].legend()
        ax[2].plot(v_row[ok], resid, ".", ms=3)
        ax[2].set_xlabel("OUT1 voltage [V]")
        ax[2].set_ylabel("residual [counts]")
        fig.tight_layout()
        png = os.path.join(args.out, "loopback.png")
        fig.savefig(png, dpi=90)
        print("plot:", png)
    except Exception as exc:
        print("no plot:", exc)

    bad = []
    if empty > 0.1:
        bad.append(f"{empty:.0%} empty pixels (is the fast signal inside +-{args.fast_amp} V?)")
    if r2 < 0.999:
        bad.append(f"row values not linear in OUT1 (R^2 {r2:.4f}): ADC conversion or wiring")
    print("RESULT:", "PASS" if not bad else "CHECK: " + "; ".join(bad))
    return 0 if not bad else 2


if __name__ == "__main__":
    sys.exit(main())
