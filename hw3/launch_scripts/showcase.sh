#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
make
mkdir -p results/demo
./exam-unnamed --delay-ms 100 --log results/demo/unnamed.log
./exam-named --delay-ms 100 --log results/demo/named.log
bash launch_scripts/independent.sh shm 6 2
bash launch_scripts/independent.sh mq 6 2
