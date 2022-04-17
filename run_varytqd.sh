#!/bin/bash

config="const50-varytqd"
target="192.168.10.115:8080"
sz=$((128*1024))
qd=8

for tqd in 10 20 30 40 50 60 70 80 90 100 110 120 130 140 150 160 170 180 190 200; do
	echo "Starting $config-tqd$tqd";
	./run_apps.sh $config-tqd$tqd $target $sz $qd $tqd;
	sleep 2;
done
