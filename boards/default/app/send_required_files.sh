#!/bin/bash
set -ex  # exit on error

SERVER_IP="10.0.0.1"
LIB_PATH="../lib/"

for val in udp_exp interrupt_exp udp_client_kernel file_receiver; do
    ./file_sender "$SERVER_IP" "$LIB_PATH$val"
done
set +x
