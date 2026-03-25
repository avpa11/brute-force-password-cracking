#!/bin/bash
# Test 1: Normal run with checkpointing visible in logs
# Test 2: Worker death + recovery via SIGSTOP

cd "$(dirname "$0")"
PORT=20020
SHADOW=shadow/shadow_demo_checkpoint.txt
USER=testuser

echo "=== TEST 1: Normal run with checkpointing ==="
echo "Password: !AB! (index ~701,678)  |  chunk=100000  checkpoint=25000  heartbeat=3s"
echo ""

./controller -f "$SHADOW" -u "$USER" -p $PORT -b 3 -c 100000 -k 25000 > /tmp/t1_ctrl.log 2>&1 &
CTRL=$!
sleep 0.3
./worker -c localhost -p $PORT -t 4 > /tmp/t1_wk1.log 2>&1
wait $CTRL 2>/dev/null

echo "--- Controller log ---"
cat /tmp/t1_ctrl.log
echo ""
echo "--- Worker log (tail) ---"
tail -8 /tmp/t1_wk1.log

echo ""
echo "=== TEST 2: Worker death + recovery ==="
echo "Steps: start 2 workers, SIGSTOP worker1 after 3s, it misses heartbeat,"
echo "       controller re-queues from last checkpoint, worker2 finishes."
echo ""

PORT2=20021
./controller -f "$SHADOW" -u "$USER" -p $PORT2 -b 3 -c 100000 -k 25000 > /tmp/t2_ctrl.log 2>&1 &
CTRL2=$!

sleep 0.3
./worker -c localhost -p $PORT2 -t 2 > /tmp/t2_wk1.log 2>&1 &
WK1=$!

sleep 0.3
./worker -c localhost -p $PORT2 -t 2 > /tmp/t2_wk2.log 2>&1 &
WK2=$!

# Let both workers process a few chunks and emit checkpoints, then stop worker1
sleep 3
echo "[$(date +%T)] Sending SIGSTOP to worker1 (PID $WK1) to simulate unresponsiveness..."
kill -SIGSTOP $WK1

# Controller should detect the missed heartbeat within one more heartbeat interval (3s)
# and re-queue worker1's in-progress chunk to worker2
wait $CTRL2 2>/dev/null
kill -SIGCONT $WK1 2>/dev/null  # resume so it can exit cleanly
kill $WK1 $WK2 2>/dev/null
wait 2>/dev/null

echo "--- Controller log ---"
cat /tmp/t2_ctrl.log
echo ""
echo "--- Worker1 log (tail) ---"
tail -5 /tmp/t2_wk1.log
echo ""
echo "--- Worker2 log (tail) ---"
tail -5 /tmp/t2_wk2.log
