#!/usr/bin/env bash
# Run one LeanStore scheduler experiment under a watchdog.
#
#     scripts/lsched-run.sh <log> <workers> "<boot args>" [run.py args...]
#
# Boots build/release.x64/loader.img with workers+1 vCPUs (vCPU 0 = load
# generator) pinned to $LSCHED_CPUS (default: host CPUs 28.. = gpu socket 1),
# guest RAM from $LSCHED_MEM_PATH (default /dev/dax0.0 if writable; none =
# anonymous host RAM),
# $LSCHED_MEM (default 8G). Always through scripts/run.py, i.e. with its
# seccomp-BPF io_uring workaround (#70).
#
# A devdax region is zeroed first (scripts/dax-zero.py): it keeps the last
# guest's RAM, and OVMF hangs on that.
# Every phase has a deadline; on a miss, an abort or an error the VM is killed:
#   boot   "Booted up"          within LSCHED_T_BOOT (15 s)
#   init   "lsched: loading"    within LSCHED_T_INIT (10 s) after boot
#   load   "lsched: loaded"     within 10 s + 15 s per GiB of data_gib
#   run    "lsched: done"       within warmup + run + LSCHED_T_SLACK (15 s)
# Prints one line per event, for a monitor to follow:
#   PHASE <name> <seconds since start>
#   WATCHDOG <phase> <reason>    (then the last log lines; exit 2)
#   DONE <seconds>               (exit 0)
set -uo pipefail
here=$(dirname "$(readlink -f "$0")")
log=$1 workers=$2 args=$3
shift 3

cpus_first=${LSCHED_CPUS_FIRST:-28}
cpus=${LSCHED_CPUS:-$cpus_first-$((cpus_first + workers))}
mem=${LSCHED_MEM:-8G}
mem_path=${LSCHED_MEM_PATH:-}
if [ -z "$mem_path" ] && [ -w /dev/dax0.0 ]; then
    mem_path=/dev/dax0.0
fi
t_boot=${LSCHED_T_BOOT:-15}
t_init=${LSCHED_T_INIT:-10}
t_slack=${LSCHED_T_SLACK:-15}

argval() { # key default
    local v
    v=$(grep -oE "(^| )$1=[^ ]+" <<<"$args" | tail -1 | cut -d= -f2)
    echo "${v:-$2}"
}
data_gib=$(argval data_gib 1)
run_s=$(argval run 10)
warmup_s=$(argval warmup 1)
t_load=$(awk -v g="$data_gib" 'BEGIN { printf "%d", 10 + 15 * g + 0.999 }')
t_run=$(awk -v r="$run_s" -v w="$warmup_s" -v s="$t_slack" 'BEGIN { printf "%d", r + w + s + 0.999 }')

mkdir -p "$(dirname "$log")"
: > "$log"

# The host clock of the cores we use. On isolated nohz_full cores schedutil
# hardly ever runs, so each core keeps whatever clock it last had: up to 2x
# run-to-run difference. Record it; warn unless the governor is performance.
expand_cpus() { # 28-32,40 -> 28 29 30 31 32 40
    local part out=()
    for part in ${1//,/ }; do
        if [[ $part == *-* ]]; then out+=($(seq "${part%-*}" "${part#*-}")); else out+=("$part"); fi
    done
    echo "${out[@]}"
}
envline="ENV"
govs=""
for c in $(expand_cpus "$cpus"); do
    f=/sys/devices/system/cpu/cpu$c/cpufreq
    g=$(cat $f/scaling_governor 2>/dev/null || echo ?)
    envline+=" cpu$c=$g@$(( $(cat $f/scaling_cur_freq 2>/dev/null || echo 0) / 1000 ))MHz"
    govs+=" $g"
done
echo "$envline" >> "$log"
if grep -qv performance <<<"$(tr ' ' '\n' <<<"${govs# }")"; then
    echo "WARN governor not performance on all cores:$(tr ' ' '\n' <<<"${govs# }" | sort | uniq -c | tr '\n' ' ')"
fi
memargs=()
[ "$mem_path" = none ] && mem_path=   # LSCHED_MEM_PATH=none: anonymous RAM
[ -n "$mem_path" ] && memargs=(--mem-path "$mem_path")
# devdax keeps the previous guest's RAM, and OVMF hangs on such a dirty boot
# (spins on vCPU 0 before printing anything): clear the region first.
if [[ "$mem_path" == /dev/dax* ]]; then
    gib=${mem%G}
    echo "PHASE zero 0.0"
    if ! timeout 10 python3 "$here/dax-zero.py" "$mem_path" "$gib" >> "$log" 2>&1; then
        echo "WATCHDOG zero clearing $mem_path failed"
        exit 2
    fi
fi
setsid python3 "$here/run.py" -m "$mem" -c $((workers + 1)) --pin "$cpus" "${memargs[@]}" \
    --nogdb --args "workers=$workers $args" "$@" < /dev/null >> "$log" 2>&1 &
pid=$!

start=$(date +%s.%N)
elapsed() { awk -v s="$start" -v n="$(date +%s.%N)" 'BEGIN { printf "%.1f", n - s }'; }
fail() {
    echo "WATCHDOG $phase $1 after $(elapsed) s"
    kill -TERM -- -"$pid" 2>/dev/null
    sleep 1
    kill -KILL -- -"$pid" 2>/dev/null
    grep -v '^\s*$' "$log" | tail -8 | sed 's/^/  | /'
    exit 2
}

errors='Aborted|cfa=0x|\[backtrace\]|page fault|lsched: (unknown|need|gen_cpu|workers and|boot argument)|mem engine: .*I/O requested|terminate called|heap: '
phase=boot deadline=$t_boot phase_start=$start
next_phase() { # name deadline-seconds
    phase=$1 deadline=$2 phase_start=$(date +%s.%N)
    echo "PHASE $phase $(elapsed)"
}
echo "PHASE boot 0.0"
while :; do
    if ! kill -0 "$pid" 2>/dev/null; then
        grep -q 'lsched: done' "$log" && break
        fail "VM exited early"
    fi
    if grep -qE "$errors" "$log"; then
        fail "error in log"
    fi
    case $phase in
    boot) grep -q 'Booted up' "$log" && next_phase init "$t_init" ;;
    init) grep -q 'lsched: loading' "$log" && next_phase load "$t_load" ;;
    load) grep -q 'lsched: loaded' "$log" && next_phase run "$t_run" ;;
    run)  grep -q 'lsched: done' "$log" && break ;;
    esac
    over=$(awk -v s="$phase_start" -v n="$(date +%s.%N)" -v d="$deadline" 'BEGIN { print (n - s > d) }')
    [ "$over" = 1 ] && fail "deadline ${deadline}s missed"
    sleep 0.2
done
# teardown: errors after "lsched: done" count too; give the guest 3 s
next_phase teardown 3
for _ in $(seq 15); do
    grep -qE "$errors" "$log" && fail "error in log"
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.2
done
grep -qE "$errors" "$log" && fail "error in log"
kill -TERM -- -"$pid" 2>/dev/null
echo "DONE $(elapsed)"
