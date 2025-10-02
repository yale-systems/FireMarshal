#!/bin/sh
set -eu

BIN=./udp_mt_client
SERVER=192.168.0.1
COUNT=10240
BASE_PORT=1200

NTHREAD=$1
SIZES="1024"

# sanity
[ -x "$BIN" ] || { echo "Error: $BIN not found or not executable"; exit 1; }

for th in $NTHREAD; do
  for sz in $SIZES; do
    echo "=== Running: threads=$th size=$sz ==="
    "$BIN" --server $SERVER --threads $th --count $COUNT --size $sz --base-port $BASE_PORT
    # The program will write: udp_result_${th}_${sz}.csv
  done
done

echo "All runs complete."

