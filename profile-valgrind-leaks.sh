#!/bin/bash

# Valgrind Memory Leak Profiling for libreactor-server
# Runs the server with Valgrind Memcheck to detect memory leaks

set -e

TARGET="./libreactor-server --disable-log"
OUTPUT_DIR="./leak-profiles/$(date +%Y%m%d_%H%M%S)"
LOG_FILE="$OUTPUT_DIR/valgrind-leak-report.log"

echo "=== Valgrind Memory Leak Profiling ==="
echo "Target: $TARGET"
echo "Output: $OUTPUT_DIR"
echo ""

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Clean start - kill any existing servers
echo "Cleaning up previous instances..."
killall -9 libreactor-server wrk 2>/dev/null || true
sleep 2

# Start single server process with Valgrind (it will spawn its own workers)
echo "Starting libreactor-server with Valgrind Memcheck..."
echo "(Server will automatically spawn 3 worker processes for 3 CPUs)"
echo ""

valgrind --tool=memcheck \
         --leak-check=full \
         --leak-resolution=high \
         --show-leak-kinds=all \
         --track-origins=yes \
         --verbose \
         --log-file="$OUTPUT_DIR/valgrind-leak-report.log" \
         --num-callers=50 \
         $TARGET &

VALGRIND_PID=$!
echo "Valgrind PID: $VALGRIND_PID"
echo ""

# Wait for server to start (it spawns workers internally)
echo "Waiting for server and workers to start..."
sleep 8

# Check if server processes are running (parent + workers)
RUNNING_PROCESSES=$(pgrep -f "libreactor-server" | wc -l)
if [ "$RUNNING_PROCESSES" -lt 1 ]; then
    echo "❌ Server failed to start! No processes running."
    kill -9 $VALGRIND_PID 2>/dev/null || true
    exit 1
fi

echo "✅ Server started successfully ($RUNNING_PROCESSES processes total: 1 parent + $((RUNNING_PROCESSES-1)) workers)"
echo ""

# Generate HTTP load to trigger memory allocations
echo "Generating HTTP load for 15 seconds..."
echo "Load test results:" > "$OUTPUT_DIR/load_results.txt"

timeout 15 wrk -t4 -c100 -d10s \
    --latency \
    http://localhost:3984/json 2>/dev/null | \
    tee -a "$OUTPUT_DIR/load_results.txt" || true

echo ""
echo "Stopping server gracefully..."

# Stop the Valgrind process with SIGTERM (allows clean shutdown and leak summary)
kill -TERM $VALGRIND_PID 2>/dev/null || true
echo "Waiting for Valgrind to write leak summary..."
sleep 10

# Check if process is still running
if pgrep -f "valgrind.*memcheck.*libreactor-server" > /dev/null; then
    echo "Valgrind still running, waiting longer..."
    sleep 5
fi

# If still running, use SIGKILL as last resort
if pgrep -f "valgrind.*memcheck.*libreactor-server" > /dev/null; then
    echo "Force stopping Valgrind process..."
    pkill -9 -f "valgrind.*memcheck.*libreactor-server" 2>/dev/null || true
    sleep 3
fi

# Also kill any remaining server processes
pkill -TERM -f "libreactor-server" 2>/dev/null || true
sleep 2
pkill -9 -f "libreactor-server" 2>/dev/null || true

# Give extra time for Valgrind to finish writing logs
sleep 2

# Wait for Valgrind to finish writing logs
sleep 3

echo ""
echo "=== Leak Analysis Complete ==="
echo "Results saved to: $OUTPUT_DIR"
echo ""

# Analyze Valgrind log file
LOG_FILE="$OUTPUT_DIR/valgrind-leak-report.log"

if [ -f "$LOG_FILE" ]; then
    echo "=== MEMORY LEAK SUMMARY ==="
    echo "Log file: $LOG_FILE"
    echo ""

    # Count different types of leaks
    if grep -q "definitely lost:" "$LOG_FILE"; then
        echo "Definitely lost memory:"
        grep "definitely lost:" "$LOG_FILE" | tail -1
    fi

    if grep -q "indirectly lost:" "$LOG_FILE"; then
        echo "Indirectly lost memory:"
        grep "indirectly lost:" "$LOG_FILE" | tail -1
    fi

    if grep -q "possibly lost:" "$LOG_FILE"; then
        echo "Possibly lost memory:"
        grep "possibly lost:" "$LOG_FILE" | tail -1
    fi

    if grep -q "still reachable:" "$LOG_FILE"; then
        echo "Still reachable memory:"
        grep "still reachable:" "$LOG_FILE" | tail -1
    fi

    echo ""
    echo "Total heap usage:"
    grep "total heap usage:" "$LOG_FILE" | tail -1

    echo ""
    echo "=== DETAILED LEAK REPORT ==="
    echo "(showing leak details)"
    echo ""

    # Show detailed leak information
    awk '/HEAP SUMMARY/{flag=1} flag{print} /ERROR SUMMARY/{flag=0}' "$LOG_FILE" | head -50

    echo ""
    echo "=== FULL REPORT LOCATION ==="
    echo "Complete Valgrind output: $LOG_FILE"
    echo "Load test results: $OUTPUT_DIR/load_results.txt"
    echo ""

    # Check if any significant leaks were found
    if grep -q "definitely lost: [1-9]" "$LOG_FILE"; then
        echo "⚠️  MEMORY LEAKS DETECTED!"
        echo "Check the full log file for details."
    elif grep -q "All heap blocks were freed" "$LOG_FILE"; then
        echo "✅ NO MEMORY LEAKS DETECTED!"
    else
        echo "ℹ️  Leak detection inconclusive - check full log."
    fi

else
    echo "❌ Valgrind log file not found!"
    echo "Check if Valgrind started properly."
fi

echo ""
echo "=== Profiling Complete ==="
