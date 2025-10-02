#!/usr/bin/env bash
# run_clients.sh — launch N RDMA clients on consecutive ports; rotate across given CPU cores

set -euo pipefail

IP="20.0.0.1"
BASE_PORT=9000

usage() {
  cat <<EOF
Usage: $0 <nproc> <bytes> [cores]
  nproc : number of clients to launch
  bytes : message size
  cores : optional core list/range (default: 0-3). Examples:
          "0-3" or "0,1,2,3" or "0-1,4,6-7"
Example:
  $0 8 128
  $0 8 128 0-3
  $0 8 128 0,2,4,6
EOF
  exit 1
}

[[ $# -ge 2 && $# -le 3 ]] || usage

NPROC="$1"
BYTES="$2"
CORES_SPEC="${3:-0-3}"

[[ "$NPROC" =~ ^[0-9]+$ ]] && (( NPROC > 0 )) || usage
[[ "$BYTES" =~ ^[0-9]+$ ]] && (( BYTES > 0 )) || usage

# --- Expand cores spec like "0-3,8,10-11" into an array ---
expand_cores() {
  local spec="$1"
  local out=()
  IFS=',' read -r -a parts <<< "$spec"
  for p in "${parts[@]}"; do
    if [[ "$p" =~ ^([0-9]+)-([0-9]+)$ ]]; then
      local a="${BASH_REMATCH[1]}" b="${BASH_REMATCH[2]}"
      if (( b < a )); then echo "Invalid core range: $p" >&2; exit 1; fi
      for c in $(seq "$a" "$b"); do out+=("$c"); done
    elif [[ "$p" =~ ^[0-9]+$ ]]; then
      out+=("$p")
    else
      echo "Invalid core token: $p" >&2; exit 1
    fi
  done
  printf '%s\n' "${out[@]}"
}

mapfile -t CORES < <(expand_cores "$CORES_SPEC")
(( ${#CORES[@]} > 0 )) || { echo "No cores parsed from '$CORES_SPEC'"; exit 1; }

# Prefer taskset; fall back to numactl; or no pinning if neither exists
PIN_CMD=""
if command -v taskset >/dev/null 2>&1; then
  PIN_CMD="taskset -c"
elif command -v numactl >/dev/null 2>&1; then
  PIN_CMD="numactl --physcpubind"
else
  echo "Warning: neither taskset nor numactl found; running without CPU pinning." >&2
fi

declare -a PIDS=()

forward_sigint() {
  echo
  echo "Caught signal; forwarding SIGINT to ${#PIDS[@]} client(s)..."
  for pid in "${PIDS[@]}"; do
    kill -INT "$pid" 2>/dev/null || true
  done
}
trap forward_sigint INT TERM

echo "Launching $NPROC client(s) with size $BYTES starting at port $BASE_PORT (IP $IP)"
echo "CPU cores (round-robin): ${CORES[*]}"

for ((i=0; i<NPROC; i++)); do
  port=$((BASE_PORT + i))
  core="${CORES[$(( i % ${#CORES[@]} ))]}"
  if [[ -n "$PIN_CMD" ]]; then
    echo "  -> [core $core] ./client -a $IP -p $port -s $BYTES"
    $PIN_CMD "$core" ./client -a "$IP" -p "$port" -s "$BYTES" &
  else
    echo "  -> ./client -a $IP -p $port -s $BYTES"
    ./client -a "$IP" -p "$port" -s "$BYTES" &
  fi
  PIDS+=("$!")
done

status=0
for pid in "${PIDS[@]}"; do
  if ! wait "$pid"; then status=1; fi
done
exit "$status"
