#!/bin/bash
# $1 = tree dir, $2 = label
tree="$1"; label="$2"
cd "$tree" || exit 1
mkdir -p "/tmp/mut_$label"
: > "/tmp/mut_$label/results.tsv"
while read -r t; do
  bin="./build/linux/x86_64/debug/$t"
  if [ ! -x "$bin" ]; then echo -e "MISSING\t$t\t-\t-" >> "/tmp/mut_$label/results.tsv"; continue; fi
  rc1=0; rc2=0
  timeout 300 "$bin" > "/tmp/mut_$label/$t.1.log" 2>&1; rc1=$?
  timeout 300 "$bin" > "/tmp/mut_$label/$t.2.log" 2>&1; rc2=$?
  echo -e "$t\t$rc1\t$rc2" >> "/tmp/mut_$label/results.tsv"
done < /tmp/f2_mutants.txt
echo "DONE $label"
