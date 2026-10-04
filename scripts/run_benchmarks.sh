#!/usr/bin/env bash
# run_benchmarks.sh — cross-process benchmark matrix.
#
# Runs the publisher and subscriber as separate processes (the realistic
# deployment, and the case where the SHM / loaned paths matter most) for every
# executor × transport × rate, then calls plot_latency.py.
#
# For the single-process matrix use:  ros2 run ros2_rt_middleware benchmark_runner
#
# Prerequisites:
#   source /opt/ros/humble/setup.bash
#   source install/setup.bash
#   # optional, for SCHED_FIFO without sudo:
#   sudo setcap cap_sys_nice,cap_ipc_lock+ep install/ros2_rt_middleware/lib/ros2_rt_middleware/latency_publisher
#
# Usage:
#   bash scripts/run_benchmarks.sh [--duration s] [--core n] [--results-dir dir]
#                                  [--executors "stock rt"] [--modes "copy loaned shm"]
#                                  [--rates "100 500 1000"] [--sched fifo|rr|other]
#                                  [--prio n] [--busy-wait]
# Every option can also be set via the matching upper-case environment
# variable (DURATION, CPU_CORE, RESULTS_DIR, EXECUTORS, MODES, RATES, SCHED, PRIO).

set -euo pipefail

DURATION=${DURATION:-10}
CPU_CORE=${CPU_CORE:--1}
RESULTS_DIR=${RESULTS_DIR:-results}
EXECUTORS=${EXECUTORS:-"stock rt"}
MODES=${MODES:-"copy loaned shm"}
RATES=${RATES:-"100 500 1000"}
SCHED=${SCHED:-fifo}
PRIO=${PRIO:-80}
BUSY_WAIT=""

while [[ $# -gt 0 ]]; do
  case $1 in
    --duration)    DURATION="$2"; shift 2 ;;
    --core)        CPU_CORE="$2"; shift 2 ;;
    --results-dir) RESULTS_DIR="$2"; shift 2 ;;
    --executors)   EXECUTORS="$2"; shift 2 ;;
    --modes)       MODES="$2"; shift 2 ;;
    --rates)       RATES="$2"; shift 2 ;;
    --sched)       SCHED="$2"; shift 2 ;;
    --prio)        PRIO="$2"; shift 2 ;;
    --busy-wait)   BUSY_WAIT="--busy-wait"; shift ;;
    -h|--help)     sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

if ! command -v ros2 >/dev/null 2>&1; then
  echo "ros2 not found — source /opt/ros/humble/setup.bash and install/setup.bash first" >&2
  exit 1
fi
BIN="$(ros2 pkg prefix ros2_rt_middleware)/lib/ros2_rt_middleware"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$RESULTS_DIR"
{
  echo "kernel=$(uname -r)"
  echo "kernel_version=$(uname -v)"
  echo "machine=$(uname -m)"
  echo "preempt_rt=$([[ "$(cat /sys/kernel/realtime 2>/dev/null)" == 1 ]] && echo yes || echo no)"
  echo "cpus=$(nproc)"
  echo "rmw=${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp (default)}"
  echo "rlimit_rtprio=$(ulimit -r)"
  echo "rlimit_memlock=$(ulimit -l)"
  echo "cpu_governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
  echo "isolcpus=$(grep -q isolcpus /proc/cmdline && echo yes || echo no)"
  echo "args=--duration $DURATION --core $CPU_CORE --executors '$EXECUTORS' --modes '$MODES' --rates '$RATES' --sched $SCHED --prio $PRIO $BUSY_WAIT"
} > "$RESULTS_DIR/env.txt"
SUB_PID=""
cleanup() { [[ -n "$SUB_PID" ]] && kill -INT "$SUB_PID" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

for exec in $EXECUTORS; do
  for mode in $MODES; do
    for rate in $RATES; do
      tag="xproc_${exec}_${mode}_${rate}Hz"
      echo "────────────────────────────────────────────────────────────"
      echo " $exec executor | $mode | ${rate} Hz | ${DURATION} s"
      echo "────────────────────────────────────────────────────────────"

      # Subscriber starts first and outlives the publisher by a second on
      # each side, so no samples are missed at either end. Both processes
      # exit on their own and flush their CSVs.
      "$BIN/latency_subscriber" --rate "$rate" --mode "$mode" \
        --output "$RESULTS_DIR/latency_${tag}.csv" \
        --duration "$((DURATION + 2))" &
      SUB_PID=$!
      sleep 1

      "$BIN/latency_publisher" --rate "$rate" --mode "$mode" --executor "$exec" \
        --core "$CPU_CORE" --sched "$SCHED" --prio "$PRIO" $BUSY_WAIT \
        --duration "$DURATION" --jitter-out "$RESULTS_DIR/jitter_${tag}.csv"

      wait "$SUB_PID" || true
      SUB_PID=""
      sleep 1
    done
  done
done

echo ""
echo "All runs complete. Generating plots..."
python3 "$SCRIPT_DIR/plot_latency.py" --results-dir "$RESULTS_DIR" --output-dir "$RESULTS_DIR"
echo "Done."
