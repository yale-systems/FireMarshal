#!/usr/bin/env bash
# run_servers.sh — launch N RDMA servers on consecutive ports, pinning to CPUs 0-3; forward Ctrl-C to all

set -euo pipefail

IP="20.0.0.2"
BASE_PORT=9000

usage() {
  echo "Usage: $0 <nproc> <bytes>"
  echo "Example: $0 4 128"
  exit 1
}

[[ $# -eq 2 ]] || usage

NPROC="$1"
BYTES="$2"

[[ "$NPROC" =~ ^[0-9]+$ ]] && (( NPROC > 0 )) || usage
[[ "$BYTES" =~ ^[0-9]+$ ]] && (( BYTES > 0 )) || usage

declare -a PIDS=()

# Forward Ctrl-C/TERM to all children
forward_sig() {
  echo
  echo "Caught signal; forwarding SIGINT to ${#PIDS[@]} server(s)..."
  for pid in "${PIDS[@]}"; do
    kill -INT "$pid" 2>/dev/null || true
  done
}
trap forward_sig INT TERM

# Check for taskset
HAVE_TASKSET=1
if ! command -v taskset >/dev/null 2>&1; then
  echo "WARNING: 'taskset' not found; running without CPU pinning." >&2
  HAVE_TASKSET=0
fi

echo "Launching $NPROC server(s) with size $BYTES starting at port $BASE_PORT (IP $IP)"
for ((i=0; i<NPROC; i++)); do
  port=$((BASE_PORT + i))
  cpu=$(( i % 4 ))   # CPUs 0-3 round-robin

  if (( HAVE_TASKSET )); then
    echo "  -> CPU $cpu : ./server_ts -a $IP -p $port -s $BYTES"
    taskset -c "${cpu}" ./server_ts -a "$IP" -p "$port" -s "$BYTES" &
  else
    echo "  -> ./server -a $IP -p $port -s $BYTES"
    ./server_ts -a "$IP" -p "$port" -s "$BYTES" &
  fi

  PIDS+=("$!")
done

# Wait for all to finish
status=0
for pid in "${PIDS[@]}"; do
  if ! wait "$pid"; then
    status=1
  fi
done

exit "$status"
