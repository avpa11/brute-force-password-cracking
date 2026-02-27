#!/bin/bash

# MD5 Thread Scaling Experiment
# Runs MD5 password cracking at 1, 2, 3, 4, and 10 threads
# with multiple trials per thread count for statistical analysis

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

PORT=5555
NUM_TRIALS=5
HEARTBEAT=2
CHUNK_SIZE=100000
RESULTS_DIR="$ROOT_DIR/results"
RESULTS_FILE="$RESULTS_DIR/md5_thread_experiment.csv"
SHADOW_FILE="$ROOT_DIR/shadow/shadow_test_md5.txt"

mkdir -p "$RESULTS_DIR"

# Initialize CSV
echo "Threads,Trial,Worker_ms,Total_ms" > "$RESULTS_FILE"

# Ensure binaries are built
make -s -C "$ROOT_DIR" || { echo "ERROR: Build failed"; exit 1; }

echo "=========================================="
echo "  MD5 THREAD SCALING EXPERIMENT"
echo "=========================================="
echo ""
echo "Configuration:"
echo "  Trials per thread count: $NUM_TRIALS"
echo "  Thread counts: 1 2 3 4 10"
echo "  Shadow file: $SHADOW_FILE"
echo "  Heartbeat: ${HEARTBEAT}s"
echo ""

# Phase 1: Run 1-4 threads
for threads in 1 2 3 4; do
    echo "--- MD5 with $threads thread(s) ---"

    for trial in $(seq 1 $NUM_TRIALS); do
        echo -n "  Trial $trial/$NUM_TRIALS... "

        # Start controller in background
        "$ROOT_DIR/controller" -f "$SHADOW_FILE" -u testuser -p $PORT -b $HEARTBEAT -c $CHUNK_SIZE \
            > "$RESULTS_DIR/md5_ctrl_t${threads}_r${trial}.log" 2>&1 &
        CTRL_PID=$!
        sleep 1

        # Run worker
        "$ROOT_DIR/worker" -c localhost -p $PORT -t $threads \
            > "$RESULTS_DIR/md5_work_t${threads}_r${trial}.log" 2>&1
        wait $CTRL_PID 2>/dev/null

        # Extract timings from controller log
        log="$RESULTS_DIR/md5_ctrl_t${threads}_r${trial}.log"
        worker_ms=$(grep "Worker cracking time:" "$log" | awk '{print $4}')
        total_ms=$(grep "Total elapsed time:" "$log" | awk '{print $4}')

        echo "$threads,$trial,$worker_ms,$total_ms" >> "$RESULTS_FILE"
        echo "Worker: ${worker_ms}ms  Total: ${total_ms}ms"

        # Increment port to avoid bind conflicts
        PORT=$((PORT + 1))
        sleep 0.5
    done
    echo ""
done

echo "Phase 1 complete (1-4 threads)."
echo ""

# Phase 2: Run 10 threads (after analysis script makes prediction)
echo "--- MD5 with 10 thread(s) ---"
for trial in $(seq 1 $NUM_TRIALS); do
    echo -n "  Trial $trial/$NUM_TRIALS... "

    "$ROOT_DIR/controller" -f "$SHADOW_FILE" -u testuser -p $PORT -b $HEARTBEAT -c $CHUNK_SIZE \
        > "$RESULTS_DIR/md5_ctrl_t10_r${trial}.log" 2>&1 &
    CTRL_PID=$!
    sleep 1

    "$ROOT_DIR/worker" -c localhost -p $PORT -t 10 \
        > "$RESULTS_DIR/md5_work_t10_r${trial}.log" 2>&1
    wait $CTRL_PID 2>/dev/null

    log="$RESULTS_DIR/md5_ctrl_t10_r${trial}.log"
    worker_ms=$(grep "Worker cracking time:" "$log" | awk '{print $4}')
    total_ms=$(grep "Total elapsed time:" "$log" | awk '{print $4}')

    echo "10,$trial,$worker_ms,$total_ms" >> "$RESULTS_FILE"
    echo "Worker: ${worker_ms}ms  Total: ${total_ms}ms"

    PORT=$((PORT + 1))
    sleep 0.5
done

echo ""
echo "=========================================="
echo "  ALL EXPERIMENTS COMPLETE"
echo "=========================================="
echo "Results saved to: $RESULTS_FILE"
echo ""
echo "Run analysis: python3 $ROOT_DIR/analysis/analyze_md5_scaling.py"
