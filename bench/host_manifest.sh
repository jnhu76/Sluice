#!/usr/bin/env bash
# Captures the G0 (#403) measurement-host manifest. Every field is read from the
# live system at capture time; nothing is inferred from the issue text.
set -uo pipefail

section() { printf '\n[%s]\n' "$1"; }

section "capture"
date -Is
echo "hostname=$(hostname)"
echo "capture_user=$(id -un) uid=$(id -u)"

section "baseline"
cd "$(dirname "$0")/.."
echo "base_sha=$(git rev-parse HEAD)"
echo "base_sha_tree=$(git rev-parse HEAD^{tree})"
echo "base_sha_subject=$(git log -1 --format=%s)"

section "windows_wsl"
echo "wsl_version_lines:"
(wsl.exe --version 2>/dev/null | tr -d '\0' || echo "wsl.exe unavailable from inside the distribution") | sed 's/^/  /'
if [ -r /mnt/c/Users/fred1/.wslconfig ]; then
    echo "wslconfig:"
    sed 's/^/  /' /mnt/c/Users/fred1/.wslconfig
else
    echo "wslconfig=NOT_READABLE"
fi

section "kernel"
uname -a
cat /proc/version
echo "cmdline=$(cat /proc/cmdline)"

section "cpu"
lscpu | grep -Ei "^(Architecture|CPU\(s\)|Thread|Core|Socket|Model name|Vendor|Hypervisor|BogoMIPS|CPU max MHz|CPU min MHz|L1d|L2|L3)" | sed 's/^/  /'
echo "nproc=$(nproc)"
echo "affinity=$(taskset -pc $$ 2>/dev/null)"
echo "smt_active=$(cat /sys/devices/system/cpu/smt/active 2>/dev/null || echo NOT_AVAILABLE)"
echo "cpufreq_present=$(ls /sys/devices/system/cpu/cpu0/cpufreq 2>/dev/null | head -1 || echo NO)"
for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -r "$f" ] && cat "$f"
done | sort | uniq -c | sed 's/^/  governor: /' || true
grep -m1 "model name" /proc/cpuinfo

section "memory"
free -b | sed 's/^/  /'
echo "swapon:"
swapon --show 2>/dev/null | sed 's/^/  /' || echo "  none"
echo "cgroup_v2_memory_max=$(cat /sys/fs/cgroup/memory.max 2>/dev/null || echo NOT_PRESENT)"
echo "cgroup_v2_cpu_max=$(cat /sys/fs/cgroup/cpu.max 2>/dev/null || echo NOT_PRESENT)"
echo "overcommit=$(cat /proc/sys/vm/overcommit_memory 2>/dev/null)"
echo "swappiness=$(cat /proc/sys/vm/swappiness 2>/dev/null)"
echo "dirty_ratio=$(cat /proc/sys/vm/dirty_ratio 2>/dev/null)"
echo "dirty_background_ratio=$(cat /proc/sys/vm/dirty_background_ratio 2>/dev/null)"

section "cpu_counters"
echo "perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null)"
echo "perf_version=$(perf --version 2>/dev/null || echo NOT_AVAILABLE)"
echo "pidstat=$(command -v pidstat || echo NOT_INSTALLED)"
echo "strace=$(command -v strace || echo NOT_INSTALLED)"

section "storage"
df -hT /tmp /home / 2>/dev/null | sed 's/^/  /'
echo "filesystem_types:"
printf '  /tmp -> %s\n' "$(stat -f -c %T /tmp)"
printf '  %s -> %s\n' "$HOME" "$(stat -f -c %T "$HOME")"
echo "lsblk:"
lsblk -d -o NAME,SIZE,ROTA,MODEL 2>/dev/null | sed 's/^/  /'
echo "mount_line_root=$(grep -m1 ' / ' /proc/self/mountinfo)"
echo "mount_line_tmp=$(grep -m1 ' /tmp ' /proc/self/mountinfo)"
echo "fadvise_dontneed_supported=assumed_linux_generic"
echo "drop_caches_writable=$([ -w /proc/sys/vm/drop_caches ] && echo yes || echo no)"
echo "sudo_nopasswd=$(sudo -n true 2>/dev/null && echo yes || echo no)"

section "toolchain"
echo "gcc=$(gcc --version | head -1)"
echo "g++=$(g++ --version | head -1)"
echo "clang=$(clang --version 2>/dev/null | head -1 || echo NOT_AVAILABLE)"
echo "xmake=$(xmake --version 2>/dev/null | head -1)"
echo "cmake=$(cmake --version 2>/dev/null | head -1 || echo NOT_AVAILABLE)"
if [ -r /usr/include/liburing/io_uring_version.h ]; then
    grep -E "IO_URING_VERSION_(MAJOR|MINOR)" /usr/include/liburing/io_uring_version.h | sed 's/^/  /'
fi
echo "liburing_pkg=$(dpkg -l liburing-dev 2>/dev/null | awk '/liburing-dev/{print $2, $3}')"
echo "liburing_so=$(ldconfig -p 2>/dev/null | grep -m2 liburing.so)"

section "io_uring_runtime"
if [ -x /tmp/uring_probe ]; then
    /tmp/uring_probe | sed 's/^/  /'
else
    echo "  probe binary absent"
fi

section "background_load_at_capture"
uptime
echo "top_cpu_processes:"
ps -eo pcpu,pmem,rss,comm --sort=-pcpu --no-headers | head -8 | sed 's/^/  /'
echo "vmstat_first_line: $(vmstat 1 2 2>/dev/null | tail -1)"
