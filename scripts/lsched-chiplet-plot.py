#!/usr/bin/env python3
"""Evaluate scripts/lsched-chiplet.sh: same CCD vs different CCDs.

    scripts/lsched-chiplet-plot.py <dir with same-r*.log, diff-r*.log> [out-stem]

Prints a Markdown table (mean and spread over the repetitions) and writes
<out-stem>.png/.pdf: package power and worker-CCD temperatures over time,
plus throughput and core power per run.
"""
import glob
import os
import statistics as st
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

COLOR = {"same": "#104281", "diff": "#eb6834"}
LABEL = {"same": "same CCD (35+36)", "diff": "different CCDs (35+42)"}


def parse(path):
    r = {"energy": {}, "temp": {}, "series": [], "cols": None}
    with open(path, errors="replace") as fh:
        for line in fh:
            line = line.strip().replace("\r", "")
            if not line.startswith("lsched,"):
                continue
            f = line.split(",")
            if f[1] == "summary" and f[2][:1].isdigit():
                r["tps"] = float(f[7])
            elif f[1] == "energy" and f[2] != "domain" and len(f) >= 7:
                r["energy"][f[2]] = float(f[6])
            elif f[1] == "temp" and f[2] != "sensor":
                r["temp"][f[2]] = {"start": float(f[3]), "end": float(f[4]), "mean": float(f[6]), "max": float(f[7])}
            elif f[1] == "sample":
                if f[2] == "t_s":
                    r["cols"] = f[2:]
                else:
                    r["series"].append([float(x) for x in f[2:]])
    return r


def runs(d, setup):
    return [parse(p) for p in sorted(glob.glob(os.path.join(d, f"{setup}-r*.log")))]


def agg(vals):
    vals = [v for v in vals if v == v]
    if not vals:
        return "n/a"
    m = st.mean(vals)
    return f"{m:.2f} ± {st.stdev(vals):.2f}" if len(vals) > 1 else f"{m:.2f}"


def main():
    d = sys.argv[1]
    stem = sys.argv[2] if len(sys.argv) > 2 else os.path.join(d, "chiplet")
    data = {s: runs(d, s) for s in ("same", "diff")}
    if not all(data.values()):
        sys.exit("need same-r*.log and diff-r*.log in " + d)

    rows = [
        ("throughput [tx/s]", lambda r: r.get("tps", float("nan"))),
        ("package [W]", lambda r: r["energy"].get("package", float("nan"))),
        ("worker core 1 [W]", lambda r: r["energy"].get("core_worker0", float("nan"))),
        ("worker core 2 [W]", lambda r: r["energy"].get("core_worker1", float("nan"))),
        ("generator core [W]", lambda r: r["energy"].get("core_generator", float("nan"))),
        ("package energy per tx [mJ]", lambda r: 1e3 * r["energy"].get("package", float("nan")) / r.get("tps", float("nan"))),
        ("worker energy per tx [uJ]", lambda r: 1e6 * (r["energy"].get("core_worker0", 0) + r["energy"].get("core_worker1", 0)) / r.get("tps", float("nan"))),
    ]
    sensors = sorted({k for s in data.values() for r in s for k in r["temp"]})
    for sn in sensors:
        rows.append((f"{sn} mean [C]", lambda r, sn=sn: r["temp"].get(sn, {}).get("mean", float("nan"))))
        rows.append((f"{sn} max [C]", lambda r, sn=sn: r["temp"].get(sn, {}).get("max", float("nan"))))
    print(f"| metric | {LABEL['same']} | {LABEL['diff']} |")
    print("|---|---|---|")
    for name, fn in rows:
        print(f"| {name} | {agg([fn(r) for r in data['same']])} | {agg([fn(r) for r in data['diff']])} |")
    print(f"\n(mean ± stdev over {len(data['same'])} / {len(data['diff'])} runs)")

    fig, ax = plt.subplots(1, 4, figsize=(17, 5), gridspec_kw={"width_ratios": [1.3, 1.3, 0.6, 0.6]})
    for setup, rs in data.items():
        for i, r in enumerate(rs):
            cols = r["cols"]
            t = [row[0] for row in r["series"]]
            # package watts over each period; the first sample has none and the
            # last one (taken at the end, right after a periodic one) a period
            # far too short to mean anything
            periods = [b - a for a, b in zip(t, t[1:])]
            full = st.median(periods) if periods else 0
            pw = [(row[0], row[1]) for k, row in enumerate(r["series"])
                  if k > 0 and periods[k - 1] >= 0.5 * full and row[1] == row[1]]
            ax[0].plot([a for a, _ in pw], [b for _, b in pw], color=COLOR[setup], lw=1, alpha=0.8,
                       label=LABEL[setup] if i == 0 else None)
            for sn, ls in (("ccd2_c", "-"), ("ccd4_c", "--")):
                if sn in cols:
                    k = cols.index(sn)
                    ax[1].plot(t, [row[k] for row in r["series"]], color=COLOR[setup], lw=1, ls=ls, alpha=0.8,
                               label=f"{LABEL[setup]}, {sn[:-2]}" if i == 0 else None)
    ax[0].set_title("package power (RAPL, socket 1)", fontsize=10)
    ax[0].set_xlabel("s (0 = start of measurement; before = warmup)")
    ax[0].set_ylabel("W")
    ax[1].set_title("worker CCD temperature (SMN): CCD1 = slot 2, CCD2 = slot 4", fontsize=10)
    ax[1].set_xlabel("s")
    ax[1].set_ylabel("C")
    ax[0].legend(fontsize=8, frameon=False, loc="lower right")
    ax[1].legend(fontsize=8, frameon=False, loc="upper center", bbox_to_anchor=(0.5, -0.15), ncol=2)

    # one quantity per panel: bars = mean, dots = runs
    metrics = [(ax[2], "throughput [tx/s ÷ 1000]", lambda r: r["tps"] / 1000),
               (ax[3], "worker cores [W, sum of 2]",
                lambda r: r["energy"].get("core_worker0", 0) + r["energy"].get("core_worker1", 0))]
    for a, mname, fn in metrics:
        for j, setup in enumerate(("same", "diff")):
            vals = [fn(r) for r in data[setup]]
            a.bar(j, st.mean(vals), width=0.7, color=COLOR[setup], alpha=0.85)
            a.scatter([j] * len(vals), vals, color="#333333", s=12, zorder=3)
            a.annotate(f"{st.mean(vals):.1f}", (j, st.mean(vals)), xytext=(0, 3), textcoords="offset points",
                       ha="center", fontsize=8)
        a.set_xticks([0, 1], ["same", "diff"])
        a.set_title(mname, fontsize=10)
    for a in ax:
        a.grid(axis="y", color="#BBBBBB", ls="--", lw=0.6)
        a.set_axisbelow(True)
        for side in ("top", "right"):
            a.spines[side].set_visible(False)
    fig.suptitle("TPC-C, 10 warehouses, 2 workers + load generator (cpu 28), gpu socket 1, performance governor",
                 fontsize=11)
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(f"{stem}.{ext}", dpi=150)
    print(f"\nwrote {stem}.png/pdf")


if __name__ == "__main__":
    main()
