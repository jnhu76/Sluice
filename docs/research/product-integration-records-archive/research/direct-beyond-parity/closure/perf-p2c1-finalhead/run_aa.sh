#!/usr/bin/env bash
# A/A wrapper: writes R4-format receipt with header + cmd line.
set -euo pipefail
cd "$(dirname "$0")"
name=$1; shift
{ echo "# aa-$name — paired2 $* (process-level A/A, this session)"
  echo "# cmd: ./paired2.sh $*"
  ./paired2.sh "$@"
} > "aa-$name.txt"
echo "done aa-$name"
