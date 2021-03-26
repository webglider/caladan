#!/bin/bash

# Example usage: ./run_apps.sh multicore-remoteram 192.168.10.115:5000 192.168.10.116:5000 60 $((32*1024)) 2 6 6 "0,4,8,12,16,20" 1

config=$1
thru_target=$2
lat_target=$3
duration=$4
thru_sz=$5
thru_qd=$6
lat_sz=$((4*1024))
lat_qd="${10}"
num_thru=$7
num_lat=$8
use_cores=$9

buffer_duration=20
record_sleep=25
record_duration=5
record_cores="0,4,8,12,16,20"

function cleanup() {
    sudo killall storage_client;
    sudo killall iokerneld;
    echo "Cleaned up";
}

trap cleanup EXIT

function arr_to_str() {
    data=$1
    printf -v joined '%s,' "${data[@]}";
    echo "${joined%,}";
}

cleanup;


pids=()

outlabel="$config"
echo "Starting $outlabel";

# Spawn IOkernel
sudo NUMA_COUNT=1 ./iokerneld simple $use_cores 2>&1 | tee ~/caladan-eval/$outlabel.iokernel &
pids+=($!);
echo "Launched IOKernel";

# Give IOKernel time to start
sleep 10;

# Start apps
# Thru-apps
for ((i = 1 ; i <= $num_thru ; i++)); do
    sudo NUMA_COUNT=1 apps/bench/storage_client "thru$i.config" 192.168.10.$((150+$i)):5000 $thru_sz $thru_qd $duration 1 100 > ~/caladan-eval/$outlabel.thru$i.txt 2>&1 &
    pids+=($!);
    echo "Launched thru$i";
done

# Lat-apps
for ((i = 1 ; i <= $num_lat ; i++)); do
    sudo NUMA_COUNT=1 apps/bench/storage_client "lat$i.config" $lat_target $lat_sz $lat_qd $duration $lat_qd 100 > ~/caladan-eval/$outlabel.lat$i.txt 2>&1 &
    pids+=($!);
    echo "Launched lat$i";
done
(sleep $record_sleep; sar -u 1 $record_duration -P $record_cores > ~/caladan-eval/$outlabel.util) &
(sleep $record_sleep; pidstat -u -hl -p $(pgrep -d, storage_client) 1 $record_duration > ~/caladan-eval/$outlabel.apputil) &


# Wait for run to complete
sleep $(($duration+$buffer_duration));
cleanup;
sleep 2;






