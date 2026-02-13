PORT=5555
NUM_TRIALS=3
RESULTS_DIR="results"
RESULTS_FILE="$RESULTS_DIR/experiment_results.csv"

# Create results directory
mkdir -p "$RESULTS_DIR"

# Initialize CSV file with headers
echo "Algorithm,Trial,Parse_ms,Dispatch_ms,Worker_ms,Return_ms,Total_ms,Found" > "$RESULTS_FILE"

# Fast algorithms only (MD5, SHA-256, SHA-512)
declare -a ALGORITHMS=("MD5" "SHA256" "SHA512")
declare -a SHADOW_FILES=("shadow_test_md5.txt" "shadow_test_sha256.txt" "shadow_test_sha512.txt")
declare -a PASSWORDS=("ABC" "GHI" "JKL")

echo "=========================================="
echo "  FAST PASSWORD CRACKING PERFORMANCE TEST"
echo "=========================================="
echo ""
echo "Configuration:"
echo "  Algorithms: MD5, SHA-256, SHA-512 (skipping slow bcrypt/yescrypt)"
echo "  Number of trials per algorithm: $NUM_TRIALS"
echo "  Port: $PORT"
echo "  Estimated time: ~5-10 minutes"
echo ""


extract_timings() {
    local output="$1"
    local parse=$(echo "$output" | grep "Parsing shadow file:" | awk '{print $4}')
    local dispatch=$(echo "$output" | grep "Job dispatch latency:" | awk '{print $4}')
    local worker=$(echo "$output" | grep "Worker cracking time:" | awk '{print $4}')
    local return=$(echo "$output" | grep "Result return latency:" | awk '{print $4}')
    local total=$(echo "$output" | grep "Total elapsed time:" | awk '{print $4}')
    local found=$(echo "$output" | grep -q "Password FOUND" && echo "1" || echo "0")

    echo "$parse,$dispatch,$worker,$return,$total,$found"
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

    for trial in $(seq 1 $NUM_TRIALS); do
        echo ""
        echo "--- Trial $trial/$NUM_TRIALS ---"

        ./controller -f "$shadow" -u testuser -p $PORT > "$RESULTS_DIR/controller_${algo}_trial${trial}.log" 2>&1 &
        CONTROLLER_PID=$!

        sleep 1

        ./worker -c localhost -p $PORT > "$RESULTS_DIR/worker_${algo}_trial${trial}.log" 2>&1
        worker_exit=$?

        wait $CONTROLLER_PID 2>/dev/null
        controller_exit=$?

        controller_output=$(cat "$RESULTS_DIR/controller_${algo}_trial${trial}.log")

        timings=$(extract_timings "$controller_output")

        echo "$algo,$trial,$timings" >> "$RESULTS_FILE"

        if [ $controller_exit -eq 0 ]; then
            echo "✓ Trial $trial complete - Password found"
        else
            echo "✗ Trial $trial complete - Password not found"
        fi

        IFS=',' read -r parse dispatch worker return total found <<< "$timings"
        echo "  Parse: ${parse}ms, Dispatch: ${dispatch}ms, Worker: ${worker}ms, Return: ${return}ms, Total: ${total}ms"

        sleep 1
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
echo "  python3 analyze_results.py"
echo ""
echo "Summary:"
tail -n +2 "$RESULTS_FILE" | column -t -s','
