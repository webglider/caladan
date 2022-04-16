#!/bin/bash

config=$1
for tqd in  10 20 30 40 50 60 70 80 90 100 110 120 130 140 150 160 170 180 190 200; do

	paste <(echo $tqd) <(cat ~/kairos-eval/$config-tqd$tqd.lat.txt | grep 'iops' | awk '{print $8, $12, $14, $16}') <(cat ~/kairos-eval/$config-tqd$tqd.util | grep 'Average' | awk '{print 100-$8}')

done
