#!/bin/sh
set -eu

# Defaults (override via env or flags)
BIN=${BIN:-./udp_exp}
PRIO=${PRIO:-5}
SRC_MAC=${SRC_MAC:-00:0a:35:06:4d:e2}
SRC_IP=${SRC_IP:-10.0.0.1}
DST_MAC=${DST_MAC:-b4:96:91:90:3c:81}
DST_IP=${DST_IP:-10.0.0.2}
N=${N:-4}
BASE=${BASE:-1200}
CPUSET=${CPUSET:-3}     # CPUs to rotate over (e.g., "0-3" or "1" or "0-1,4,6-7")
RING_BASE=${RING_BASE:-0}
MAX_RINGS=64

usage() {
  cat <<EOF
Usage: $0 [-n N] [-p BASE] [-C CPUSET] [-R RING_BASE]
  -n N          Number of instances (default: $N)
  -p BASE       Base UDP port (default: $BASE) ; src-port == dst-port == BASE+i
  -C CPUSET     CPU set to rotate (e.g. "1", "0-3", "0-1,4,6-7")
  -R RING_BASE  Starting ring index (default: $RING_BASE), each instance uses a unique ring

Env overrides: BIN PRIO SRC_MAC SRC_IP DST_MAC DST_IP N BASE CPUSET RING_BASE
Example:
  BIN=./udp_exp PRIO=5 $0 -n 32 -p 1200 -C 0-3 -R 0
EOF
}

# Parse flags
while getopts "n:p:C:R:h" opt; do
  case "$opt" in
    n) N="$OPTARG" ;;
    p) BASE="$OPTARG" ;;
    C) CPUSET="$OPTARG" ;;
    R) RING_BASE="$OPTARG" ;;
    h) usage; exit 0 ;;
    \?) usage >&2; exit 1 ;;
  esac
done

# Basic validation
case "$N" in ''|*[!0-9]*) echo "N must be an integer" >&2; exit 1;; esac
case "$BASE" in ''|*[!0-9]*) echo "BASE must be an integer" >&2; exit 1;; esac
case "$RING_BASE" in ''|*[!0-9]*) echo "RING_BASE must be an integer" >&2; exit 1;; esac

if [ "$BASE" -lt 1 ] || [ "$BASE" -gt 65535 ]; then
  echo "BASE must be 1..65535" >&2; exit 1
fi
end_port=$((BASE + N - 1))
if [ "$end_port" -gt 65535 ]; then
  echo "Error: last port $end_port exceeds 65535" >&2; exit 1
fi

if [ "$RING_BASE" -lt 0 ] || [ "$RING_BASE" -ge "$MAX_RINGS" ]; then
  echo "RING_BASE must be 0..$((MAX_RINGS-1))" >&2; exit 1
fi
last_ring=$((RING_BASE + N - 1))
if [ "$last_ring" -ge "$MAX_RINGS" ]; then
  echo "Error: need $N unique rings starting at $RING_BASE, but max ring is $((MAX_RINGS-1))" >&2
  exit 1
fi

# Ensure chrt exists
if ! command -v chrt >/dev/null 2>&1; then
  echo "chrt not found in PATH" >&2; exit 1
fi

# ---- Parse CPUSET into a space-separated CPUS list (supports ranges & commas) ----
parse_cpuset() {
  CPUS=""
  s="$1"
  oldifs=$IFS
  IFS=,
  set -- $s
  IFS=$oldifs
  for part in "$@"; do
    case "$part" in
      *-*)
        start=${part%%-*}
        finish=${part#*-}
        case "$start" in ''|*[!0-9]*) echo "Bad CPU range start: $start" >&2; exit 1;; esac
        case "$finish" in ''|*[!0-9]*) echo "Bad CPU range end: $finish" >&2; exit 1;; esac
        if [ "$start" -gt "$finish" ]; then
          echo "Bad CPU range: $part" >&2; exit 1
        fi
        n="$start"
        while [ "$n" -le "$finish" ]; do
          CPUS="$CPUS $n"
          n=$((n+1))
        done
        ;;
      *)
        case "$part" in ''|*[!0-9]*) echo "Bad CPU id: $part" >&2; exit 1;; esac
        CPUS="$CPUS $part"
        ;;
    esac
  done
  CPUS=${CPUS# }
  set -- $CPUS
  CPU_COUNT=$#
  if [ "$CPU_COUNT" -eq 0 ]; then
    echo "Empty CPU set" >&2; exit 1
  fi
}

cpu_at_index() {
  idx="$1"
  set -- $CPUS
  n=0
  for c in "$@"; do
    if [ "$n" -eq "$idx" ]; then
      echo "$c"; return 0
    fi
    n=$((n+1))
  done
  echo "$1"
}

parse_cpuset "$CPUSET"
echo "Using CPU set: $CPUS"
echo "Using rings: $RING_BASE .. $last_ring (unique per instance)"

PIDS=""

cleanup() {
  printf "\nStopping...\n"
  for pid in $PIDS; do
    kill "$pid" 2>/dev/null || :
  done
}
trap 'cleanup' INT TERM

i=0
while [ "$i" -lt "$N" ]; do
  port=$((BASE + i))
  ring=$((RING_BASE + i))   # unique ring per instance, no wrap
  idx=$((i % CPU_COUNT))
  CPU_CHOSEN=$(cpu_at_index "$idx")
  printf "Launching instance %d: port %d, ring %d, cpu %s\n" "$i" "$port" "$ring" "$CPU_CHOSEN"
  chrt -f "$PRIO" "$BIN" \
    --src-mac "$SRC_MAC" --src-ip "$SRC_IP" --src-port "$port" \
    --dst-mac "$DST_MAC" --dst-ip "$DST_IP" --dst-port "$port" \
    --mode server --cpu "$CPU_CHOSEN" --ring "$ring" &
  PIDS="$PIDS $!"
  i=$((i + 1))
done

printf "Started %s instances. PIDs:%s\n" "$N" "$PIDS"

for pid in $PIDS; do
  wait "$pid" || :
done
echo "All instances exited."
