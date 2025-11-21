#!/bin/bash
#sudo cgdelete -g cpuset:/cpulimit 2>/dev/null || true
#sudo cgcreate -t $USER:$USER -a $USER:$USER -g cpuset:/cpulimit
nshard=$1
shard=$2
trd=$3
cluster=$4
is_micro=$5
is_replicated=$6
bench=${7:-tpcc}
shift 7
EXTRA_ARGS="$@"
let up=trd+3
#sudo cgset -r cpuset.mems=0 cpulimit
#sudo cgset -r cpuset.cpus=0-$up cpulimit
mkdir -p results
path=$(pwd)/src/mako

# Build the base command. We invoke the top-level ./dbtest binary,
# which is what the CMake/Makefile build produces and which we keep
# up-to-date when rebuilding the project.
CMD="./dbtest --bench $bench --num-threads $trd --shard-index $shard --shard-config $path/config/local-shards$nshard-warehouses$trd.yml -F config/1leader_2followers/paxos$trd\_shardidx$shard.yml -F config/occ_paxos.yml -P $cluster"

# Add --is-micro flag if enabled (value is 1)
if [ "$is_micro" == "1" ]; then
    CMD="$CMD --is-micro"
fi

# Add --is-replicated flag if enabled (value is 1)
if [ "$is_replicated" == "1" ]; then
    CMD="$CMD --is-replicated"
fi

# Append any benchmark-specific extra args (e.g., YCSB -w/-r/-u/-m)
# after all core dbtest flags so that dbtest can parse its own options
# before handing remaining args to the benchmark.
if [ -n "$EXTRA_ARGS" ]; then
    CMD="$CMD $EXTRA_ARGS"
fi

# Print configuration
echo "========================================="
echo "Configuration:"
echo "========================================="
echo "  Number of shards:  $nshard"
echo "  Shard index:       $shard"
echo "  Number of threads: $trd"
echo "  Cluster:           $cluster"
echo "  Micro benchmark:   $([ "$is_micro" == "1" ] && echo "enabled" || echo "disabled")"
echo "  Replicated mode:   $([ "$is_replicated" == "1" ] && echo "enabled" || echo "disabled")"
echo "========================================="

eval $CMD    
