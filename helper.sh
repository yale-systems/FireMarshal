#!/bin/bash
set -x

# Building drivers
cd boards/default/drivers/accnet-driver/ && make; cd ../../../..
cd boards/default/drivers/iocache-driver/ && make; cd ../../../..

# Building userspace codes and placing them into the filesystem
cd boards/default/lib/ && make static; cd ../../..
cp boards/default/lib/file_receiver boards/prototype/base-workloads/br-new/overlay/root/
# cp boards/default/lib/udp_client_kernel boards/prototype/base-workloads/br-new/overlay/bin/
# cp boards/default/lib/interrupt_exp  boards/prototype/base-workloads/br-new/overlay/root/

# Build image
./marshal -v -d clean br-new.json
./marshal -v -d build br-new.json
./marshal -v -d install -t prototype br-new.json

set +x
