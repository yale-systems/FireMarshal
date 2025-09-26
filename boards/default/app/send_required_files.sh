#!/bin/bash
set -ex  # exit on error

SERVER_IP="10.0.0.1"
LIB_PATH="../lib/"

for val in udp_exp udp_client_kernel udp_server_kernel; do
    ./file_sender "$SERVER_IP" "$LIB_PATH$val"
done
set +x
