#!/bin/bash

# Experiment runner for password cracking performance analysis
# Runs multiple trials for each hashing algorithm and thread count

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

PORT=5555
NUM_TRIALS=5
HEARTBEAT=2
RESULTS_DIR="$ROOT_DIR/results"
RESULTS_FILE="$RESULTS_DIR/experiment_results.csv"

# Create results directory
mkdir -p "$RESULTS_DIR"

# Initialize CSV file with headers
echo "Algorithm,Threads,Trial,Parse_ms,Dispatch_ms,Worker_ms,Return_ms,Total_ms,Heartbeats,Found" > "$RESULTS_FILE"

# Array of algorithms and their test files
declare -a ALGORITHMS=("MD5" "bcrypt" "SHA256" "SHA512" "yescrypt")
declare -a SHADOW_FILES=("$ROOT_DIR/shadow/shadow_test_md5.txt" "$ROOT_DIR/shadow/shadow_test_bcrypt.txt" "$ROOT_DIR/shadow/shadow_test_sha256.txt" "$ROOT_DIR/shadow/shadow_test_sha512.txt" "$ROOT_DIR/shadow/shadow_test_yescrypt.txt")
declare -a PASSWORDS=("ABC" "DEF" "GHI" "JKL" "MNO")
declare -a THREAD_COUNTS=(1 2 3 4 10)

echo "=========================================="
echo "  PASSWORD CRACKING PERFORMANCE STUDY"
echo "=========================================="
echo ""
echo "Configuration:"
echo "  Number of trials per config: $NUM_TRIALS"
echo "  Thread counts: ${THREAD_COUNTS[*]}"
echo "  Heartbeat interval: ${HEARTBEAT}s"
echo "  Port: $PORT"
echo "  Character set: 79 characters (ASCII 33-111)"
echo "  Password length: 3 characters"
echo "  Search space: 79^3 = 493,039 candidates"
echo ""

# Function to extract timing from controller output
extract_timings() {
    local output="$1"
    local parse=$(echo "$output" | grep "Parsing shadow file:" | awk '{print $4}')
    local dispatch=$(echo "$output" | grep "Job dispatch latency:" | awk '{print $4}')
    local worker=$(echo "$output" | grep "Worker cracking time:" | awk '{print $4}')
    local return_time=$(echo "$output" | grep "Result return latency:" | awk '{print $4}')
    local total=$(echo "$output" | grep "Total elapsed time:" | awk '{print $4}')
    local heartbeats=$(echo "$output" | grep "Heartbeats exchanged:" | awk '{print $3}')
    local found=$(echo "$output" | grep -q "Password FOUND" && echo "1" || echo "0")

    echo "$parse,$dispatch,$worker,$return_time,$total,${heartbeats:-0},$found"
}

# Run experiments
for idx in "${!ALGORITHMS[@]}"; do
    algo="${ALGORITHMS[$idx]}"
    shadow="${SHADOW_FILES[$idx]}"
    password="${PASSWORDS[$idx]}"

    echo "=========================================="
    echo "Testing: $algo"
    echo "Shadow file: $shadow"
    echo "Expected password: $password"
    echo "=========================================="

    # Check if shadow file exists
    if [ ! -f "$shadow" ]; then
        echo "ERROR: Shadow file $shadow not found!"
        continue
    fi

    for threads in "${THREAD_COUNTS[@]}"; do
        echo ""
        echo "--- $algo with $threads thread(s) ---"

        for trial in $(seq 1 $NUM_TRIALS); do
            echo -n "  Trial $trial/$NUM_TRIALS... "

            # Start CONTROLLER in background first (it needs to listen)
            "$ROOT_DIR/controller" -f "$shadow" -u testuser -p $PORT -b $HEARTBEAT > "$RESULTS_DIR/controller_${algo}_t${threads}_trial${trial}.log" 2>&1 &
            CONTROLLER_PID=$!

            # Give controller time to start listening
            sleep 1

            # Run worker (it will connect to controller and both will terminate when done)
            "$ROOT_DIR/worker" -c localhost -p $PORT -t $threads > "$RESULTS_DIR/worker_${algo}_t${threads}_trial${trial}.log" 2>&1
            worker_exit=$?

            # Wait for controller to finish
            wait $CONTROLLER_PID 2>/dev/null
            controller_exit=$?

            # Read controller output for timing data
            controller_output=$(cat "$RESULTS_DIR/controller_${algo}_t${threads}_trial${trial}.log")

            # Extract timings
            timings=$(extract_timings "$controller_output")

            # Save to CSV
            echo "$algo,$threads,$trial,$timings" >> "$RESULTS_FILE"

            # Display summary
            IFS=',' read -r parse dispatch worker return_time total heartbeats found <<< "$timings"
            if [ $controller_exit -eq 0 ]; then
                echo "FOUND  Worker: ${worker}ms  Total: ${total}ms  HBs: $heartbeats"
            else
                echo "FAIL   Worker: ${worker}ms  Total: ${total}ms  HBs: $heartbeats"
            fi

            # Small delay between trials
            sleep 1
        done
    done

    echo ""
    echo "Completed all trials for $algo"
    echo ""
done

echo "=========================================="
echo "  EXPERIMENTS COMPLETE"
echo "=========================================="
echo ""
echo "Results saved to: $RESULTS_FILE"
echo ""
echo "Summary:"
cat "$RESULTS_FILE"
