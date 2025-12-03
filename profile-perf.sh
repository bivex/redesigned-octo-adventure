#!/bin/bash
# Linux perf profiling script for libreactor optimizations
# Lighter weight alternative to AMD uProf, shows function-level hotspots

set -e

# ============================================================================
# CONFIGURATION - Adjust these settings as needed
# ============================================================================

# Profiling settings
PERF_FREQUENCY=999              # Sampling frequency (Hz)
PERF_DURATION=10                # Profiling duration (seconds)
PERF_CALL_GRAPH="fp"            # Call graph method: fp (fast), dwarf (detailed but slow), or lbr

# Load test settings
WRK_THREADS=4                  # Number of wrk threads
WRK_CONNECTIONS=400             # Number of concurrent connections
WRK_DURATION=5                 # Load test duration (seconds)
WRK_URL="http://localhost:3984/json"  # Target URL

# Server settings
SERVER_PORT=3984                # Server port
SERVER_CPUS="0 1 2"             # CPU cores for taskset (space-separated)

# Output settings
OUTPUT_BASE="./perf-profiles"   # Base directory for results

# ============================================================================

echo "=== Linux perf Profiling for Libreactor ==="
echo ""
echo "Configuration:"
echo "  Perf frequency: ${PERF_FREQUENCY} Hz"
echo "  Perf duration: ${PERF_DURATION}s"
echo "  Load test: ${WRK_THREADS} threads, ${WRK_CONNECTIONS} connections, ${WRK_DURATION}s"
echo ""

# Check if perf is installed
if ! command -v perf > /dev/null 2>&1; then
    echo "❌ perf not found!"
    echo "Install with: apt-get install linux-perf"
    exit 1
fi

# Check perf permissions
echo "[1/6] Checking perf permissions..."
if [ "$(cat /proc/sys/kernel/perf_event_paranoid)" != "-1" ]; then
    echo "  Setting perf_event_paranoid = -1..."
    echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid > /dev/null
fi
echo "  Perf permissions: $(cat /proc/sys/kernel/perf_event_paranoid)"
echo ""

OUTPUT_DIR="${OUTPUT_BASE}/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTPUT_DIR"

echo "  Output: $OUTPUT_DIR"
echo ""

# Clean previous sessions
echo "[2/6] Cleaning previous sessions..."
killall -9 libreactor-server wrk > /dev/null 2>&1 || true
sleep 1

# Rebuild with optimizations
echo "[3/6] Rebuilding with optimizations..."
echo "  Cleaning..."
make clean > /dev/null 2>&1

echo "  Building third-party..."
if ! make third_party > /dev/null 2>&1; then
    echo "❌ Third-party build failed!"
    exit 1
fi

echo "  Building libreactor-server..."
if ! make -j$(nproc) libreactor-server > /dev/null 2>&1; then
    echo "❌ Build failed!"
    exit 1
fi

echo "  ✓ Build complete: $(ls -lh libreactor-server | awk '{print $5}')"
echo ""

# Start server directly (don't use run-optimized.sh to avoid double rebuild)
echo "[4/6] Starting server instances..."
/usr/bin/taskset -c 0 ./libreactor-server --disable-log &
sleep 0.1
/usr/bin/taskset -c 1 ./libreactor-server --disable-log &
sleep 0.1
/usr/bin/taskset -c 2 ./libreactor-server --disable-log &
SERVER_PIDS=$(pgrep -f libreactor-server | tr '\n' ' ')
echo "  Server instances started: $SERVER_PIDS"
sleep 3

# Verify server
echo "  Verifying server health..."
if ! curl -s --max-time 3 http://localhost:3984/json > /dev/null 2>&1; then
    echo "❌ Server not responding!"
    pkill -9 libreactor-server 2>/dev/null || true
    exit 1
fi
echo "  ✓ Server responding"
echo ""

# Start perf recording
echo "[5/6] Starting perf profiling..."
echo "  Recording for ${PERF_DURATION} seconds with call graphs (${PERF_CALL_GRAPH})..."

# Start perf in background
perf record \
    -F $PERF_FREQUENCY \
    -g \
    --call-graph $PERF_CALL_GRAPH \
    -a \
    -o "$OUTPUT_DIR/perf.data" \
    sleep $PERF_DURATION &
PERF_PID=$!

sleep 2

# Start load test
echo "[6/6] Running load test..."
wrk -t${WRK_THREADS} -c${WRK_CONNECTIONS} -d${WRK_DURATION}s "$WRK_URL" > "$OUTPUT_DIR/wrk-results.txt" 2>&1 &
WRK_PID=$!
echo "  Load test: ${WRK_THREADS} threads, ${WRK_CONNECTIONS} connections, ${WRK_DURATION}s"
echo "  Target: $WRK_URL"

# Wait for perf to complete
wait $PERF_PID 2>/dev/null || true
echo ""
echo "✓ Profiling complete"

# Stop everything
echo ""
echo "Stopping server and load test..."
kill -9 $WRK_PID 2>/dev/null || true
pkill -9 libreactor-server 2>/dev/null || true
sleep 1

# Generate reports
echo ""
echo "=== Generating Reports ==="

cd "$OUTPUT_DIR"

# Top functions report (simplified, no call graphs to avoid hanging)
echo "Generating top functions report..."
if timeout 30 perf report -i perf.data --stdio --sort comm,dso,symbol --percent-limit 1.0 --no-call-graph > report-functions.txt 2>/dev/null; then
    echo "  ✓ report-functions.txt"
else
    echo "  ⚠️  Report generation timed out, trying simpler format..."
    # Fallback: even simpler report
    timeout 15 perf report -i perf.data --stdio --percent-limit 2.0 > report-functions-simple.txt 2>/dev/null || true
fi

# Quick symbol summary
echo "Generating symbol summary..."
timeout 10 perf report -i perf.data --stdio -n --percent-limit 1.0 > report-symbols.txt 2>/dev/null || echo "  (skipped)"

# Flame graph data (lightweight)
echo "Generating flame graph data..."
if timeout 20 perf script -i perf.data > perf-script.txt 2>/dev/null; then
    echo "  ✓ perf-script.txt (for flame graphs)"
else
    echo "  ⚠️  Flame graph data generation timed out"
fi

cd - > /dev/null

# Show summary
echo ""
echo "=== Profiling Results ==="
echo "Data saved to: $OUTPUT_DIR"
echo ""

# Show top hotspots if report exists
if [ -f "$OUTPUT_DIR/report-functions.txt" ] && [ -s "$OUTPUT_DIR/report-functions.txt" ]; then
    echo "📊 Top Hottest Functions:"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    head -40 "$OUTPUT_DIR/report-functions.txt" | tail -30
    echo ""
elif [ -f "$OUTPUT_DIR/report-functions-simple.txt" ]; then
    echo "📊 Top Hottest Functions (simplified):"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    head -40 "$OUTPUT_DIR/report-functions-simple.txt" | tail -30
    echo ""
fi

# Show wrk results
echo "📈 Load Test Results:"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
cat "$OUTPUT_DIR/wrk-results.txt"
echo ""

echo "🔍 Analysis Commands:"
echo "  perf report -i $OUTPUT_DIR/perf.data"
echo "  perf annotate -i $OUTPUT_DIR/perf.data"
echo "  cat $OUTPUT_DIR/report-functions.txt | less"
echo ""

# Generate flame graph if tools available
if [ -d "/var/www/rads/FlameGraph" ]; then
    echo "Generating flame graph..."
    /var/www/rads/FlameGraph/stackcollapse-perf.pl "$OUTPUT_DIR/perf-script.txt" > "$OUTPUT_DIR/perf-folded.txt"
    /var/www/rads/FlameGraph/flamegraph.pl "$OUTPUT_DIR/perf-folded.txt" > "$OUTPUT_DIR/flamegraph.svg"
    echo "  ✓ flamegraph.svg"
    echo ""
    echo "View flame graph: firefox $OUTPUT_DIR/flamegraph.svg"
fi

echo "✅ Profiling completed successfully!"
