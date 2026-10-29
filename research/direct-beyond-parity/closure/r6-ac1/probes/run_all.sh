#!/bin/zsh
# R6/AC-1 probe runner — reproduces every archived raw file in ../raw/.
# Usage: zsh run_all.sh [binary] [btrfs_scratch_dir]
set -euo pipefail

BIN=${1:-./r6ac1_probe}
BTRFS_SCRATCH=${2:-/var/tmp/r6-ac1-scratch-btrfs}
RAW="$(dirname "$0")/../raw"
mkdir -p "$RAW"

# tmpfs scratch (default second argument omitted -> /tmp/r6-ac1-scratch)
for c in env array-order short-read iov-fault-read iov-fault-write iovmax \
         sum-overflow null-empty fsize-partial-write eintr cursor atomicity \
         nonseekable; do
    "$BIN" "$c" > "$RAW/r6ac1-$c.txt"
    echo "tmpfs $c exit=$?"
done

# btrfs scratch spot-checks (error/boundary rows must be shown on both fs)
mkdir -p "$BTRFS_SCRATCH"
for c in iov-fault-read iov-fault-write short-read iovmax sum-overflow \
         null-empty fsize-partial-write; do
    "$BIN" "$c" "$BTRFS_SCRATCH" > "$RAW/r6ac1-$c-btrfs.txt"
    echo "btrfs $c exit=$?"
done

# atomicity repetitions (descriptive stability check, archived separately)
for i in 2 3; do
    "$BIN" atomicity > "$RAW/r6ac1-atomicity-rep$i.txt"
    echo "atomicity rep$i exit=$?"
done
for i in 2 3; do
    "$BIN" iov-fault-write "$BTRFS_SCRATCH" > "$RAW/r6ac1-iov-fault-write-btrfs-rep$i.txt"
    echo "iov-fault-write btrfs rep$i exit=$?"
done
echo DONE
