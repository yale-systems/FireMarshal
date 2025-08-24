#!/bin/bash
set -x
cd boards/default/lib/ && make static; cd ../../..
cd boards/default/drivers/accnet-driver/ && make; cd ../../../..
cd boards/default/drivers/iocache-driver/ && make; cd ../../../..
./marshal -v clean br-base.json
./marshal -v build br-base.json
cp images/firechip/br-base/br-base.img /usr/local/cad/an683/sim-dir/sim_slot_0/br-base0-br-base.img
cp images/firechip/br-base/br-base-bin /usr/local/cad/an683/sim-dir/sim_slot_0/br-base0-br-base-bin
set +x
