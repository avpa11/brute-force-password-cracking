#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

PORT=5558
THREADS=${1:-2}
HEARTBEAT=${2:-2}
CHUNK=${3:-100000}

echo "Starting quick test of password cracking system..."
echo "  Threads: $THREADS, Heartbeat: ${HEARTBEAT}s, Chunk size: $CHUNK"
echo ""

# Start controller in background
"$ROOT_DIR/controller" -f "$ROOT_DIR/shadow/shadow_test_md5.txt" -u testuser -p $PORT -b $HEARTBEAT -c $CHUNK > /tmp/ctrl_out.txt 2>&1 &
CTRL_PID=$!

# Wait for controller to start
sleep 1

# Run worker
"$ROOT_DIR/worker" -c localhost -p $PORT -t $THREADS > /tmp/worker_out.txt 2>&1

# Wait for controller to finish
wait $CTRL_PID 2>/dev/null

echo ""
echo "=== CONTROLLER OUTPUT ==="
cat /tmp/ctrl_out.txt

echo ""
echo "=== WORKER OUTPUT ==="
cat /tmp/worker_out.txt
