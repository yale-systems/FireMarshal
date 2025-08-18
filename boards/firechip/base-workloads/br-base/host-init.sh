#!/bin/sh

exec make -C trigger
exec make -C /home/an683/chipyard-io/software/firemarshal/boards/default/lib
