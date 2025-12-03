#!/bin/bash
# Optimized libreactor runner with clean rebuild, CPU pinning and performance settings

set -e  # Exit on error

echo "=== Libreactor Optimized Build & Run ==="
echo ""

# Stop any existing libreactor processes and clean up port
echo "[1/5] Stopping existing libreactor processes..."
./stop.sh 2>/dev/null || true

# Clean previous build
echo "[2/5] Cleaning previous build..."
make clean > /dev/null 2>&1

# Build third-party libraries first
echo "[3/5] Building third-party libraries..."
if ! make third_party 2>&1 | tee /tmp/build_third_party.log | tail -5; then
    echo "❌ Third-party build failed! Check /tmp/build_third_party.log"
    exit 1
fi

# Rebuild main application with optimizations
echo "[4/5] Building main application..."
if make -j$(nproc) libreactor-server 2>&1 | grep -E "(error|Error|ERROR)" ; then
    echo "❌ Build failed! Check errors above."
    exit 1
fi

# Verify binary exists
if [ ! -f ./libreactor-server ]; then
    echo "❌ libreactor-server binary not found!"
    exit 1
fi

echo "✓ Build complete: $(ls -lh libreactor-server | awk '{print $5}')"
echo ""

# Setup CPU affinity for network interrupts (if available)
echo "[5/6] Setting up CPU affinity..."
if [ -f /usr/local/bin/setup-irq-affinity.sh ]; then
    /usr/local/bin/setup-irq-affinity.sh 2>/dev/null || echo "  (IRQ affinity setup skipped)"
else
    echo "  (IRQ affinity script not found, skipping)"
fi

# Run libreactor with CPU pinning (one process per CPU) and disable logging
echo "[6/6] Starting libreactor-server with CPU pinning..."
/usr/bin/taskset -c 0 ./libreactor-server --disable-log &
sleep 0.1
/usr/bin/taskset -c 1 ./libreactor-server --disable-log &
sleep 0.1
/usr/bin/taskset -c 2 ./libreactor-server --disable-log &

echo ""
echo "=== Libreactor Started ==="
echo "  Processes: 3 (one per CPU)"
echo "  Logging:   Disabled"
echo "  Port:      3984"
echo ""
echo "Test with: curl http://localhost:3984/json"
echo "Benchmark: wrk -t12 -c400 -d30s http://localhost:3984/plaintext"
echo ""

# Wait for processes
wait
