#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-shm}
students=${2:-6}
teachers=${3:-2}
if [[ "$mode" != shm && "$mode" != mq ]] ||
   [[ ! "$students" =~ ^([0-9]|[12][0-9]|3[0-2])$ ]] ||
   [[ ! "$teachers" =~ ^([1-9]|[12][0-9]|3[0-2])$ ]]; then
    echo "Usage: $0 shm|mq STUDENTS(0..32) TEACHERS(1..32) [options]" >&2
    exit 2
fi
shift "$(( $# < 3 ? $# : 3 ))"
make
mkdir -p results/demo
session="demo_$$_$RANDOM"
log="results/demo/$mode-$session.log"
pids=()
owner=
cleanup() {
    if [[ -n "$owner" ]]; then kill -TERM "$owner" 2>/dev/null || :; wait "$owner" || :; fi
    for pid in "${pids[@]}"; do wait "$pid" || :; done
}
trap cleanup EXIT
trap 'if [[ -n "$owner" ]]; then kill -INT "$owner" 2>/dev/null || :; wait "$owner" || :; owner=; fi; exit 130' INT
trap 'exit 143' TERM
"./exam-$mode" --delay-ms 100 "$@" --session "$session" \
    --students "$students" --teachers "$teachers" --log "$log" &
owner=$!
for (( attempt=0; attempt<200; attempt++ )); do
    if [[ -f "$log" ]] && grep -q 'ГОТОВО' "$log"; then break; fi
    if ! kill -0 "$owner" 2>/dev/null; then wait "$owner"; exit 2; fi
    sleep .01
done
if [[ ! -f "$log" ]] || ! grep -q 'ГОТОВО' "$log"; then exit 2; fi
for ((i=1; i<=teachers; i++)); do
    ./exam-teacher --mode "$mode" --session "$session" --id "$i" &
    pids+=("$!")
done
for ((i=1; i<=students; i++)); do
    ./exam-student --mode "$mode" --session "$session" --id "$i" &
    pids+=("$!")
done
rc=0
wait "$owner" || rc=$?
owner=
for pid in "${pids[@]}"; do wait "$pid" || rc=2; done
pids=()
exit "$rc"
