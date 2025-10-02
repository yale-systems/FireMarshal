#!/usr/bin/env bash
# run_udp_servers.sh — start N udp_server processes on ports 9000..9000+N-1
# Clean shutdown: Ctrl-C kills all children (INT → TERM → KILL).

set -euo pipefail

[[ $# -eq 1 ]] || { echo "Usage: $0 <N>"; exit 1; }
N="$1"
[[ "$N" =~ ^[0-9]+$ && "$N" -gt 0 ]] || { echo "Error: N must be a positive integer"; exit 1; }

BASE_PORT=9000
declare -a PIDS=()

shutdown() {
  echo
  echo "[run_udp_servers] Caught signal — stopping ${#PIDS[@]} server(s)..."

  # 1) Ask nicely
  kill -INT "${PIDS[@]}" 2>/dev/null || true
  sleep 1

  # 2) Ask firmly
  kill -TERM "${PIDS[@]}" 2>/dev/null || true
  sleep 1

  # 3) Nuke anything left, by process group (covers any grandchildren)
  # shellcheck disable=SC2046
  kill -KILL -$$ 2>/dev/null || true

  # Drain waits
  wait || true
  echo "[run_udp_servers] All servers stopped."
}

# Trap Ctrl-C / TERM and also run on script exit to avoid orphans
trap shutdown INT TERM
trap shutdown EXIT

for ((i=0; i<N; i++)); do
  PORT=$((BASE_PORT + i))
  echo "Launching: ./udp_server 0.0.0.0 $PORT"
  ./udp_server 0.0.0.0 "$PORT" &
  PIDS+=($!)
done

echo "Started $N server(s) on ports ${BASE_PORT}..$((BASE_PORT + N - 1)). Press Ctrl-C to stop."
wait
