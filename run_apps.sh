#!/bin/bash

config=$1
target=$2
sz=$3
qd=$4
tqd=$5

record_sleep=10
record_duration=5
duration=30
buffer_duration=5

function cleanup() {
    killall storage_client;
    killall cpu_intensive;
    killall iokerneld;
    echo "Cleaned up";
}

trap cleanup EXIT


cleanup;


# Spawn IOkernel
./iokerneld ias noidlefastwake noht nicpci 0000:25:00.1 0,24,4,28 &
pids+=($!);
echo "Launched IOKernel";

# Give IOKernel time to start
sleep 10;

#L-app
./apps/bench/storage_client tqd$tqd.config $target 1 $duration $qd $sz 100 > ~/kairos-eval/$config.lat.txt 2>&1 &
pids+=($!);
echo "Launched L-app";

#T-app
#./apps/bench/cpu_intensive cpu_intensive.config $duration > ~/kairos-eval/$config.thru.txt 2>&1 &
#pids+=($!);
#echo "Launched T-app";

(sleep $record_sleep; sar -u 1 $record_duration -P 4 > ~/kairos-eval/$config.util) &

# Wait for run to complete
sleep $(($duration+$buffer_duration));
cleanup;
sleep 2;


