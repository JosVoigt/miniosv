#!/usr/bin/env bash
# Two workers on one chiplet (CCD) vs on two, TPC-C, power and temperature.
#
#     scripts/lsched-chiplet.sh [reps=3] [out-dir=results/chiplet]
#
# gpu socket 1: CCD 0 = cpus 28-34, CCD 1 = 35-41, CCD 2 = 42-48, CCD 3 =
# 49-55 (SMN slots 0, 2, 4, 6). The load generator sits on 28 in both
# setups; the workers on 35+36 (same CCD) or 35+42 (different CCDs). Runs
# alternate, with a cooldown before each. Needs dvfs_passthrough and
# smn_passthrough loaded and the performance governor on 28-55.
#
# One line per run on stdout (for a monitor); the full logs in out-dir.
set -uo pipefail
here=$(dirname "$(readlink -f "$0")")
reps=${1:-3}
out=${2:-results/chiplet}
cooldown=${CHIPLET_COOLDOWN:-45}
args="workload=tpcc warehouses=10 dram_gib=8 run=${CHIPLET_RUN:-60} warmup=5"
mkdir -p "$out"

host_tctl() { # socket 1's Tctl from the host's k10temp, in C
    for h in /sys/class/hwmon/hwmon*; do
        [ "$(cat "$h/name")" = k10temp ] || continue
        [ "$(basename "$(readlink -f "$h/device")")" = 0000:00:19.3 ] || continue
        awk '{ printf "%.1f", $1 / 1000 }' "$h/temp1_input"
    done
}

summary() { # log -> one line
    awk -F, '
        /^lsched,summary,[0-9]/ { tps = $8 }
        /^lsched,energy,package,/ { pkg = $7 }
        /^lsched,energy,core_generator,/ { gen = $7 }
        /^lsched,energy,core_worker0,/ { w0 = $7 }
        /^lsched,energy,core_worker1,/ { w1 = $7 }
        /^lsched,temp,[a-z]/ && $3 != "sensor" { t = t sprintf(" %s %.1f/%.1f", $3, $7, $8) }
        END { printf "tps %d | W pkg %.1f gen %.2f w0 %.2f w1 %.2f | C mean/max%s", tps, pkg, gen, w0, w1, t }
    ' "$1"
}

for rep in $(seq 1 "$reps"); do
    for setup in same diff; do
        case $setup in
        same) cpus=28,35,36 ;;
        diff) cpus=28,35,42 ;;
        esac
        sleep "$cooldown"
        before=$(host_tctl)
        log=$out/$setup-r$rep.log
        status=$(LSCHED_CPUS=$cpus LSCHED_MEM=12G timeout 300 "$here/lsched-run.sh" "$log" 2 "$args" 2>&1 |
                 grep -E '^(DONE|WATCHDOG|WARN)' | tr '\n' ' ')
        echo "$setup r$rep (cpus $cpus, host Tctl before $before C): $(summary "$log") | $status"
    done
done
echo "END"
