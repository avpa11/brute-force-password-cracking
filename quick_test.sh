#!/bin/bash

PORT=5558

echo "Starting quick test of password cracking system..."

# Start controller in background
./controller -f shadow_test_md5.txt -u testuser -p $PORT > /tmp/ctrl_out.txt 2>&1 &
CTRL_PID=$!

# Wait for controller to start
sleep 1

# Run worker
./worker -c localhost -p $PORT > /tmp/worker_out.txt 2>&1

# Wait for controller to finish
sleep 1

echo ""
echo "=== CONTROLLER OUTPUT ==="
cat /tmp/ctrl_out.txt

echo ""
echo "=== WORKER OUTPUT ==="
cat /tmp/worker_out.txt
