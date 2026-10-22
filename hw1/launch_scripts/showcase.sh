#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
make
mkdir -p results/demo
printf '\n=== Обычный экзамен ===\n'
./exam --log results/demo/normal.log
printf '\n=== Один преподаватель и полная очередь ===\n'
./exam --teachers 1 --students 3 --tickets 3 --ticket-mode sequential \
  --grade-mode ticket --prep-min 1 --prep-max 1 --check-time 2 \
  --queue 1 --log results/demo/queue.log
printf '\n=== Билетов меньше, чем студентов ===\n'
./exam --students 5 --tickets 2 --log results/demo/shortage.log
printf '\n=== Ограничение времени ===\n'
./exam --max-time 1 --log results/demo/timeout.log
printf '\nЖурналы созданы в results/demo/\n'
