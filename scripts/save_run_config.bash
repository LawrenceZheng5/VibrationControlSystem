#!/usr/bin/env bash

# Save the runtime and system configuration associated with a paRead run.
#
# Usage:
#   ./scripts/save_run_config.bash <run_directory_relative_to_data_raw>
#
# Examples:
#   ./scripts/save_run_config.bash 20260715
#
# Saves to:
#   /home/scexao/VibrationControlSystem/data/raw/20260715/system_config.log
#
# Another example:
#   ./scripts/save_run_config.bash 20260715/RT_kernel/taskset14_overnight
#
# Saves to:
#   /home/scexao/VibrationControlSystem/data/raw/20260715/RT_kernel/taskset14_overnight/system_config.log

set -u

BASE_DATA_DIR="/home/scexao/VibrationControlSystem/data/raw"

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <run_directory_relative_to_data_raw>"
    echo
    echo "Example:"
    echo "  $0 20260715/RT_kernel/taskset14_overnight"
    exit 1
fi

RUN_SUBDIR="$1"
RUN_DIR="${BASE_DATA_DIR}/${RUN_SUBDIR}"
OUTPUT_FILE="${RUN_DIR}/run_config.log"

mkdir -p "$RUN_DIR"

PID="$(pgrep -n -x paRead || true)"

{
    echo "============================================================"
    echo "paRead Run Configuration"
    echo "============================================================"

    echo
    echo "Run directory:"
    realpath "$RUN_DIR"

    echo
    echo "Configuration file:"
    realpath -m "$OUTPUT_FILE"

    echo
    echo "Date:"
    date --iso-8601=seconds

    echo
    echo "Hostname:"
    hostname

    echo
    echo "User:"
    whoami

    echo
    echo "Working directory:"
    pwd

    echo
    echo "============================================================"
    echo "paRead Process"
    echo "============================================================"

    if [[ -n "$PID" && -d "/proc/$PID" ]]; then
        echo
        echo "paRead PID:"
        echo "$PID"

        echo
        echo "paRead executable:"
        if [[ -L "/proc/$PID/exe" ]]; then
            readlink -f "/proc/$PID/exe"
        else
            echo "Executable path unavailable"
        fi

        echo
        echo "paRead working directory:"
        if [[ -L "/proc/$PID/cwd" ]]; then
            readlink -f "/proc/$PID/cwd"
        else
            echo "Working directory unavailable"
        fi

        echo
        echo "paRead command line:"
        if [[ -r "/proc/$PID/cmdline" ]]; then
            tr '\0' ' ' < "/proc/$PID/cmdline"
            echo
        else
            echo "Command line unavailable"
        fi

        echo
        echo "Process start time:"
        ps -p "$PID" -o lstart=

        echo
        echo "Elapsed runtime:"
        ps -p "$PID" -o etime=

        echo
        echo "Process CPU affinity:"
        taskset -pc "$PID"

        echo
        echo "Process scheduling policy and priority:"
        chrt -p "$PID"

        echo
        echo "Process summary:"
        ps -o pid,ppid,psr,cls,rtprio,pri,ni,stat,%cpu,%mem,etime,comm,args \
            -p "$PID"

        echo
        echo "Thread scheduling and CPU placement:"
        ps -L -o pid,tid,psr,cls,rtprio,pri,ni,stat,%cpu,comm \
            -p "$PID"

        echo
        echo "Per-thread CPU affinity:"
        while read -r TID; do
            echo
            echo "TID $TID:"
            taskset -pc "$TID"
        done < <(ps -L -p "$PID" -o tid=)

        echo
        echo "Process limits:"
        if [[ -r "/proc/$PID/limits" ]]; then
            cat "/proc/$PID/limits"
        else
            echo "Process limits unavailable"
        fi

        echo
        echo "Memory-lock status:"
        if [[ -r "/proc/$PID/status" ]]; then
            grep -E '^(VmLck|VmRSS|VmSize|Threads|Cpus_allowed_list|Mems_allowed_list):' \
                "/proc/$PID/status"
        else
            echo "Process status unavailable"
        fi

        echo
        echo "Process cgroup:"
        if [[ -r "/proc/$PID/cgroup" ]]; then
            cat "/proc/$PID/cgroup"
        else
            echo "Cgroup information unavailable"
        fi
    else
        echo
        echo "WARNING: paRead is not currently running."
        echo "Process-specific information could not be recorded."
    fi

    echo
    echo "============================================================"
    echo "Kernel and Operating System"
    echo "============================================================"

    echo
    echo "Operating system:"
    if [[ -r /etc/os-release ]]; then
        cat /etc/os-release
    else
        echo "/etc/os-release unavailable"
    fi

    echo
    echo "Kernel:"
    uname -a

    echo
    echo "Kernel command line:"
    cat /proc/cmdline

    echo
    echo "Kernel preemption model:"
    if [[ -r /sys/kernel/realtime ]]; then
        echo -n "Realtime kernel flag: "
        cat /sys/kernel/realtime
    fi

    if [[ -r /sys/kernel/debug/sched/preempt ]]; then
        cat /sys/kernel/debug/sched/preempt
    else
        echo "Detailed preemption information unavailable"
    fi

    echo
    echo "============================================================"
    echo "CPU Configuration"
    echo "============================================================"

    echo
    echo "CPU information:"
    lscpu

    echo
    echo "Online CPUs:"
    cat /sys/devices/system/cpu/online

    echo
    echo "Offline CPUs:"
    if [[ -r /sys/devices/system/cpu/offline ]]; then
        cat /sys/devices/system/cpu/offline
    else
        echo "None reported"
    fi

    echo
    echo "Isolated CPUs:"
    if [[ -r /sys/devices/system/cpu/isolated ]]; then
        cat /sys/devices/system/cpu/isolated
    else
        echo "Isolation information unavailable"
    fi

    echo
    echo "SMT control:"
    if [[ -r /sys/devices/system/cpu/smt/control ]]; then
        cat /sys/devices/system/cpu/smt/control
    else
        echo "SMT control unavailable"
    fi

    echo
    echo "SMT active:"
    if [[ -r /sys/devices/system/cpu/smt/active ]]; then
        cat /sys/devices/system/cpu/smt/active
    else
        echo "SMT active state unavailable"
    fi

    echo
    echo "CPU frequency governors:"
    for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
        CPU_NAME="$(basename "$cpu")"
        GOVERNOR_FILE="$cpu/cpufreq/scaling_governor"

        if [[ -r "$GOVERNOR_FILE" ]]; then
            printf "%-8s " "$CPU_NAME"
            cat "$GOVERNOR_FILE"
        fi
    done

    echo
    echo "CPU frequency driver:"
    for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
        CPU_NAME="$(basename "$cpu")"
        DRIVER_FILE="$cpu/cpufreq/scaling_driver"

        if [[ -r "$DRIVER_FILE" ]]; then
            printf "%-8s " "$CPU_NAME"
            cat "$DRIVER_FILE"
        fi
    done

    echo
    echo "CPU energy-performance preferences:"
    for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
        CPU_NAME="$(basename "$cpu")"
        EPP_FILE="$cpu/cpufreq/energy_performance_preference"

        if [[ -r "$EPP_FILE" ]]; then
            printf "%-8s " "$CPU_NAME"
            cat "$EPP_FILE"
        fi
    done

    echo
    echo "CPU idle-state configuration:"
    for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
        CPU_NAME="$(basename "$cpu")"

        if compgen -G "$cpu/cpuidle/state*" > /dev/null; then
            echo
            echo "$CPU_NAME:"

            for state in "$cpu"/cpuidle/state*; do
                STATE_NAME="unknown"
                DISABLED="unknown"
                LATENCY="unknown"

                [[ -r "$state/name" ]] &&
                    STATE_NAME="$(cat "$state/name")"

                [[ -r "$state/disable" ]] &&
                    DISABLED="$(cat "$state/disable")"

                [[ -r "$state/latency" ]] &&
                    LATENCY="$(cat "$state/latency")"

                printf "  %-10s disabled=%-3s latency=%s us\n" \
                    "$STATE_NAME" "$DISABLED" "$LATENCY"
            done
        fi
    done

    echo
    echo "============================================================"
    echo "Memory and Real-Time Limits"
    echo "============================================================"

    echo
    echo "Swappiness:"
    cat /proc/sys/vm/swappiness

    echo
    echo "Current shell limits:"
    ulimit -a

    echo
    echo "Realtime group configuration:"
    if command -v getent > /dev/null; then
        getent group realtime || echo "realtime group not found"
    fi

    echo
    echo "Realtime limits configuration:"
    FOUND_RT_LIMITS=0

    for limits_file in \
        /etc/security/limits.conf \
        /etc/security/limits.d/*.conf; do

        if [[ -r "$limits_file" ]]; then
            MATCHES="$(grep -E '(@realtime|rtprio|memlock|nice)' "$limits_file" || true)"

            if [[ -n "$MATCHES" ]]; then
                echo
                echo "$limits_file:"
                echo "$MATCHES"
                FOUND_RT_LIMITS=1
            fi
        fi
    done

    if [[ "$FOUND_RT_LIMITS" -eq 0 ]]; then
        echo "No matching realtime limit entries found"
    fi

    echo
    echo "============================================================"
    echo "USB and IRQ Configuration"
    echo "============================================================"

    echo
    echo "Connected USB devices:"
    lsusb

    echo
    echo "USB audio devices:"
    if command -v arecord > /dev/null; then
        arecord -l
    else
        echo "arecord is not installed"
    fi

    echo
    echo "PortAudio-related processes:"
    pgrep -af 'paRead|jack|pipewire|pulseaudio' || \
        echo "No matching processes found"

    echo
    echo "xHCI IRQ entries:"
    XHCI_LINES="$(grep -i xhci /proc/interrupts || true)"

    if [[ -n "$XHCI_LINES" ]]; then
        echo "$XHCI_LINES"
    else
        echo "No xHCI IRQ entries found"
    fi

    echo
    echo "xHCI IRQ affinities:"
    XHCI_IRQS="$(
        grep -i xhci /proc/interrupts |
        awk -F: '{gsub(/^[[:space:]]+/, "", $1); print $1}'
    )"

    if [[ -n "$XHCI_IRQS" ]]; then
        while read -r IRQ; do
            [[ -z "$IRQ" ]] && continue

            echo
            echo "IRQ $IRQ:"

            if [[ -r "/proc/irq/$IRQ/smp_affinity_list" ]]; then
                echo -n "  smp_affinity_list: "
                cat "/proc/irq/$IRQ/smp_affinity_list"
            else
                echo "  smp_affinity_list unavailable"
            fi

            if [[ -r "/proc/irq/$IRQ/effective_affinity_list" ]]; then
                echo -n "  effective_affinity_list: "
                cat "/proc/irq/$IRQ/effective_affinity_list"
            else
                echo "  effective_affinity_list unavailable"
            fi
        done <<< "$XHCI_IRQS"
    else
        echo "No xHCI IRQ affinities found"
    fi

    echo
    echo "============================================================"
    echo "End of Configuration"
    echo "============================================================"

} | tee "$OUTPUT_FILE"

echo
echo "Configuration saved to:"
realpath "$OUTPUT_FILE"