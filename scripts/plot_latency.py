#!/usr/bin/env python3
"""
plot_latency.py — Analyse benchmark_runner / run_benchmarks.sh output.

Usage:
    python3 scripts/plot_latency.py [--results-dir results] [--output-dir results]
                                    [--warmup-s 1.0]

Inputs (in --results-dir):
    latency_<exec>_<mode>_<rate>Hz.csv   one row per received sample
        timestamp_ns,message_size_bytes,transport_mode,publish_rate_hz,
        latency_ns,message_type,seq
    jitter_<exec>_<mode>_<rate>Hz.csv    publisher wake-up lateness (metric,value_ns)
    summary.csv, env.txt                 optional run metadata

    <exec> is "stock" or "rt"; cross-process runs use an "xproc_" prefix
    (e.g. latency_xproc_rt_shm_1000Hz.csv). Legacy files named
    latency_<mode>_<rate>Hz.csv (no message_type column) are also accepted.

Outputs (in --output-dir):
    latency_cdf_<rate>Hz.png   joint-sample latency CDF per transport/executor
    latency_vs_rate.png        p50 and p99 joint-sample latency vs publish rate
    wakeup_jitter.png          publisher wake-up lateness, stock vs RT executor
    summary.md                 markdown tables (latency, jitter, run health)
"""

import argparse
import glob
import os
import re
import sys

import matplotlib

matplotlib.use("Agg")  # headless — no display required
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

MODES = ["copy", "loaned", "shm"]
MODE_IDS = {0: "copy", 1: "loaned", 2: "shm"}
MODE_LABELS = {"copy": "Copy", "loaned": "Loaned", "shm": "SHM"}
MSG_LABELS = {0: "joint (96 B)", 1: "cloud (3840 B)"}

# Validated categorical slots 1–3 (all-pairs CVD-safe). Identity is never
# colour-alone: executors also differ by line style, and every chart has a legend.
MODE_COLORS = {"copy": "#2a78d6", "loaned": "#eb6834", "shm": "#1baf7a"}
EXEC_COLORS = {"stock": "#eb6834", "rt": "#2a78d6"}
EXEC_STYLES = {"stock": "--", "rt": "-", "legacy": ":"}
EXEC_LABELS = {"stock": "stock executor", "rt": "RT executor", "legacy": "legacy run"}

SURFACE = "#fcfcfb"
TEXT_PRIMARY = "#0b0b0b"
TEXT_SECONDARY = "#52514e"
GRID = "#e4e3df"

FILE_RE = re.compile(
    r"^(?P<kind>latency|jitter)_(?:(?P<scope>xproc)_)?(?:(?P<exec>stock|rt)_)?"
    r"(?P<mode>copy|loaned|shm)_(?P<rate>\d+)Hz\.csv$"
)


def style_axes(ax):
    ax.set_facecolor(SURFACE)
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(TEXT_SECONDARY)
    ax.tick_params(colors=TEXT_SECONDARY, labelsize=9)
    ax.xaxis.label.set_color(TEXT_PRIMARY)
    ax.yaxis.label.set_color(TEXT_PRIMARY)
    ax.title.set_color(TEXT_PRIMARY)


def new_figure(*args, **kwargs):
    fig, ax = plt.subplots(*args, **kwargs)
    fig.patch.set_facecolor(SURFACE)
    for a in np.atleast_1d(ax):
        style_axes(a)
    return fig, ax


def parse_name(path):
    m = FILE_RE.match(os.path.basename(path))
    if not m:
        return None
    d = m.groupdict()
    return {
        "kind": d["kind"],
        "scope": d["scope"] or "inproc",
        "executor": d["exec"] or "legacy",
        "mode": d["mode"],
        "rate": int(d["rate"]),
    }


def load_latency(results_dir, warmup_s):
    frames = []
    for path in sorted(glob.glob(os.path.join(results_dir, "latency_*.csv"))):
        meta = parse_name(path)
        if meta is None:
            print(f"  skipping unrecognised file {path}", file=sys.stderr)
            continue
        try:
            df = pd.read_csv(path)
        except Exception as e:  # noqa: BLE001 — report and keep going
            print(f"  warning: could not read {path}: {e}", file=sys.stderr)
            continue
        if df.empty:
            continue
        if "message_type" not in df.columns:  # legacy schema
            df["message_type"] = np.where(df["message_size_bytes"] > 1000, 1, 0)
        # Discard the warm-up window (discovery, page faults, cache warm-up).
        t0 = df["timestamp_ns"].min()
        df = df[df["timestamp_ns"] >= t0 + warmup_s * 1e9]
        for k, v in meta.items():
            if k != "kind":
                df[k] = v
        frames.append(df)

    if not frames:
        print(f"No latency CSV files found in {results_dir}", file=sys.stderr)
        sys.exit(1)

    df = pd.concat(frames, ignore_index=True)
    df["latency_us"] = df["latency_ns"] / 1_000.0
    bad = (df["latency_us"] <= 0).sum()
    if bad:
        print(f"  note: dropped {bad} non-positive latencies", file=sys.stderr)
    df = df[df["latency_us"] > 0]
    print(f"Loaded {len(df):,} latency samples from {len(frames)} files "
          f"(first {warmup_s:g} s of each run discarded).")
    return df


def load_jitter(results_dir, warmup_s):
    frames = []
    for path in sorted(glob.glob(os.path.join(results_dir, "jitter_*.csv"))):
        meta = parse_name(path)
        if meta is None:
            continue
        df = pd.read_csv(path)
        df = df[df["metric"] == "wakeup"].reset_index(drop=True)
        # Samples are in release order; drop the warm-up releases.
        df = df.iloc[int(warmup_s * meta["rate"]):]
        for k, v in meta.items():
            if k != "kind":
                df[k] = v
        frames.append(df)
    if not frames:
        return pd.DataFrame()
    df = pd.concat(frames, ignore_index=True)
    df["lateness_us"] = df["value_ns"] / 1_000.0
    return df


def pct(series, q):
    return float(np.percentile(series, q)) if len(series) else float("nan")


def latency_table(df):
    rows = []
    keys = ["scope", "executor", "mode", "rate", "message_type"]
    for key, g in df.groupby(keys):
        x = g["latency_us"].values
        rows.append(dict(zip(keys, key), n=len(x), p50=pct(x, 50), p99=pct(x, 99),
                         p999=pct(x, 99.9), max=float(x.max())))
    return pd.DataFrame(rows)


def jitter_table(jdf):
    rows = []
    keys = ["scope", "executor", "mode", "rate"]
    for key, g in jdf.groupby(keys):
        x = g["lateness_us"].values
        rows.append(dict(zip(keys, key), n=len(x), p50=pct(x, 50), p99=pct(x, 99),
                         p999=pct(x, 99.9), max=float(x.max())))
    return pd.DataFrame(rows)


# ── plots ─────────────────────────────────────────────────────────────────────

def plot_cdf(df, output_dir):
    """Joint-sample latency CDF per rate: colour = transport, style = executor."""
    joint = df[(df["message_type"] == 0) & (df["scope"] == "inproc")]
    for rate in sorted(joint["rate"].unique()):
        sub = joint[joint["rate"] == rate]
        fig, ax = new_figure(figsize=(8, 5))
        xmax = 0.0
        for executor in ["rt", "stock", "legacy"]:
            for mode in MODES:
                x = sub[(sub["executor"] == executor) & (sub["mode"] == mode)]["latency_us"].values
                if len(x) == 0:
                    continue
                xs = np.sort(x)
                ys = np.arange(1, len(xs) + 1) / len(xs)
                ax.plot(xs, ys, color=MODE_COLORS[mode], linestyle=EXEC_STYLES[executor],
                        linewidth=2,
                        label=f"{MODE_LABELS[mode]}, {EXEC_LABELS[executor]} (n={len(xs):,})")
                xmax = max(xmax, pct(xs, 99.5))
        ax.set_xlim(0, xmax * 1.05 if xmax else None)
        ax.set_ylim(0, 1)
        ax.set_xlabel("One-way latency (µs), joint samples")
        ax.set_ylabel("Fraction of samples ≤ x")
        ax.set_title(f"Latency CDF at {rate} Hz (x-axis clipped at p99.5)", fontsize=12)
        ax.legend(fontsize=8, frameon=False, labelcolor=TEXT_PRIMARY)
        fig.tight_layout()
        out = os.path.join(output_dir, f"latency_cdf_{rate}Hz.png")
        fig.savefig(out, dpi=150, facecolor=SURFACE)
        plt.close(fig)
        print(f"  Saved {out}")


def plot_latency_vs_rate(lat, output_dir):
    """p50 and p99 (joint samples) vs rate — two panels, one shared unit."""
    t = lat[(lat["message_type"] == 0) & (lat["scope"] == "inproc")]
    if t.empty:
        return
    fig, axes = new_figure(1, 2, figsize=(12, 5))
    for ax, col, title in [(axes[0], "p50", "Median (p50)"), (axes[1], "p99", "p99")]:
        for executor in ["rt", "stock", "legacy"]:
            for mode in MODES:
                s = t[(t["executor"] == executor) & (t["mode"] == mode)].sort_values("rate")
                if s.empty:
                    continue
                ax.plot(s["rate"], s[col], color=MODE_COLORS[mode],
                        linestyle=EXEC_STYLES[executor], marker="o", markersize=7,
                        linewidth=2, label=f"{MODE_LABELS[mode]}, {EXEC_LABELS[executor]}")
        ax.set_xlabel("Publish rate (Hz)")
        ax.set_ylabel("One-way latency (µs)")
        ax.set_title(f"{title} joint-sample latency vs rate", fontsize=12)
        ax.set_xticks(sorted(t["rate"].unique()))
        ax.set_ylim(bottom=0)
    axes[1].legend(fontsize=8, frameon=False, labelcolor=TEXT_PRIMARY)
    fig.tight_layout()
    out = os.path.join(output_dir, "latency_vs_rate.png")
    fig.savefig(out, dpi=150, facecolor=SURFACE)
    plt.close(fig)
    print(f"  Saved {out}")


def plot_wakeup_jitter(jdf, output_dir):
    """Publisher wake-up lateness: stock vs RT, pooled over transports."""
    j = jdf[jdf["scope"] == "inproc"]
    if j.empty or set(j["executor"]) - {"stock", "rt"}:
        j = j[j["executor"].isin(["stock", "rt"])]
    if j.empty:
        return
    rates = sorted(j["rate"].unique())
    stats = ["p50", "p99", "p99.9"]
    qs = [50, 99, 99.9]
    fig, axes = new_figure(1, len(stats), figsize=(4.2 * len(stats), 4.6), sharey=False)
    width = 0.38
    x = np.arange(len(rates))
    for ax, name, q in zip(axes, stats, qs):
        for i, executor in enumerate(["stock", "rt"]):
            vals = []
            for r in rates:
                s = j[(j["executor"] == executor) & (j["rate"] == r)]["lateness_us"].values
                vals.append(pct(s, q) if len(s) else 0.0)
            bars = ax.bar(x + (i - 0.5) * width, vals, width * 0.92,
                          color=EXEC_COLORS[executor], label=EXEC_LABELS[executor],
                          edgecolor=SURFACE, linewidth=2,
                          hatch="//" if executor == "stock" else None)
            for b, v in zip(bars, vals):
                ax.annotate(f"{v:.0f}", (b.get_x() + b.get_width() / 2, b.get_height()),
                            ha="center", va="bottom", fontsize=8, color=TEXT_SECONDARY,
                            xytext=(0, 2), textcoords="offset points")
        ax.set_xticks(x)
        ax.set_xticklabels([f"{r} Hz" for r in rates])
        ax.set_ylabel("Wake-up lateness (µs)")
        ax.set_title(f"{name} lateness", fontsize=12)
        ax.set_ylim(bottom=0)
    axes[0].legend(fontsize=9, frameon=False, labelcolor=TEXT_PRIMARY)
    fig.suptitle("Publisher wake-up lateness (release → callback start), all transports pooled",
                 fontsize=12, color=TEXT_PRIMARY)
    fig.tight_layout()
    out = os.path.join(output_dir, "wakeup_jitter.png")
    fig.savefig(out, dpi=150, facecolor=SURFACE)
    plt.close(fig)
    print(f"  Saved {out}")


# ── summary ───────────────────────────────────────────────────────────────────

def fmt(v, digits=1):
    return "–" if pd.isna(v) else f"{v:,.{digits}f}"


def write_summary(lat, jit, results_dir, output_dir, warmup_s):
    lines = ["# Benchmark summary", ""]

    env_path = os.path.join(results_dir, "env.txt")
    if os.path.exists(env_path):
        lines += ["## Environment", "", "```"]
        with open(env_path) as f:
            lines += [l.rstrip() for l in f if l.strip()]
        lines += ["```", ""]

    lines += [f"First {warmup_s:g} s of every run discarded. Latencies in µs.", ""]

    lines += ["## One-way message latency", "",
              "| Scope | Executor | Transport | Rate (Hz) | Message | n | p50 | p99 | p99.9 | max |",
              "|---|---|---|--:|---|--:|--:|--:|--:|--:|"]
    order = {"rt": 0, "stock": 1, "legacy": 2}
    lat = lat.assign(_o=lat["executor"].map(order)).sort_values(
        ["scope", "_o", "mode", "rate", "message_type"])
    for _, r in lat.iterrows():
        lines.append(
            f"| {r['scope']} | {r['executor']} | {MODE_LABELS[r['mode']]} | {r['rate']} | "
            f"{MSG_LABELS.get(int(r['message_type']), '?')} | {int(r['n']):,} | {fmt(r['p50'])} | "
            f"{fmt(r['p99'])} | {fmt(r['p999'])} | {fmt(r['max'])} |")
    lines.append("")

    if not jit.empty:
        lines += ["## Publisher wake-up lateness", "",
                  "| Scope | Executor | Transport | Rate (Hz) | n | p50 | p99 | p99.9 | max |",
                  "|---|---|---|--:|--:|--:|--:|--:|--:|"]
        jit = jit.assign(_o=jit["executor"].map(order)).sort_values(["scope", "_o", "mode", "rate"])
        for _, r in jit.iterrows():
            lines.append(
                f"| {r['scope']} | {r['executor']} | {MODE_LABELS[r['mode']]} | {r['rate']} | "
                f"{int(r['n']):,} | {fmt(r['p50'])} | {fmt(r['p99'])} | {fmt(r['p999'])} | "
                f"{fmt(r['max'])} |")
        lines.append("")

    summary_path = os.path.join(results_dir, "summary.csv")
    if os.path.exists(summary_path):
        s = pd.read_csv(summary_path)
        lines += ["## Run health", "",
                  "| Executor | Transport | Rate (Hz) | Achieved (Hz) | Published | Received | Lost "
                  "| Logger drops | Loans (pub/sub) | RT sched applied | Missed releases |",
                  "|---|---|--:|--:|--:|--:|--:|--:|---|---|--:|"]
        for _, r in s.iterrows():
            achieved = fmt(r["achieved_rate_hz"]) if "achieved_rate_hz" in s.columns else "–"
            sched = "n/a" if r["executor"] == "stock" else ("yes" if r["sched_ok"] else "no")
            lines.append(
                f"| {r['executor']} | {MODE_LABELS.get(r['mode'], r['mode'])} | {r['rate_hz']} | "
                f"{achieved} | {int(r['published']):,} | {int(r['received']):,} | {int(r['lost']):,} | "
                f"{int(r['logger_dropped']):,} | {'yes' if r['loans_pub'] else 'no'}/"
                f"{'yes' if r['loans_sub'] else 'no'} | {sched} | {int(r['wake_missed']):,} |")
        lines.append("")

    out = os.path.join(output_dir, "summary.md")
    with open(out, "w") as f:
        f.write("\n".join(lines))
    print(f"  Saved {out}")


def main():
    parser = argparse.ArgumentParser(description="Plot ROS2 RT middleware benchmark results")
    parser.add_argument("--results-dir", "--results", default="results",
                        help="Directory containing latency_*.csv / jitter_*.csv")
    parser.add_argument("--output-dir", "--output", default=None,
                        help="Directory for PNGs and summary.md (default: --results-dir)")
    parser.add_argument("--warmup-s", type=float, default=1.0,
                        help="Seconds discarded from the start of every run")
    args = parser.parse_args()
    output_dir = args.output_dir or args.results_dir
    os.makedirs(output_dir, exist_ok=True)

    df = load_latency(args.results_dir, args.warmup_s)
    jdf = load_jitter(args.results_dir, args.warmup_s)
    lat = latency_table(df)
    jit = jitter_table(jdf) if not jdf.empty else pd.DataFrame()

    print("\nGenerating plots...")
    plot_cdf(df, output_dir)
    plot_latency_vs_rate(lat, output_dir)
    if not jdf.empty:
        plot_wakeup_jitter(jdf, output_dir)
    write_summary(lat, jit, args.results_dir, output_dir, args.warmup_s)
    print("\nDone. Output written to:", output_dir)


if __name__ == "__main__":
    main()
