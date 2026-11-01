#!/usr/bin/env python3
"""
Mechanical integrator: experiment runner + data analysis.

Setup (once):
    pip install pyserial numpy matplotlib

Find the controller's port in the Arduino IDE (Tools > Port), e.g. COM5.
Close the Arduino Serial Monitor first: only one program can use the port.

Commands:
    python integrator_logger.py run --port COM5 --repeats 1     quick trial run
    python integrator_logger.py run --port COM5                 full experiment (3 repeats)
    python integrator_logger.py run --port COM5 --sine          also run the sin test
                                                                (needs SLIDE + ZERO first)
    python integrator_logger.py analyze                         tables + graphs
    python integrator_logger.py run --simulate                  try it with no hardware

Everything is saved in the folder integrator_data/:
    results.csv          one row per run (the raw RESULT lines)
    traces/run_###.csv   the live x / f(x) / wheel-ticks data for each run
    summary.csv          means, standard deviations, predicted and measured errors
    *.png                the graphs

Re-running "run" adds to results.csv, so you can collect data over several sessions.
"""

import argparse
import csv
import math
import os
import random
import sys
import time
from datetime import datetime

# Data is always saved next to this script, whatever folder you run it from.
DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "integrator_data")
RESULTS_CSV = os.path.join(DATA_DIR, "results.csv")
TRACE_DIR = os.path.join(DATA_DIR, "traces")
SUMMARY_CSV = os.path.join(DATA_DIR, "summary.csv")

# ---- These must match the controller firmware ----
XMAX = 10240               # sweep length in turntable steps
FUNC_PEAK = 4000.0
RADIUS_SCALE = 8000.0      # DISPLAY_RADIUS_MAX
DISK_RADIUS_MM = 100.0
MAX_TRAVEL_MM = 90.0
PEAK_X_FRAC = 0.45

# ---- Default experiment plan ----
SEGS = {
    "SQUARE": [2560, 1280, 640, 320],  # n = 4, 8, 16, 32
    "SINE":   [2560, 1280, 640, 320],  # n = 4, 8, 16, 32
}
RULES = ["LEFT", "MID", "TRAP"]
FLAT_SEG = 1280            # FLAT runs calibrate the machine; any rule is exact for a constant

RESULT_FIELDS = ["run_id", "timestamp", "rule", "function", "seg_steps", "segments",
                 "ticks", "wheel_rad", "clips", "trace_file"]


# =====================================================================
#  Model of the firmware's function -> radius mapping
# =====================================================================
def func_value(mode, u):
    """f as the firmware computes it, with u = x / XMAX in [0, 1]."""
    import numpy as np
    u = np.clip(np.asarray(u, dtype=float), 0.0, 1.0)
    if mode == "FLAT":
        return np.full_like(u, FUNC_PEAK * 0.5)
    if mode == "SLOPE":
        return FUNC_PEAK * u
    if mode == "CURVE":
        ratio = u / PEAK_X_FRAC
        return FUNC_PEAK * ratio * np.exp(1.0 - ratio)
    if mode == "SQUARE":
        return FUNC_PEAK * u * u
    if mode == "SINE":
        return FUNC_PEAK * np.sin(1.5 * np.pi * u)
    raise ValueError(f"Analysis does not support function {mode}")


def radius_mm(mode, u, negative_allowed):
    """Radius in mm the carriage is commanded to, including the firmware's clipping."""
    import numpy as np
    norm = func_value(mode, u) / RADIUS_SCALE
    norm = np.clip(norm, -1.0 if negative_allowed else 0.0, 1.0)
    return np.clip(norm * DISK_RADIUS_MM, -MAX_TRAVEL_MM, MAX_TRAVEL_MM)


def exact_area(mode, negative_allowed, upto=1.0, points=200001):
    """Integral of r(u) du from 0 to `upto` (fine-grid Simpson's rule)."""
    import numpy as np
    if points % 2 == 0:
        points += 1
    u = np.linspace(0.0, upto, points)
    r = radius_mm(mode, u, negative_allowed)
    h = upto / (points - 1)
    return h / 3 * (r[0] + r[-1] + 4 * r[1:-1:2].sum() + 2 * r[2:-1:2].sum())


def rule_area(mode, rule, n, negative_allowed):
    """What the machine computes with n segments: the left, midpoint or trapezoid sum of r(u)."""
    import numpy as np
    h = 1.0 / n
    nodes = np.arange(n) * h
    if rule == "LEFT":
        return h * radius_mm(mode, nodes, negative_allowed).sum()
    if rule == "MID":
        return h * radius_mm(mode, nodes + h / 2, negative_allowed).sum()
    if rule == "TRAP":
        r0 = radius_mm(mode, nodes, negative_allowed)
        r1 = radius_mm(mode, nodes + h, negative_allowed)
        return h * ((r0 + r1) / 2).sum()
    raise ValueError(rule)


# =====================================================================
#  Serial link
# =====================================================================
class FakeController:
    """Stands in for the real machine so the script can be tried without hardware."""

    def __init__(self):
        self.out = []
        self.rule, self.seg, self.xmax, self.neg = "TRAP", 5, 10240, False
        self.K = 0.08            # pretend wheel radians per (mm x unit sweep)
        self.out.append("Mechanical Integrator ready.")

    def write(self, data):
        import numpy as np
        cmd = data.decode().strip().upper()
        if cmd == "STATUS":
            self.out.append(f"STATUS rule={self.rule} mode=CURVE seg={self.seg} xmax={self.xmax} segments=1 negRadius=off")
        elif cmd == "RESET":
            self.out.append("Reset. Carriage returned to start.")
        elif cmd.startswith("RULE"):
            self.rule = cmd.split()[1]; self.out.append(f"Rule: {self.rule}")
        elif cmd.startswith("SEG"):
            self.seg = int(cmd.split()[1]); self.out.append(f"Segment size: {self.seg} steps")
        elif cmd.startswith("XMAX"):
            self.xmax = int(cmd.split()[1]); self.out.append(f"Sweep length: {self.xmax} steps")
        elif cmd.startswith("SPEED"):
            self.out.append(f"Step delay: {cmd.split()[1]} us")
        elif cmd.startswith("NEG"):
            self.neg = cmd.endswith("ON"); self.out.append("Negative radius: " + ("on" if self.neg else "off"))
        elif cmd.startswith("RUN"):
            mode = cmd.split()[1]
            n = math.ceil(self.xmax / self.seg)
            self.out.append(f"Started: {cmd} rule={self.rule} seg={self.seg}")
            cpr = 4096 / (2 * math.pi)
            h = 1.0 / n
            nodes = np.arange(n) * h
            if self.rule == "LEFT":
                pieces = h * radius_mm(mode, nodes, self.neg)
            elif self.rule == "MID":
                pieces = h * radius_mm(mode, nodes + h / 2, self.neg)
            else:
                pieces = h * (radius_mm(mode, nodes, self.neg) + radius_mm(mode, nodes + h, self.neg)) / 2
            running = np.cumsum(pieces)
            for i in range(n):
                self.out.append(f"{(i + 1) * self.seg},{float(func_value(mode, (i + 1) / n)):.2f},"
                                f"{int(self.K * running[i] * 1.003 * cpr)}")
            area = running[-1]
            wheel = self.K * area * 1.003 + np.random.normal(0, 0.004)   # 0.3% slip-like bias + noise
            ticks = int(round(wheel * cpr))
            self.out.append("Sweep complete.")
            self.out.append(f"RESULT,{self.rule},{mode},{self.seg},{n},{ticks},{ticks / cpr:.5f},0")

    def flush(self):
        pass

    def readline(self):
        time.sleep(0.001)
        return (self.out.pop(0) + "\n").encode() if self.out else b""

    def reset_input_buffer(self):
        self.out = [l for l in self.out if "ready" in l.lower()]

    def close(self):
        pass


def open_port(port, simulate):
    if simulate:
        link = FakeController()
    else:
        try:
            import serial
        except ImportError:
            sys.exit("pyserial is missing. Run: pip install pyserial")
        try:
            link = serial.Serial(port, 115200, timeout=0.5)
        except Exception as e:
            sys.exit(f"Could not open {port}: {e}\n(Is the Arduino Serial Monitor still open?)")
    # Opening the port usually resets the ESP32. Wait for its start-up message.
    t0 = time.time()
    while time.time() - t0 < 5:
        if "ready" in read_line(link).lower():
            break
    time.sleep(0.3)
    link.reset_input_buffer()
    return link


def read_line(link):
    return link.readline().decode(errors="ignore").strip()


def send(link, cmd):
    link.write((cmd + "\n").encode())
    link.flush()


def wait_for(link, prefix, timeout, collect=None):
    t0 = time.time()
    while time.time() - t0 < timeout:
        line = read_line(link)
        if not line:
            continue
        if collect is not None:
            collect.append(line)
        if line.startswith(prefix):
            return line
    raise TimeoutError(f"No '{prefix}' reply within {timeout} s")


# =====================================================================
#  run
# =====================================================================
def next_run_id():
    if not os.path.exists(RESULTS_CSV):
        return 1
    with open(RESULTS_CSV, newline="") as f:
        ids = [int(r["run_id"]) for r in csv.DictReader(f)]
    return max(ids, default=0) + 1


def append_result(row):
    os.makedirs(DATA_DIR, exist_ok=True)
    new = not os.path.exists(RESULTS_CSV)
    with open(RESULTS_CSV, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=RESULT_FIELDS)
        if new:
            w.writeheader()
        w.writerow(row)


def do_run(link, func, rule, seg, run_id):
    send(link, "STOP")
    send(link, "RESET")
    wait_for(link, "Reset.", 120)
    send(link, f"RULE {rule}")
    wait_for(link, "Rule:", 5)
    send(link, f"SEG {seg}")
    wait_for(link, "Segment size", 5)
    send(link, f"RUN {func}")
    wait_for(link, "Started:", 30)

    trace, lines = [], []
    result = None
    t0 = time.time()
    while time.time() - t0 < 900:
        line = read_line(link)
        if not line:
            continue
        lines.append(line)
        if line.startswith("RESULT,"):
            result = line
            break
        parts = line.split(",")
        if len(parts) == 3:
            try:
                trace.append([float(p) for p in parts])
            except ValueError:
                pass
    if result is None:
        raise TimeoutError("Run did not finish within 15 minutes")

    _, r_rule, r_func, r_seg, r_n, r_ticks, r_wheel, r_clips = result.split(",")
    trace_file = os.path.join(TRACE_DIR, f"run_{run_id:03d}.csv")
    os.makedirs(TRACE_DIR, exist_ok=True)       # recreate it if the folder was deleted mid-session
    with open(trace_file, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["x_steps", "f", "wheel_ticks"])
        w.writerows(trace)

    row = {"run_id": run_id, "timestamp": datetime.now().isoformat(timespec="seconds"),
           "rule": r_rule, "function": r_func, "seg_steps": int(r_seg), "segments": int(r_n),
           "ticks": int(r_ticks), "wheel_rad": float(r_wheel), "clips": int(r_clips),
           "trace_file": os.path.relpath(trace_file, DATA_DIR)}
    append_result(row)
    return row


def cmd_run(args):
    os.makedirs(TRACE_DIR, exist_ok=True)
    if args.only_sine:
        args.sine = True
        funcs = ["SINE"]
    else:
        funcs = ["SQUARE"] + (["SINE"] if args.sine else [])

    plan = []
    if not args.no_flat:
        plan += [("FLAT", "TRAP", FLAT_SEG)] * args.repeats
    for func in funcs:
        for rule in RULES:
            for seg in SEGS[func]:
                plan += [(func, rule, seg)] * args.repeats
    # Random order so slow drift (belt warming, loosening) doesn't line up with one setting.
    random.Random(args.seed).shuffle(plan)

    if args.sine and not args.simulate:
        print("The sin test needs a negative radius. Before continuing:")
        print("  1. In the Serial Monitor, use SLIDE <steps> until the wheel is exactly over the disk centre.")
        print("  2. Send ZERO.  3. Close the Serial Monitor.")
        if input("Done? (y/n) ").strip().lower() != "y":
            sys.exit("Stopped. Run again when the carriage is zeroed.")

    link = open_port(args.port, args.simulate)
    send(link, "STATUS")
    try:
        wait_for(link, "STATUS rule=", 5)
    except TimeoutError:
        sys.exit("The controller didn't answer STATUS. Is the new firmware flashed, and is this the controller's port?")
    send(link, f"XMAX {XMAX}")
    wait_for(link, "Sweep length", 5)

    run_id = next_run_id()
    print(f"{len(plan)} runs planned. Results go to {RESULTS_CSV}. Press Ctrl+C to stop; finished runs are kept.\n")
    neg_on = None
    started = time.time()
    try:
        for i, (func, rule, seg) in enumerate(plan, 1):
            want_neg = func == "SINE"
            if want_neg != neg_on:
                send(link, "NEG ON" if want_neg else "NEG OFF")
                wait_for(link, "Negative radius", 5)
                neg_on = want_neg
            row = do_run(link, func, rule, seg, run_id)
            flag = "  <-- CLIPPED, ignore" if row["clips"] else ""
            elapsed = time.time() - started
            left = elapsed / i * (len(plan) - i)
            print(f"[{i:3d}/{len(plan)}] run {run_id:3d}  {func:6s} {rule:4s} seg={seg:5d} "
                  f"n={row['segments']:3d}  wheel = {row['wheel_rad']:.4f} rad{flag}"
                  f"   (~{left / 60:.0f} min left)")
            run_id += 1
    except KeyboardInterrupt:
        print("\nStopped by you. Completed runs are saved.")
    finally:
        send(link, "STOP")
        send(link, "NEG OFF")
        link.close()
    print("\nNext: python integrator_logger.py analyze")


# =====================================================================
#  analyze
# =====================================================================
def cmd_analyze(args):
    try:
        import numpy as np
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("Install the analysis libraries: pip install numpy matplotlib")

    if not os.path.exists(RESULTS_CSV):
        sys.exit(f"No data yet: {RESULTS_CSV} not found. Do a run first.")
    with open(RESULTS_CSV, newline="") as f:
        rows = [r for r in csv.DictReader(f)]
    clipped = [r for r in rows if int(r["clips"]) > 0]
    rows = [r for r in rows if int(r["clips"]) == 0]
    if clipped:
        print(f"Ignoring {len(clipped)} clipped run(s).")

    # ---- Machine constant K: wheel radians = K x (area under r(u), mm) ----
    # A constant function has no discretization error under any rule, so FLAT runs
    # measure K directly. This folds the disk angle per step, wheel radius and belt
    # scale into one measured number.
    flat = [float(r["wheel_rad"]) for r in rows if r["function"] == "FLAT"]
    if flat:
        area_flat = exact_area("FLAT", False)
        K_vals = np.array(flat) / area_flat
        K, K_sd = K_vals.mean(), (K_vals.std(ddof=1) if len(K_vals) > 1 else 0.0)
        print(f"Calibration from {len(flat)} FLAT run(s): K = {K:.6f} rad/mm (sd {K_sd:.6f}, "
              f"{(K_sd / K * 100 if K else 0):.3f}%)")
    elif args.phi and args.R:
        K = args.phi * XMAX / args.R
        K_sd = 0.0
        print(f"No FLAT runs; using K = phi*XMAX/R = {K:.6f} rad/mm from --phi and --R")
    else:
        sys.exit("No FLAT runs to calibrate from. Run with FLAT included, or pass --phi (disk rad per step) and --R (wheel mm).")

    # ---- Group repeats ----
    groups = {}
    for r in rows:
        if r["function"] == "FLAT":
            continue
        key = (r["function"], r["rule"], int(r["seg_steps"]), int(r["segments"]))
        groups.setdefault(key, []).append(float(r["wheel_rad"]))

    summary = []
    for (func, rule, seg, n), vals in sorted(groups.items()):
        neg = func == "SINE"
        vals = np.array(vals)
        exact = K * exact_area(func, neg)
        pred = K * rule_area(func, rule, n, neg)
        mean = vals.mean()
        sd = vals.std(ddof=1) if len(vals) > 1 else float("nan")
        e_meas = exact - mean            # E = exact - machine, as in the write-up
        e_pred = exact - pred
        summary.append({
            "function": func, "rule": rule, "seg_steps": seg, "segments": n,
            "h_fraction": 1.0 / n, "repeats": len(vals),
            "mean_wheel_rad": mean, "sd_wheel_rad": sd,
            "exact_wheel_rad": exact, "predicted_wheel_rad": pred,
            "E_predicted": e_pred, "E_measured": e_meas,
            "E_measured_pct": e_meas / exact * 100,
            "E_predicted_pct": e_pred / exact * 100,
        })

    with open(SUMMARY_CSV, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(summary[0].keys()) if summary else ["none"])
        w.writeheader()
        for s in summary:
            w.writerow({k: (f"{v:.6g}" if isinstance(v, float) else v) for k, v in s.items()})

    sds = [s["sd_wheel_rad"] for s in summary if not math.isnan(s["sd_wheel_rad"])]
    noise = float(np.sqrt(np.mean(np.square(sds)))) if sds else 0.0
    if noise:
        print(f"Noise floor (pooled sd of repeats): {noise:.5f} rad. "
              f"Errors smaller than ~{2 * noise:.5f} rad can't be told apart from noise.")

    # ---- Table + order-of-convergence fits ----
    colors = {"LEFT": "#c0392b", "MID": "#2471a3", "TRAP": "#1e8449"}
    expected_p = {"LEFT": 1, "MID": 2, "TRAP": 2}
    for func in sorted({s["function"] for s in summary}):
        print(f"\n=== {func} ===")
        print(f"{'rule':5s} {'n':>4s} {'mean wheel':>11s} {'sd':>9s} {'E meas':>10s} {'E pred':>10s} {'E meas %':>9s}")
        fig, ax = plt.subplots(figsize=(7, 5))
        for rule in RULES:
            pts = sorted([s for s in summary if s["function"] == func and s["rule"] == rule],
                         key=lambda s: s["h_fraction"])
            if not pts:
                continue
            for s in pts:
                print(f"{rule:5s} {s['segments']:4d} {s['mean_wheel_rad']:11.5f} {s['sd_wheel_rad']:9.5f} "
                      f"{s['E_measured']:10.5f} {s['E_predicted']:10.5f} {s['E_measured_pct']:8.3f}%")
            h = np.array([s["h_fraction"] for s in pts])
            em = np.abs([s["E_measured"] for s in pts])
            ep = np.abs([s["E_predicted"] for s in pts])
            sd = np.nan_to_num([s["sd_wheel_rad"] for s in pts])

            ax.loglog(h, ep, "--", color=colors[rule], alpha=0.6)
            ax.errorbar(h, em, yerr=sd, fmt="o", color=colors[rule], capsize=3)

            usable = em > 2 * noise if noise else np.ones_like(em, bool)
            label = f"{rule} (predicted p = {expected_p[rule]})"
            if usable.sum() >= 2:
                p, c = np.polyfit(np.log(h[usable]), np.log(em[usable]), 1)
                label = f"{rule}: fitted p = {p:.2f} (predicted {expected_p[rule]})"
                print(f"  -> {rule}: fitted gradient p = {p:.3f} from {usable.sum()} points above the noise floor")
            else:
                print(f"  -> {rule}: fewer than 2 points above the noise floor; no fit")
            ax.plot([], [], "o--", color=colors[rule], label=label)

        mids = {s["segments"]: s["E_measured"] for s in summary if s["function"] == func and s["rule"] == "MID"}
        traps = {s["segments"]: s["E_measured"] for s in summary if s["function"] == func and s["rule"] == "TRAP"}
        for n in sorted(set(mids) & set(traps)):
            if mids[n]:
                print(f"  E_T / E_M at n={n}: {traps[n] / mids[n]:.2f}  (theory: -2)")

        if noise:
            ax.axhline(2 * noise, color="gray", linestyle=":", label="noise floor (2 sd)")
        ax.set_xlabel("Segment size h (fraction of the sweep, = 1/n)")
        ax.set_ylabel("|Error| in wheel rotation (rad)")
        has_repeats = any(s["repeats"] > 1 for s in summary if s["function"] == func)
        point_note = "measured (mean, sd error bars)" if has_repeats else "measured"
        ax.set_title(f"Error vs segment size: {func}\npoints = {point_note}, dashed = predicted")
        ax.grid(True, which="both", alpha=0.3)
        ax.legend(fontsize=8)
        fig.tight_layout()
        out = os.path.join(DATA_DIR, f"error_vs_h_{func}.png")
        fig.savefig(out, dpi=200)
        plt.close(fig)
        print(f"  saved {out}")

        # ---- Running-integral trace: coarsest segment size, one run per rule ----
        fig, ax = plt.subplots(figsize=(7, 4.5))
        u_fine = np.linspace(0, 1, 400)
        neg = func == "SINE"
        r_fine = radius_mm(func, u_fine, neg)
        running = np.concatenate([[0], np.cumsum((r_fine[1:] + r_fine[:-1]) / 2 * np.diff(u_fine))])
        ax.plot(u_fine * XMAX, K * running, color="black", lw=2, label="exact running integral")
        coarsest = max((int(r["seg_steps"]) for r in rows if r["function"] == func), default=None)
        for rule in RULES:
            match = [r for r in rows if r["function"] == func and r["rule"] == rule and int(r["seg_steps"]) == coarsest]
            if not match:
                continue
            path = os.path.join(DATA_DIR, match[-1]["trace_file"])
            if not os.path.exists(path):
                continue
            with open(path, newline="") as f:
                tr = [(float(a["x_steps"]), float(a["wheel_ticks"])) for a in csv.DictReader(f)]
            if tr:
                if tr[0][0] > 0:
                    tr.insert(0, (0.0, 0.0))     # every run starts at x = 0 with the wheel zeroed
                x, t = zip(*tr)
                ax.plot(x, np.array(t) * 2 * np.pi / 4096, "o-", ms=3, color=colors[rule],
                        label=f"{rule}, run {match[-1]['run_id']}")
        ax.set_xlabel("Turntable position x (steps)")
        ax.set_ylabel("Wheel rotation (rad)")
        ax.set_title(f"Running integral, {func}, coarsest segments (seg = {coarsest} steps)")
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
        fig.tight_layout()
        out = os.path.join(DATA_DIR, f"running_integral_{func}.png")
        fig.savefig(out, dpi=200)
        plt.close(fig)
        print(f"  saved {out}")

    print(f"\nFull table: {SUMMARY_CSV}")


# =====================================================================
def main():
    p = argparse.ArgumentParser(description="Mechanical integrator experiment runner and analysis")
    sub = p.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="run the experiment on the machine and log the data")
    r.add_argument("--port", help="serial port of the controller, e.g. COM5")
    r.add_argument("--repeats", type=int, default=3, help="repeats per setting (default 3; use 1 for a trial)")
    r.add_argument("--sine", action="store_true", help="also run the sin test (needs SLIDE + ZERO first)")
    r.add_argument("--only-sine", action="store_true", help="run only the sin test (plus FLAT), skipping SQUARE")
    r.add_argument("--no-flat", action="store_true", help="skip the FLAT calibration runs")
    r.add_argument("--seed", type=int, default=None, help="seed for the random run order")
    r.add_argument("--simulate", action="store_true", help="use a pretend machine (no hardware needed)")

    a = sub.add_parser("analyze", help="make the summary table and graphs")
    a.add_argument("--phi", type=float, help="disk angle per turntable step (rad), only if there are no FLAT runs")
    a.add_argument("--R", type=float, help="wheel radius (mm), only if there are no FLAT runs")

    if len(sys.argv) == 1:
        print(__doc__)
        sys.exit(0)
    args = p.parse_args()
    if args.cmd == "run":
        if not args.port and not args.simulate:
            p.error("--port is required (or use --simulate)")
        cmd_run(args)
    else:
        cmd_analyze(args)


if __name__ == "__main__":
    main()
