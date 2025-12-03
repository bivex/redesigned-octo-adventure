#!/bin/bash
# Performance analysis script for libreactor_postgres examples
# Profiles the example programs to analyze performance hotspots

set -e

# ============================================================================
# CONFIGURATION - Adjust these settings as needed
# ============================================================================

# Profiling settings
PERF_FREQUENCY=999              # Sampling frequency (Hz)
PERF_CALL_GRAPH="fp"            # Call graph method: fp (fast), dwarf (detailed but slow), or lbr

# Example program settings
EXAMPLE_PROGRAM="queries"       # "query_lowlevel" or "queries"
EXAMPLE_ARGS="2 50 'SELECT count(*) FROM test_data'"  # For queries: <parallel> <count> <command> - lightweight query

# Build settings
BUILD_OPTIMIZED=true           # Whether to build with optimizations
BUILD_WITH_ASAN=false          # Whether to build with AddressSanitizer

# Output settings
OUTPUT_BASE="./perf-analysis"   # Base directory for results

# ============================================================================

echo "=== Performance Analysis for Libreactor Postgres Examples ==="
echo ""
echo "Configuration:"
echo "  Perf frequency: ${PERF_FREQUENCY} Hz"
echo "  Call graph: ${PERF_CALL_GRAPH}"
echo "  Example program: ${EXAMPLE_PROGRAM}"
echo "  Example args: ${EXAMPLE_ARGS}"
echo "  Optimized build: ${BUILD_OPTIMIZED}"
echo "  Build with ASAN: ${BUILD_WITH_ASAN}"
echo ""

# Check if perf is installed
if ! command -v perf > /dev/null 2>&1; then
    echo "❌ perf not found!"
    echo "Install with: apt-get install linux-perf"
    exit 1
fi

# Check perf permissions
echo "[1/4] Checking perf permissions..."
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

# Clean previous builds
echo "[2/4] Preparing build..."
make clean > /dev/null 2>&1

# Build with appropriate flags
if [ "$BUILD_OPTIMIZED" = true ]; then
    echo "  Building with optimizations..."
    if [ "$BUILD_WITH_ASAN" = true ]; then
        make CFLAGS="-std=gnu11 -g -O3 -flto -fuse-linker-plugin -I./src -I/usr/include/postgresql -I../libreactor/src -Wall -Wextra -Wpedantic" -j$(nproc) example/${EXAMPLE_PROGRAM} > /dev/null 2>&1
    else
        make CFLAGS="-std=gnu11 -g -O3 -flto -fuse-linker-plugin -I./src -I/usr/include/postgresql -I../libreactor/src -Wall -Wextra -Wpedantic" LDFLAGS="-static -lpq" AM_CFLAGS="-std=gnu11 -g -O3 -flto -fuse-linker-plugin -I./src -I/usr/include/postgresql -I../libreactor/src -Wall -Wextra -Wpedantic" AM_LDFLAGS="-static -lpq" -j$(nproc) example/${EXAMPLE_PROGRAM} > /dev/null 2>&1
    fi
else
    echo "  Building with debug flags..."
    if [ "$BUILD_WITH_ASAN" = true ]; then
        make CFLAGS="-std=gnu11 -g -O0 -I./src -I/usr/include/postgresql -I../libreactor/src -Wall -Wextra -Wpedantic" -j$(nproc) example/${EXAMPLE_PROGRAM} > /dev/null 2>&1
    else
        make CFLAGS="-std=gnu11 -g -O0 -I./src -I/usr/include/postgresql -I../libreactor/src -Wall -Wextra -Wpedantic" LDFLAGS="-static -lpq" AM_CFLAGS="-std=gnu11 -g -O0 -I./src -I/usr/include/postgresql -I../libreactor/src -Wall -Wextra -Wpedantic" AM_LDFLAGS="-static -lpq" -j$(nproc) example/${EXAMPLE_PROGRAM} > /dev/null 2>&1
    fi
fi

if ! [ -f "example/${EXAMPLE_PROGRAM}" ]; then
    echo "❌ Build failed!"
    exit 1
fi

echo "  ✓ Build complete: $(ls -lh example/${EXAMPLE_PROGRAM} | awk '{print $5}')"
echo ""

# Validate example program
echo "[3/4] Validating example program..."
if [ "$EXAMPLE_PROGRAM" = "queries" ]; then
    # Check if we have the right number of arguments
    # Parse arguments properly, handling quoted strings
    if [ -z "$EXAMPLE_ARGS" ]; then
        echo "❌ queries program needs exactly 3 arguments: <parallel> <count> <command>"
        exit 1
    fi
    # We'll validate the arguments when we actually run the command
elif [ "$EXAMPLE_PROGRAM" = "query_lowlevel" ]; then
    if [ -n "$EXAMPLE_ARGS" ]; then
        echo "⚠️  query_lowlevel doesn't take arguments, ignoring: $EXAMPLE_ARGS"
        EXAMPLE_ARGS=""
    fi
else
    echo "❌ Unknown example program: $EXAMPLE_PROGRAM"
    echo "Available: query_lowlevel, queries"
    exit 1
fi

echo "  ✓ Configuration valid"
echo ""

# Start perf recording
echo "[4/4] Starting perf profiling..."
echo "  Recording performance data..."

# Run the example program under perf
if [ -n "$EXAMPLE_ARGS" ]; then
    # Use eval to properly parse the arguments
    eval "perf record \
        -F $PERF_FREQUENCY \
        -g \
        --call-graph $PERF_CALL_GRAPH \
        -o \"$OUTPUT_DIR/perf.data\" \
        ./example/${EXAMPLE_PROGRAM} $EXAMPLE_ARGS"
else
    perf record \
        -F $PERF_FREQUENCY \
        -g \
        --call-graph $PERF_CALL_GRAPH \
        -o "$OUTPUT_DIR/perf.data" \
        ./example/${EXAMPLE_PROGRAM}
fi

echo ""
echo "✓ Profiling complete"

# Generate reports
echo ""
echo "=== Generating Reports ==="

cd "$OUTPUT_DIR"

# Top functions report
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

# Annotate key functions
echo "Generating source annotations..."
timeout 15 perf annotate -i perf.data --stdio --percent-limit 5.0 > report-annotations.txt 2>/dev/null || echo "  (skipped)"

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

# Show perf stats
echo "📈 Performance Statistics:"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
perf stat -i "$OUTPUT_DIR/perf.data" --table 2>/dev/null || echo "  (perf stat data not available)"
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

echo "✅ Performance analysis completed successfully!"
