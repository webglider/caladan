#!/bin/bash

for tqd in  210 220 230 240 250 260 270 280 290 300 310 320 330 340 350 360 370 380 390 400; do

	paste <(echo $tqd) <(cat ~/kairos-eval/varytqd-tqd$tqd.lat.txt | grep 'iops' | awk '{print $8, $12, $14, $16}') <(cat ~/kairos-eval/varytqd-tqd$tqd.thru.txt | grep spins | awk '{print $2}')

done
