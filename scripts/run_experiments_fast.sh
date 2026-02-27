SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

PORT=5555
NUM_TRIALS=3
HEARTBEAT=2
CHUNK_SIZE=100000
RESULTS_DIR="$ROOT_DIR/results"
RESULTS_FILE="$RESULTS_DIR/experiment_results.csv"

# Create results directory
mkdir -p "$RESULTS_DIR"

# Initialize CSV file with headers
echo "Algorithm,Threads,Trial,Parse_ms,Dispatch_ms,Worker_ms,Return_ms,Total_ms,Heartbeats,Found" > "$RESULTS_FILE"

# Fast algorithms only (MD5, SHA-256, SHA-512)
declare -a ALGORITHMS=("MD5" "SHA256" "SHA512")
declare -a SHADOW_FILES=("$ROOT_DIR/shadow/shadow_test_md5.txt" "$ROOT_DIR/shadow/shadow_test_sha256.txt" "$ROOT_DIR/shadow/shadow_test_sha512.txt")
declare -a PASSWORDS=("ABC" "GHI" "JKL")
declare -a THREAD_COUNTS=(1 2 4)

echo "=========================================="
echo "  FAST PASSWORD CRACKING PERFORMANCE TEST"
echo "=========================================="
echo ""
echo "Configuration:"
echo "  Algorithms: MD5, SHA-256, SHA-512 (skipping slow bcrypt/yescrypt)"
echo "  Thread counts: ${THREAD_COUNTS[*]}"
echo "  Heartbeat interval: ${HEARTBEAT}s"
echo "  Number of trials per config: $NUM_TRIALS"
echo "  Port: $PORT"
echo ""


extract_timings() {
    local output="$1"
    local parse=$(echo "$output" | grep "Parsing shadow file:" | awk '{print $4}')
    local dispatch="0"
    local worker=$(echo "$output" | grep "Worker cracking time:" | awk '{print $4}')
    local return_time=$(echo "$output" | grep "Result return latency:" | awk '{print $4}')
    local total=$(echo "$output" | grep "Total elapsed time:" | awk '{print $4}')
    local heartbeats=$(echo "$output" | grep "Heartbeats sent:" | awk '{print $3}')
    local found=$(echo "$output" | grep -q "Password FOUND" && echo "1" || echo "0")

    echo "$parse,$dispatch,$worker,$return_time,$total,${heartbeats:-0},$found"
}

for idx in "${!ALGORITHMS[@]}"; do
    algo="${ALGORITHMS[$idx]}"
    shadow="${SHADOW_FILES[$idx]}"
    password="${PASSWORDS[$idx]}"

    echo "=========================================="
    echo "Testing: $algo"
    echo "Shadow file: $shadow"
    echo "Expected password: $password"
    echo "=========================================="

    if [ ! -f "$shadow" ]; then
        echo "ERROR: Shadow file $shadow not found!"
        continue
    fi

    for threads in "${THREAD_COUNTS[@]}"; do
        echo ""
        echo "--- $algo with $threads thread(s) ---"

        for trial in $(seq 1 $NUM_TRIALS); do
            echo -n "  Trial $trial/$NUM_TRIALS... "

            "$ROOT_DIR/controller" -f "$shadow" -u testuser -p $PORT -b $HEARTBEAT -c $CHUNK_SIZE > "$RESULTS_DIR/controller_${algo}_t${threads}_trial${trial}.log" 2>&1 &
            CONTROLLER_PID=$!

            sleep 1

            "$ROOT_DIR/worker" -c localhost -p $PORT -t $threads > "$RESULTS_DIR/worker_${algo}_t${threads}_trial${trial}.log" 2>&1
            worker_exit=$?

            wait $CONTROLLER_PID 2>/dev/null
            controller_exit=$?

            controller_output=$(cat "$RESULTS_DIR/controller_${algo}_t${threads}_trial${trial}.log")

            timings=$(extract_timings "$controller_output")

            echo "$algo,$threads,$trial,$timings" >> "$RESULTS_FILE"

            IFS=',' read -r parse dispatch worker return_time total heartbeats found <<< "$timings"
            if [ $controller_exit -eq 0 ]; then
                echo "FOUND  Worker: ${worker}ms  Total: ${total}ms  HBs: $heartbeats"
            else
                echo "FAIL   Worker: ${worker}ms  Total: ${total}ms  HBs: $heartbeats"
            fi

            sleep 1
        done
    done

    echo ""
    echo "Completed all trials for $algo"
    echo ""
done

echo "=========================================="
echo "  FAST EXPERIMENTS COMPLETE"
echo "=========================================="
echo ""
echo "Results saved to: $RESULTS_FILE"
echo ""
echo "To analyze results, run:"
echo "  python3 $ROOT_DIR/analysis/analyze_results.py"
echo ""
echo "Summary:"
tail -n +2 "$RESULTS_FILE" | column -t -s','
