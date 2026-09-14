#!/bin/bash
# Benchmark measurement campaign on the remote machine, following the
# measurement protocol: seven runs per configuration, the fastest
# and the slowest discarded and the rest averaged (done inside the
# program) and the machine state recorded around the run with uptime/who
# so the conditions of the measurement are documented. Output is
# written under results/, so the run is reproducible in place.
cd "$HOME/TremorFSR" || exit 1
mkdir -p results

{
  echo "=== run_campaign $(date -u) ==="
  echo "--- BEFORE: uptime ---"; uptime
  echo "--- BEFORE: who ---";    who
  echo "--- nproc ---";          nproc
} > results/run_meta.txt

./benchmark data/synthetic 256 1,2,4,8,16,32,64,128,256 7 > results/benchmark.log 2>&1

{
  echo "--- AFTER: uptime ---"; uptime
  echo "--- AFTER: who ---";    who
  echo "DONE $(date -u)"
} >> results/run_meta.txt
