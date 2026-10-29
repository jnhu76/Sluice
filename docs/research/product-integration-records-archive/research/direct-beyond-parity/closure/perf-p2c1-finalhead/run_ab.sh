#!/usr/bin/env bash
# A/B wrapper: writes R4-format receipt with header + cmd line.
set -euo pipefail
cd "$(dirname "$0")"
name=$1; shift
{
  echo "# ab-$name — paired2 $* (ABBA, ladderA=cand 64508282, ladderB=base 43cb8029)"
  echo "# cmd: ./paired2.sh $*"
  ./paired2.sh "$@"
} > "ab-$name.txt"
echo "done ab-$name"
