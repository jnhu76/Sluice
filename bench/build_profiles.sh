#!/usr/bin/env bash
# Builds the G0 (#403) benchmark binaries for both request-execution profiles.
#
# Each profile gets its own build directory: reusing one build directory across a
# --liburing switch can link no-ring stub objects into ring consumers (#520), and
# a benchmark must measure the profile it claims to measure.
#
# The profile of each produced binary is asserted from its dynamic dependencies, not from
# the build directory name:
#   * bench_w02_pipeline / bench_w03_external_loop link sluice_async, so the ring
#     profile must depend on liburing.so and the no-ring profile must not;
#   * bench_w01_direct links sluice_core only, so it must be byte-identical in
#     both profiles (W-01 has no request-execution dependency by contract).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

mode="${1:-release}"
targets=(bench_w01_direct bench_w02_pipeline bench_w03_external_loop)
async_targets=(bench_w02_pipeline bench_w03_external_loop)

declare -A hash

build_profile() {
    local profile="$1" buildir
    buildir="build-bench-$profile"
    if [ "$profile" = "ring" ]; then
        xmake f -m "$mode" -o "$buildir" --liburing=y -y >/dev/null
    else
        xmake f -m "$mode" -o "$buildir" --liburing=n -y >/dev/null
    fi
    for target in "${targets[@]}"; do
        xmake build -y "$target" >/dev/null
        local binary="$buildir/linux/x86_64/$mode/$target"
        if [ ! -x "$binary" ]; then
            echo "FAIL: $binary was not produced (build directory is not the configured one?)"
            exit 1
        fi
        hash["$profile:$target"]="$(sha256sum "$binary" | cut -d' ' -f1)"
        printf '%s %s %s\n' "$profile" "$binary" "${hash["$profile:$target"]}"
    done
}

build_profile noring
build_profile ring

echo "# profile assertions"

for target in "${async_targets[@]}"; do
    ring_binary="build-bench-ring/linux/x86_64/$mode/$target"
    noring_binary="build-bench-noring/linux/x86_64/$mode/$target"
    if ! readelf -d "$ring_binary" 2>/dev/null | grep -q "liburing.so"; then
        echo "FAIL: $ring_binary is ring-profile but does not link liburing"
        exit 1
    fi
    if readelf -d "$noring_binary" 2>/dev/null | grep -q "liburing.so"; then
        echo "FAIL: $noring_binary is no-ring-profile but links liburing"
        exit 1
    fi
    if [ "${hash["ring:$target"]}" = "${hash["noring:$target"]}" ]; then
        echo "FAIL: $target is byte-identical across profiles"
        exit 1
    fi
    echo "ok: $target differs across profiles and only the ring build depends on liburing"
done

if [ "${hash["ring:bench_w01_direct"]}" != "${hash["noring:bench_w01_direct"]}" ]; then
    echo "FAIL: bench_w01_direct differs across profiles but W-01 has no async dependency"
    exit 1
fi
echo "ok: bench_w01_direct is profile-independent (core-only), identical in both profiles"
