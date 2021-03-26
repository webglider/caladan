#!/bin/bash

config=$1
lapp_sz=$2
tapp_sz=$3

cat ~/caladan-eval/$config.lat*.txt | grep "iops"
cat ~/caladan-eval/$config.thru*.txt | grep "iops"

echo "L-apps"
cat ~/caladan-eval/$config.lat*.txt | grep "iops" | awk -v sz=$lapp_sz '{iops += $2, xput += $2*sz*8/1e9, avglat += $8, taillat += $12} END {print iops, xput, avglat/NR, taillat/NR}'

echo "T-apps"
cat ~/caladan-eval/$config.thru*.txt | grep "iops" | awk -v sz=$tapp_sz '{iops += $2, xput += $2*sz*8/1e9} END {print iops, xput}'