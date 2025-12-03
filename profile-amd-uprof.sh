#!/bin/bash
# AMD uProf 5.1-701 profiling script for EPYC systems (Zen 3/4/5)
# Optimized for KVM/virtualized environments with correct perf settings
#
# USAGE:
#   ./profile-amd-uprof.sh
#
# REQUIREMENTS:
#   - AMD uProf 5.1-701 installed at /opt/AMDuProf_5.1-701/
#   - wrk load testing tool
#
# OUTPUT:
#   - ./amd-profiles/YYYYMMDD_HHMMSS/ directory with:
#     * profile-data/ - Raw profiling data
#     * summary-report.txt - Top functions and hotspots
#     * detailed-report.txt - Full analysis with call stacks
#     * load-results.txt - wrk benchmark results
#
# ANALYSIS:
#   cat ./amd-profiles/*/summary-report.txt | grep -A 15 "10 HOTTEST FUNCTIONS"

set -e  # Exit on error

echo "=== AMD uProf 5.1-701 Profiling for EPYC Systems ==="
echo "Target: CPU + L3 + Memory analysis optimized for virtualized EPYC"
echo ""

# Check if AMD uProf is installed
if ! command -v /opt/AMDuProf_5.1-701/bin/AMDuProfCLI >/dev/null 2>&1; then
    echo "❌ AMD uProf not found!"
    echo ""
    echo "AMD uProf should be installed at /opt/AMDuProf_5.1-701/"
    echo ""
    exit 1
fi

# Define AMDuProfCLI command path for consistent usage
AMDuProfCLI="/opt/AMDuProf_5.1-701/bin/AMDuProfCLI"

# Check and set perf permissions (critical for EPYC profiling)
echo "[1/7] Setting up perf permissions..."
if [ "$(cat /proc/sys/kernel/perf_event_paranoid)" != "-1" ]; then
    echo "  Setting perf_event_paranoid = -1..."
    echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid > /dev/null
    echo "kernel.perf_event_paranoid = -1" | sudo tee /etc/sysctl.d/99-perf.conf > /dev/null
    sudo sysctl -p /etc/sysctl.d/99-perf.conf > /dev/null 2>&1 || true
fi

# Disable NMI watchdog (interferes with profiling)
echo 0 | sudo tee /proc/sys/kernel/nmi_watchdog > /dev/null 2>&1 || true

# Add AMD uProf to PATH
export PATH=/opt/AMDuProf_5.1-701/bin:$PATH

OUTPUT_DIR="./amd-profiles/$(date +%Y%m%d_%H%M%S)"
SESSION_NAME="libreactor-epyc-profile"

mkdir -p "$OUTPUT_DIR"

echo "  Session: $SESSION_NAME"
echo "  Output: $OUTPUT_DIR"
echo "  Perf permissions: $(cat /proc/sys/kernel/perf_event_paranoid)"
echo ""

# Clean start
echo "[2/7] Cleaning previous sessions..."
killall -9 libreactor-server AMDuProfPcm wrk >/dev/null 2>&1 || true
pkill -f "AMDuProfCLI\|wrk" >/dev/null 2>&1 || true
sleep 2

# Rebuild the server with optimizations
echo "[3/7] Rebuilding server with optimizations..."
echo "  Cleaning previous build..."
make clean > /dev/null 2>&1

echo "  Building third-party libraries..."
if ! make third_party > /dev/null 2>&1; then
    echo "❌ Third-party build failed!"
    exit 1
fi

echo "  Building libreactor-server..."
if ! make -j$(nproc) libreactor-server > /dev/null 2>&1; then
    echo "❌ Server build failed!"
    exit 1
fi

if [ ! -f ./libreactor-server ]; then
    echo "❌ libreactor-server binary not found!"
    exit 1
fi

echo "  ✓ Build complete: $(ls -lh libreactor-server | awk '{print $5}')"
echo ""

# AMD uProf profiling configuration for EPYC (system-wide for reliability)
echo "Starting AMD uProf profiling session..."

# Start the server (multiple instances for load distribution)
echo "[4/7] Starting optimized server instances..."
./run-optimized.sh &
SERVER_GROUP_PID=$!
echo "  Server group started (PGID: $SERVER_GROUP_PID)"
sleep 5

# Verify server is responding
echo "[5/7] Verifying server health..."
if curl -s --max-time 3 http://localhost:3984/json > /dev/null 2>&1; then
    echo "  ✅ Server responding"
else
    echo "  ❌ Server not responding on port 3984"
    echo ""
    echo "Checking if any libreactor processes are running..."
    if pgrep -f libreactor-server > /dev/null 2>&1; then
        echo "⚠️  libreactor processes found but server not responding"
        echo "Try restarting: ./stop.sh && ./run-optimized.sh"
    else
        echo "No libreactor processes found"
    fi
    kill -TERM -$SERVER_GROUP_PID 2>/dev/null || true
    exit 1
fi

# Start AMD uProf profiling FIRST (system-wide mode for stability)
echo "[6/7] Starting AMD uProf system-wide profiling..."
$AMDuProfCLI collect \
    --config tbp \
    -a \
    -d 25 \
    -o "$OUTPUT_DIR/profile-data" &
UPROF_PID=$!
echo "  AMDuProf started (PID: $UPROF_PID)"
sleep 3

# Start load testing (optimized for EPYC stress testing)
echo "[7/7] Running optimized load test..."
timeout 20 wrk -t16 -c512 -d15s http://localhost:3984/json > "$OUTPUT_DIR/load-results.txt" 2>&1 &
WRK_PID=$!
echo "  Load test started (PID: $WRK_PID, 16 threads, 512 connections, 15s duration)"

# Give wrk a moment to start and check if it's running
sleep 2
if kill -0 $WRK_PID 2>/dev/null 2>&1; then
    echo "  ✅ wrk is running successfully"
else
    echo "  ⚠️  Warning: wrk exited early - check load-results.txt for errors"
    echo "  This might indicate server overload or network issues"
fi

# Wait for profiling to complete
echo ""
echo "Waiting for profiling to complete (25 seconds)..."
wait $UPROF_PID 2>/dev/null || true
echo "✓ Profiling completed."

# Stop load test and servers
echo ""
echo "Stopping load test and servers..."
kill -9 $WRK_PID 2>/dev/null || true
kill -9 -$SERVER_GROUP_PID 2>/dev/null || true
pkill -9 -f "libreactor-server\|wrk" > /dev/null 2>&1 || true
wait 2>/dev/null || true

# Generate analysis report
echo ""
echo "=== Generating Analysis Reports ==="

# Find the actual data directory (AMDuProf creates subdirectory with timestamp)
DATA_DIR=$(find "$OUTPUT_DIR/profile-data" -maxdepth 1 -type d -name "AMDuProf-*" | head -1)

if [ -d "$DATA_DIR" ]; then
    echo "✅ Profile data collected: $DATA_DIR"
    echo ""

    # Always generate summary report first (critical)
    echo "Generating summary report..."
    if $AMDuProfCLI report -i "$DATA_DIR" --stdout > "$OUTPUT_DIR/summary-report.txt" 2>/dev/null; then
        echo "  ✅ Summary report generated"
    else
        echo "  ❌ Failed to generate summary report"
        # Continue anyway, summary is critical
    fi

    # Generate detailed report with timeout (can be memory intensive)
    echo "Generating detailed report (may take time)..."
    if timeout 30 $AMDuProfCLI report -i "$DATA_DIR" --detail --stdout > "$OUTPUT_DIR/detailed-report.txt" 2>/dev/null; then
        echo "  ✅ Detailed report generated"
    else
        echo "  ⚠️  Detailed report generation failed or timed out (this is normal for large datasets)"
        echo "  Summary report is still available for analysis"
    fi

    # Show load test results
    echo ""
    echo "Load test results:"
    cat "$OUTPUT_DIR/load-results.txt" 2>/dev/null || echo "No load test results found"

    echo ""
    echo "=== AMD EPYC Profiling Complete ==="
    echo "Data saved to: $OUTPUT_DIR"
    echo "Raw data: $DATA_DIR"
    echo ""
    echo "📊 Reports generated:"
    echo "  - Summary: $OUTPUT_DIR/summary-report.txt"
    if [ -f "$OUTPUT_DIR/detailed-report.txt" ] && [ -s "$OUTPUT_DIR/detailed-report.txt" ]; then
        echo "  - Detailed: $OUTPUT_DIR/detailed-report.txt"
    else
        echo "  - Detailed: Not generated (memory/time constraints)"
    fi
    echo "  - Load results: $OUTPUT_DIR/load-results.txt"
    echo ""
    echo "🔍 Quick analysis commands:"
    echo "  cat $OUTPUT_DIR/summary-report.txt | grep -A 15 '10 HOTTEST FUNCTIONS'"
    echo "  cat $OUTPUT_DIR/summary-report.txt | grep -A 10 '10 HOTTEST MODULES'"
    if [ -f "$OUTPUT_DIR/detailed-report.txt" ] && [ -s "$OUTPUT_DIR/detailed-report.txt" ]; then
        echo "  less $OUTPUT_DIR/detailed-report.txt"
    fi
    echo ""
    echo "✅ Profiling completed successfully!"

else
    echo "❌ No profile data generated in $OUTPUT_DIR/profile-data"
    ls -la "$OUTPUT_DIR/profile-data" 2>/dev/null || echo "Profile data directory not found"
    exit 1
fi
