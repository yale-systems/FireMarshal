#!/bin/sh

# === Configuration ===
BASE_PORT=1234      # starting src-port number
CPU=3               # CPU binding
DST_PORT=1111
SRC_MAC="00:0a:35:06:4d:e2"
SRC_IP="10.0.0.1"
DST_MAC="0c:42:a1:a8:2d:e6"
DST_IP="10.0.0.2"
NTEST=1024
PAYLOAD=64
MODE="block"

# === Parse arguments ===
if [ $# -lt 1 ]; then
    echo "Usage: $0 <nports>"
    exit 1
fi

NPORTS=$1

# Launch all processes
i=0
pids=""
outfiles=""
while [ $i -lt $NPORTS ]; do
    port=$((BASE_PORT + i))
    outfile="udp_exp_${port}.txt"
    outfiles="$outfiles $outfile"

    echo "Launching for src-port $port -> $outfile"
    ./udp_exp \
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
        >"$outfile" 2>&1 &

    pids="$pids $!"
    i=$((i + 1))
done

# Wait for all PIDs to finish
for pid in $pids; do
    wait "$pid"
done

echo "All udp_exp instances finished. Printing results one by one..."

# Print each result
for outfile in $outfiles; do
    echo "===== $outfile ====="
    cat "$outfile"
    echo
done
