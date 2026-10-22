#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
make
mkdir -p results/demo
./exam --delay-ms 100 --max-time 0 --log results/demo/normal.log
./exam --teachers 1 --students 8 --tickets 8 --queue 1 --prep-min 1 --prep-max 1 --check-time 5 --max-time 0 --delay-ms 100 --log results/demo/queue.log
./exam --students 5 --tickets 2 --max-time 0 --log results/demo/shortage.log
./exam --prep-min 100 --prep-max 100 --max-time 1 --log results/demo/timeout.log
