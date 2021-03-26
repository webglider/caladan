#!/bin/bash

# Example usage: ./run_servers.sh

numa_cnt=$1
num_servers=$2

function cleanup() {
    sudo killall storage_server;
    echo "Cleaned up";
}

trap cleanup EXIT

function arr_to_str() {
    data=$1
    printf -v joined '%s,' "${data[@]}";
    echo "${joined%,}";
}

#cleanup;


pids=()


# Start servers
for ((i = 1 ; i <= $num_servers ; i++)); do
    sudo NUMA_COUNT=$numa_cnt apps/storage_service/storage_server thru_server$i.config 5000 &
    pids+=($!);
    echo "Launched thru_server$i";
done

sleep infinity;







