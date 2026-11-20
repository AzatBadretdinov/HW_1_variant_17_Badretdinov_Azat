#!/usr/bin/env bash
# Same four applications can be started manually in separate terminals/hosts.
set -euo pipefail
cd "$(dirname "$0")/.."
make
mkdir -p results/demo
log="results/demo/server-$$.log"
pids=()
owner=
cleanup() {
    if [[ -n "$owner" ]]; then kill -TERM "$owner" 2>/dev/null || :; wait "$owner" || :; fi
    for p in "${pids[@]}"; do wait "$p" || :; done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
./exam-server --host 127.0.0.1 --port 0 --delay-ms 100 --max-time 300 --log "$log" &
owner=$!
for ((n=0;n<300;n++)); do
    if [[ -f "$log" ]] && grep -q 'LISTEN' "$log"; then break; fi
    if ! kill -0 "$owner" 2>/dev/null; then wait "$owner"; exit 2; fi
    sleep .01
done
port=$(sed -n 's/.*LISTEN .*port=\([0-9]*\).*/\1/p' "$log" | head -1)
if [[ -z "$port" ]]; then echo "Server did not start" >&2; exit 2; fi
./exam-monitor --host 127.0.0.1 --port "$port" > "results/demo/monitor-$$.log" &
pids+=("$!")
for ((i=1;i<=2;i++)); do
    ./exam-teacher --host 127.0.0.1 --port "$port" --id "$i" > "results/demo/teacher-$i-$$.log" &
    pids+=("$!")
done
for ((i=1;i<=6;i++)); do
    ./exam-student --host 127.0.0.1 --port "$port" --id "$i" > "results/demo/student-$i-$$.log" &
    pids+=("$!")
done
rc=0
wait "$owner" || rc=$?
owner=
for p in "${pids[@]}"; do wait "$p" || rc=2; done
pids=()
exit "$rc"
