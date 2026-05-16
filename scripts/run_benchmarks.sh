#!/usr/bin/env bash
# run_benchmarks.sh — Full benchmark automation script.
#
# Runs publisher + subscriber pairs for each transport mode and rate, then
# calls the Python plotting script.
#
# Prerequisites:
#   source /opt/ros/humble/setup.bash
#   source install/setup.bash
#   sudo setcap cap_sys_nice+ep install/lib/ros2_rt_middleware/latency_publisher
#
# Usage:
#   bash scripts/run_benchmarks.sh [--duration <s>] [--core <n>]

set -euo pipefail

DURATION=${DURATION:-10}
CPU_CORE=${CPU_CORE:--1}
RESULTS_DIR="results"
RMW=${RMW_IMPLEMENTATION:-rmw_cyclonedds_cpp}

while [[ $# -gt 0 ]]; do
  case $1 in
    --duration) DURATION="$2"; shift 2 ;;
    --core)     CPU_CORE="$2"; shift 2 ;;
    *) echo "Unknown arg: $1"; exit 1 ;;
  esac
done

mkdir -p "$RESULTS_DIR"

modes=(copy loaned shm)
rates=(100 500 1000)

for mode in "${modes[@]}"; do
  for rate in "${rates[@]}"; do
    echo "────────────────────────────────────────────────────────────"
    echo " Transport: $mode  |  Rate: ${rate} Hz  |  Duration: ${DURATION}s"
    echo "────────────────────────────────────────────────────────────"

    CSV="${RESULTS_DIR}/latency_${mode}_${rate}Hz.csv"

    # Launch subscriber in background first (so it's ready before publisher).
    RMW_IMPLEMENTATION="$RMW" ros2 run ros2_rt_middleware latency_subscriber \
      --rate "$rate" --mode "$mode" --output "$CSV" &
    SUB_PID=$!
    sleep 0.5  # Give subscriber time to bind.

    # Launch publisher.
    RMW_IMPLEMENTATION="$RMW" ros2 run ros2_rt_middleware latency_publisher \
      --rate "$rate" --mode "$mode" &
    PUB_PID=$!

    sleep "$DURATION"

    kill "$PUB_PID" 2>/dev/null || true
    kill "$SUB_PID" 2>/dev/null || true
    wait "$PUB_PID" 2>/dev/null || true
    wait "$SUB_PID" 2>/dev/null || true

    echo "  → Saved $CSV"
    sleep 1
  done
done

echo ""
echo "All runs complete. Generating plots..."
python3 scripts/plot_latency.py --results-dir "$RESULTS_DIR" --output-dir "$RESULTS_DIR"
echo "Done."
