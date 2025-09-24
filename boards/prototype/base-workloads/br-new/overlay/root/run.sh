#!/bin/sh
# run_udp_exp.sh — launch many udp_exp instances with varying src ports & rings

# Defaults (edit here if you like)
CMD="chrt -f 10 ./udp_exp"
RESET_ALL="./udp_exp --reset"
SRC_MAC="00:0a:35:06:4d:e2"
SRC_IP="10.0.0.1"
DST_MAC="0c:42:a1:a8:2d:e6"
DST_IP="10.0.0.2"
DST_PORT=1111
NTEST=1024
PAYLOAD=64
CPU="3"   # can now be a single CPU, range (e.g., 0-3), or list (e.g., 0,2,4-6)

# Tunables via flags (see usage)
N=1                  # -n : number of instances
MODE="poll"          # -m : block|poll
START_PORT=1200      # -p : starting src port
RING_START=0         # -R : first ring id
RING_COUNT=""        # -K : wrap rings modulo this count (optional)
OUTDIR=""            # -o : output directory (default = auto timestamp)

usage() {
  cat <<EOF
Usage: $0 [-n INSTANCES] [-m MODE] [-p START_PORT] [-R RING_START] [-K RING_COUNT] [-o OUTDIR] [-c CPU_SPEC] [-- EXTRA_ARGS]

Options:
  -n INSTANCES   Number of parallel runs (default: $N)
  -m MODE        'block' or 'poll' (default: $MODE)
  -p START_PORT  First source port to use (default: $START_PORT)
  -R RING_START  First ring id (default: $RING_START)
  -K RING_COUNT  If set, ring = (RING_START + i) % RING_COUNT (wrap)
  -o OUTDIR      Output directory (default: ./udp_runs_YYYYmmdd_HHMMSS)
  -c CPU_SPEC    Either a single CPU (e.g., 3),
                 a range (e.g., 0-3),
                 or a comma list with ranges (e.g., 0,2,4-6).
                 Instances round-robin across this set.

Any arguments after '--' are appended to each udp_exp invocation.

Examples:
  $0 -n 32 -m block -p 1220 -R 0 -c 0-3
  $0 -n 64 -m poll -p 1500 -R 0 -K 8 -o ./runs64 -c 1,3,5,7 -- --payload-size 128
EOF
  exit 1
}

# --- helpers to expand CPU specs and pick nth word (POSIX /bin/sh) ---

expand_cpu_spec() {
  # Expands "0-3,6,8-10" -> "0 1 2 3 6 8 9 10"
  # Returns empty on invalid input.
  spec=$1
  [ -n "$spec" ] || { echo ""; return; }

  # If purely digits, just return it
  case "$spec" in
    *[!0-9,-]*)
      echo ""; return ;;  # invalid character
  esac

  oldIFS=$IFS
  IFS=','; set -f
  list=""
  for tok in $spec; do
    case "$tok" in
      *-*)
        a=${tok%-*}
        b=${tok#*-}
        # validate numbers
        case "$a$b" in
          *[!0-9]*|'') echo ""; IFS=$oldIFS; set +f; return ;;
        esac
        if [ "$a" -gt "$b" ]; then echo ""; IFS=$oldIFS; set +f; return; fi
        i=$a
        while [ "$i" -le "$b" ]; do
          list="$list $i"
          i=$((i+1))
        done
        ;;
      '')
        echo ""; IFS=$oldIFS; set +f; return ;;
      *)
        # single number
        case "$tok" in *[!0-9]*)
          echo ""; IFS=$oldIFS; set +f; return ;;
        esac
        list="$list $tok"
        ;;
    esac
  done
  IFS=$oldIFS; set +f
  echo "${list# }"
}

nth_word() {
  # nth_word 3 "a b c d" -> c
  n=$1; shift
  set -- $*
  idx=1
  for w in "$@"; do
    if [ "$idx" -eq "$n" ]; then
      echo "$w"; return
    fi
    idx=$((idx+1))
  done
  echo ""
}

# Parse flags
while getopts ":n:m:p:R:K:o:c:h" opt; do
  case "$opt" in
    n) N="$OPTARG" ;;
    m) MODE="$OPTARG" ;;
    p) START_PORT="$OPTARG" ;;
    R) RING_START="$OPTARG" ;;
    K) RING_COUNT="$OPTARG" ;;
    o) OUTDIR="$OPTARG" ;;
    c) CPU="$OPTARG" ;;
    h) usage ;;
    :) echo "Error: -$OPTARG requires an argument"; usage ;;
    \?) echo "Error: invalid option -$OPTARG"; usage ;;
  esac
done
shift $((OPTIND - 1))

# Validate & set derived defaults
case "$MODE" in
  block|poll) ;;
  *) echo "Error: MODE must be 'block' or 'poll'"; exit 1 ;;
esac

case "$N" in
  ''|*[!0-9]*)
    echo "Error: INSTANCES (-n) must be a positive integer"; exit 1 ;;
  *) [ "$N" -le 0 ] && { echo "Error: INSTANCES must be > 0"; exit 1; } ;;
esac

if [ -n "$RING_COUNT" ]; then
  case "$RING_COUNT" in
    ''|*[!0-9]*|0) echo "Error: RING_COUNT (-K) must be a positive integer"; exit 1 ;;
  esac
fi

# Expand CPU spec (single, range, or list)
CPU_LIST="$(expand_cpu_spec "$CPU")"
if [ -z "$CPU_LIST" ]; then
  # If expand failed but CPU is a single integer, accept it
  case "$CPU" in
    ''|*[!0-9]*)
      echo "Error: invalid CPU spec '$CPU'"; exit 1 ;;
    *) CPU_LIST="$CPU" ;;
  esac
fi
CPU_COUNT=$(set -- $CPU_LIST; echo $#)

[ -n "$OUTDIR" ] || OUTDIR="./udp_runs_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTDIR" || { echo "Error: cannot create OUTDIR '$OUTDIR'"; exit 1; }

# Trap to clean up children on Ctrl-C/TERM
pids=""
cleanup() { [ -n "$pids" ] && kill $pids 2>/dev/null; }
trap cleanup INT TERM

echo "Launching $N instances (CPUs: $CPU_LIST):"
echo
sleep 1

# Collector in memory
outputs="$(
  pids=""
  cleanup_in_subshell() { [ -n "$pids" ] && kill $pids 2>/dev/null; }
  trap cleanup_in_subshell INT TERM

  i=0
  while [ "$i" -lt "$N" ]; do
    port=$((START_PORT + i))
    if [ -n "$RING_COUNT" ]; then
      ring=$(( (RING_START + i) % RING_COUNT ))
    else
      ring=$(( RING_START + i ))
    fi

    # Round-robin pick CPU i -> nth in CPU_LIST
    idx=$(( (i % CPU_COUNT) + 1 ))
    cpu="$(nth_word "$idx" "$CPU_LIST")"

    (
      out="$($CMD \
        --src-mac "$SRC_MAC" \
        --src-ip "$SRC_IP" \
        --src-port "$port" \
        --dst-mac "$DST_MAC" \
        --dst-ip "$DST_IP" \
        --dst-port "$DST_PORT" \
        --ntest "$NTEST" \
        --mode "$MODE" \
        --payload-size "$PAYLOAD" \
        --cpu "$cpu" \
        --ring "$ring" \
        "$@" 2>&1)"
      printf "===== INSTANCE %02d (port=%u ring=%u cpu=%u) =====\n%s\n\n" \
        "$i" "$port" "$ring" "$cpu" "$out"
    ) &
    pids="$pids $!"
    i=$((i + 1))
  done

  rc_total=0
  for pid in $pids; do
    if ! wait "$pid"; then rc_total=$((rc_total | 1)); fi
  done
  exit $rc_total
)"

$RESET_ALL

echo
echo "==== All runs completed. Printing outputs ===="
printf "%s\n" "$outputs"

exit $?
