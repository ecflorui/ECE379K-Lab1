#!/usr/bin/env bash
# part6.sh -- every Part 6 measurement in one go.  Run on a Frontera
# COMPUTE node (idev), from starter_files/:
#
#     bash part6.sh                 # defaults below
#     N=4096 WT=16 bash part6.sh    # override shard count / writer-test threads
#
# Output: results/part6/
#   sharded_<lock>_N<shards>_mix<F-I-E>.csv/.log   thread sweeps (sweep.sh: pinned, median of 3)
#   writers_N<shards>.txt                          WRITERS=n runs, full bench lines (rd= / wr=)
#
# 4 locks x 3 mixes x 2 shard counts = 24 sweeps, plus the writer runs.
# Roughly 25 minutes.  Each sweep and each writer run has a timeout:
# a reader-preferring lock at 1 shard can starve its writers, and a
# point that hangs is itself a result -- it is recorded, not hidden.

set -u
cd "$(dirname "$0")"

N=${N:-4096}                                # your chosen shard count (Part 4/5)
THREADS=${THREADS:-"1 2 4 8 16 32 56 112"}  # thread sweep
WT=${WT:-16}                                # threads for the WRITERS comparison
LOCKS=${LOCKS:-"ttas rw rwp shared_mutex"}
MIXES=${MIXES:-"80/10/10 50/25/25 100/0/0"}
OUT=results/part6
mkdir -p "$OUT"

make bench >/dev/null || { echo "make bench failed" >&2; exit 1; }

hostname          > "$OUT/hostname.txt"
lscpu             > "$OUT/lscpu.txt"
g++ --version | head -1 > "$OUT/compiler.txt"

# ---- 1. thread sweeps: lock x mix x shard count -----------------------
for mix in $MIXES; do
    mtag=${mix//\//-}
    for shards in "$N" 1; do
        for lock in $LOCKS; do
            name="$OUT/sharded_${lock}_N${shards}_mix${mtag}"
            echo "== sharded:$lock  shards=$shards  MIX=$mix" >&2
            MIX=$mix timeout 900 bash sweep.sh ./bench "sharded:$lock" "$shards" $THREADS \
                > "$name.csv" 2> >(tee "$name.log" >&2)
            [ $? -eq 124 ] && echo "# TIMEOUT: sweep did not finish" | tee -a "$name.log" >&2
        done
    done
done

# ---- 2. WRITERS=n: pure writers vs pure readers, per-class throughput ---
# Pinned the same way sweep.sh pins WT threads (CPUs 0..WT-1), 3 runs each.
cpus=$(seq -s, 0 $((WT - 1)))
for shards in "$N" 1; do
    f="$OUT/writers_N${shards}.txt"
    : > "$f"
    echo "# T=$WT shards=$shards cpus=$cpus  (3 runs per line group)" | tee -a "$f" >&2
    for lock in $LOCKS; do
        for w in 1 2 4 8; do
            for r in 1 2 3; do
                line=$(WRITERS=$w timeout 30 taskset -c "$cpus" ./bench "sharded:$lock" "$WT" "$shards" 2>/dev/null)
                [ -z "$line" ] && line="sharded:$lock T=$WT shards=$shards WRITERS=$w  TIMEOUT (no result in 30 s)"
                echo "W=$w run=$r  $line" | tee -a "$f" >&2
            done
        done
    done
done

echo "done -- results in $OUT" >&2