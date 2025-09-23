#!/bin/sh
# run_udp_exp.sh — launch many udp_exp instances with varying src ports & rings

# Defaults (edit here if you like)
CMD="./udp_exp"
RESET_ALL="./udp_exp --reset"
SRC_MAC="00:0a:35:06:4d:e2"
SRC_IP="10.0.0.1"
DST_MAC="0c:42:a1:a8:2d:e6"
DST_IP="10.0.0.2"
DST_PORT=1111
NTEST=1024
PAYLOAD=64
CPU=3

# Tunables via flags (see usage)
N=1                  # -n : number of instances
MODE="poll"          # -m : block|poll
START_PORT=1200      # -p : starting src port
RING_START=0         # -R : first ring id
RING_COUNT=""        # -K : wrap rings modulo this count (optional)
OUTDIR=""            # -o : output directory (default = auto timestamp)

usage() {
  cat <<EOF
Usage: $0 [-n INSTANCES] [-m MODE] [-p START_PORT] [-R RING_START] [-K RING_COUNT] [-o OUTDIR] [-c CPU] [-- EXTRA_ARGS]

Options:
  -n INSTANCES   Number of parallel runs (default: $N)
  -m MODE        'block' or 'poll' (default: $MODE)
  -p START_PORT  First source port to use (default: $START_PORT)
  -R RING_START  First ring id (default: $RING_START)
  -K RING_COUNT  If set, ring = (RING_START + i) % RING_COUNT (wrap)
  -o OUTDIR      Output directory (default: ./udp_runs_YYYYmmdd_HHMMSS)
  -c CPU         CPU index to pass to --cpu (default: $CPU)

Any arguments after '--' are appended to each udp_exp invocation.

Examples:
  $0 -n 32 -m block -p 1220 -R 0
  $0 -n 64 -m poll -p 1500 -R 0 -K 8 -o ./runs64 -- --payload-size 128
EOF
  exit 1
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
  *)
    if [ "$N" -le 0 ]; then echo "Error: INSTANCES must be > 0"; exit 1; fi ;;
esac

if [ -n "$RING_COUNT" ]; then
  case "$RING_COUNT" in
    ''|*[!0-9]*|0) echo "Error: RING_COUNT (-K) must be a positive integer"; exit 1 ;;
  esac
fi

[ -n "$OUTDIR" ] || OUTDIR="./udp_runs_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTDIR" || { echo "Error: cannot create OUTDIR '$OUTDIR'"; exit 1; }

# Trap to clean up children on Ctrl-C/TERM
pids=""
cleanup() {
  [ -n "$pids" ] && kill $pids 2>/dev/null
}
trap cleanup INT TERM

echo "Launching $N instances:"
# echo "  mode=$MODE start_port=$START_PORT ring_start=$RING_START ring_count=${RING_COUNT:-none} cpu=$CPU"
# echo "  outputs -> $OUTDIR"
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
        --cpu "$CPU" \
        --ring "$ring" \
        "$@" 2>&1)"
      printf "===== INSTANCE %02d (port=%u ring=%u) =====\n%s\n\n" \
        "$i" "$port" "$ring" "$out"
    ) &
    pids="$pids $!"
    i=$((i + 1))
  done

  # wait for all children before the subshell exits (so outputs are complete)
  rc_total=0
  for pid in $pids; do
    if ! wait "$pid"; then rc_total=$((rc_total | 1)); fi
  done
  exit $rc_total
)"

$RESET_ALL

# Now print everything at once
echo
echo "==== All runs completed. Printing outputs ===="
printf "%s\n" "$outputs"

# Exit code propagated from the subshell
exit $?
