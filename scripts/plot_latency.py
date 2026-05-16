#!/usr/bin/env python3
"""
plot_latency.py — Generate benchmark plots from latency CSV files.

Usage:
    python3 scripts/plot_latency.py [--results-dir results] [--output-dir results]

Expects CSVs with columns:
    timestamp_ns, message_size_bytes, transport_mode, publish_rate_hz, latency_ns

Transport mode encoding (matches TransportMode enum in csv_logger.hpp):
    0 = copy, 1 = loaned, 2 = shm
"""

import argparse
import glob
import os
import sys

import matplotlib
matplotlib.use("Agg")  # headless — no display required
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

TRANSPORT_LABELS = {0: "Copy", 1: "Loaned", 2: "SHM"}
TRANSPORT_COLORS = {0: "#e74c3c", 1: "#3498db", 2: "#2ecc71"}
RATES = [100, 500, 1000]


def load_all_csvs(results_dir: str) -> pd.DataFrame:
    pattern = os.path.join(results_dir, "latency_*.csv")
    files = glob.glob(pattern)
    if not files:
        print(f"No CSV files found in {results_dir}", file=sys.stderr)
        sys.exit(1)

    frames = []
    for f in files:
        try:
            df = pd.read_csv(f)
            df["source_file"] = os.path.basename(f)
            frames.append(df)
        except Exception as e:
            print(f"Warning: could not read {f}: {e}", file=sys.stderr)

    combined = pd.concat(frames, ignore_index=True)

    # Convert nanoseconds to microseconds for readability.
    combined["latency_us"] = combined["latency_ns"] / 1_000.0

    # Filter out negative latencies (clock skew artefacts) and extreme outliers.
    p999 = combined["latency_us"].quantile(0.999)
    combined = combined[(combined["latency_us"] > 0) & (combined["latency_us"] < p999 * 3)]

    print(f"Loaded {len(combined):,} records from {len(files)} files.")
    return combined


def plot_cdf(df: pd.DataFrame, output_dir: str) -> None:
    """One CDF plot per publish rate, overlaying all transport modes."""
    for rate in sorted(df["publish_rate_hz"].unique()):
        fig, ax = plt.subplots(figsize=(8, 5))
        subset = df[df["publish_rate_hz"] == rate]

        for mode_id, label in TRANSPORT_LABELS.items():
            mode_data = subset[subset["transport_mode"] == mode_id]["latency_us"]
            if mode_data.empty:
                continue
            sorted_data = np.sort(mode_data.values)
            cdf = np.arange(1, len(sorted_data) + 1) / len(sorted_data)
            ax.plot(sorted_data, cdf,
                    label=f"{label} (n={len(mode_data):,})",
                    color=TRANSPORT_COLORS.get(mode_id, "grey"),
                    linewidth=1.8)

        ax.axvline(x=50,  color="black", linestyle="--", linewidth=0.8,
                   label="50 µs target (SHM p99)")
        ax.axvline(x=200, color="black", linestyle=":",  linewidth=0.8,
                   label="200 µs target (Copy p99)")

        ax.set_xlabel("One-Way Latency (µs)", fontsize=12)
        ax.set_ylabel("CDF", fontsize=12)
        ax.set_title(f"Latency CDF — {rate} Hz", fontsize=13)
        ax.legend(fontsize=9)
        ax.set_xlim(left=0)
        ax.set_ylim(0, 1)
        ax.grid(True, alpha=0.3)
        fig.tight_layout()

        out_path = os.path.join(output_dir, f"cdf_{rate}Hz.png")
        fig.savefig(out_path, dpi=150)
        plt.close(fig)
        print(f"  Saved {out_path}")


def plot_latency_vs_rate(df: pd.DataFrame, output_dir: str) -> None:
    """Line chart: p50 and p99 latency vs publish rate, one line per transport mode."""
    stats = (
        df.groupby(["transport_mode", "publish_rate_hz"])["latency_us"]
        .agg(p50=lambda x: x.quantile(0.50),
             p99=lambda x: x.quantile(0.99))
        .reset_index()
    )

    fig, axes = plt.subplots(1, 2, figsize=(12, 5), sharey=False)

    for percentile, ax, title in [
        ("p50", axes[0], "Median (p50) Latency vs Publish Rate"),
        ("p99", axes[1], "p99 Latency vs Publish Rate"),
    ]:
        for mode_id, label in TRANSPORT_LABELS.items():
            mode_stats = stats[stats["transport_mode"] == mode_id]
            if mode_stats.empty:
                continue
            ax.plot(mode_stats["publish_rate_hz"], mode_stats[percentile],
                    marker="o", label=label,
                    color=TRANSPORT_COLORS.get(mode_id, "grey"),
                    linewidth=2)

        ax.set_xlabel("Publish Rate (Hz)", fontsize=11)
        ax.set_ylabel("Latency (µs)", fontsize=11)
        ax.set_title(title, fontsize=12)
        ax.legend(fontsize=9)
        ax.grid(True, alpha=0.3)
        ax.set_xticks(RATES)

    fig.tight_layout()
    out_path = os.path.join(output_dir, "latency_vs_rate.png")
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"  Saved {out_path}")


def plot_jitter_bar(df: pd.DataFrame, output_dir: str) -> None:
    """Bar chart: jitter = p99 - p50 per (transport_mode, publish_rate_hz)."""
    stats = (
        df.groupby(["transport_mode", "publish_rate_hz"])["latency_us"]
        .agg(p50=lambda x: x.quantile(0.50),
             p99=lambda x: x.quantile(0.99))
        .reset_index()
    )
    stats["jitter_us"] = stats["p99"] - stats["p50"]

    rates = sorted(stats["publish_rate_hz"].unique())
    x = np.arange(len(rates))
    width = 0.25

    fig, ax = plt.subplots(figsize=(9, 5))

    for i, (mode_id, label) in enumerate(TRANSPORT_LABELS.items()):
        mode_stats = stats[stats["transport_mode"] == mode_id]
        # Align bars to the rate positions.
        jitters = [
            mode_stats[mode_stats["publish_rate_hz"] == r]["jitter_us"].values[0]
            if r in mode_stats["publish_rate_hz"].values else 0.0
            for r in rates
        ]
        bars = ax.bar(x + i * width, jitters, width,
                      label=label,
                      color=TRANSPORT_COLORS.get(mode_id, "grey"),
                      alpha=0.85, edgecolor="white")
        for bar, val in zip(bars, jitters):
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.5,
                    f"{val:.1f}", ha="center", va="bottom", fontsize=8)

    ax.set_xlabel("Publish Rate (Hz)", fontsize=11)
    ax.set_ylabel("Jitter = p99 − p50 (µs)", fontsize=11)
    ax.set_title("Scheduling Jitter per Transport Mode", fontsize=12)
    ax.set_xticks(x + width)
    ax.set_xticklabels([f"{r} Hz" for r in rates])
    ax.legend(fontsize=9)
    ax.grid(True, axis="y", alpha=0.3)
    fig.tight_layout()

    out_path = os.path.join(output_dir, "jitter_bar.png")
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"  Saved {out_path}")


def print_summary_table(df: pd.DataFrame) -> None:
    print("\n── Latency Summary Table ─────────────────────────────────────────")
    print(f"{'Transport':<10} {'Rate (Hz)':<12} {'p50 (µs)':<12} "
          f"{'p99 (µs)':<12} {'max (µs)':<12} {'n':<8}")
    print("─" * 68)

    stats = (
        df.groupby(["transport_mode", "publish_rate_hz"])["latency_us"]
        .agg(p50=lambda x: x.quantile(0.50),
             p99=lambda x: x.quantile(0.99),
             maximum="max",
             count="count")
        .reset_index()
    )

    for _, row in stats.iterrows():
        label = TRANSPORT_LABELS.get(int(row["transport_mode"]), "?")
        print(f"{label:<10} {int(row['publish_rate_hz']):<12} "
              f"{row['p50']:<12.1f} {row['p99']:<12.1f} "
              f"{row['maximum']:<12.1f} {int(row['count']):<8}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot ROS2 RT middleware latency results")
    parser.add_argument("--results-dir", default="results",
                        help="Directory containing latency CSV files")
    parser.add_argument("--output-dir",  default="results",
                        help="Directory for output PNG plots")
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    df = load_all_csvs(args.results_dir)
    print_summary_table(df)

    print("\nGenerating plots...")
    plot_cdf(df, args.output_dir)
    plot_latency_vs_rate(df, args.output_dir)
    plot_jitter_bar(df, args.output_dir)

    print("\nDone. Plots written to:", args.output_dir)


if __name__ == "__main__":
    main()
