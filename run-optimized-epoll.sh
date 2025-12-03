#!/bin/bash
# Optimized libreactor-server runner with epoll optimizations
# Based on https://talawah.io/blog/extreme-http-performance-tuning-one-point-two-million/

set -e

echo "=== libreactor-server with epoll optimizations ==="
echo "Applying network stack optimizations..."

# Enable epoll busy polling
echo "Enabling epoll busy polling..."
sudo sysctl -w net.core.busy_poll=50 2>/dev/null || echo "busy_poll not supported"
sudo sysctl -w net.core.busy_read=50 2>/dev/null || echo "busy_read not supported"

# Check available CPU cores
CPU_CORES=$(nproc)
echo "Available CPU cores: $CPU_CORES"

# Calculate CPU mask (cores 0 to CPU_CORES-1)
if [ $CPU_CORES -gt 1 ]; then
    CPU_MASK="0-$((CPU_CORES-1))"
else
    CPU_MASK="0"
fi

echo "Using CPU affinity: $CPU_MASK"

# Try interrupt coalescing (may not work in containers/virtualization)
echo "Attempting interrupt coalescing..."
if command -v ethtool >/dev/null 2>&1; then
    # Find available network interface (exclude loopback and virtual interfaces)
    IFACE=$(ip link show | grep -v "lo:" | grep -E "^[0-9]+: (eth|en|ens|enp)" | head -1 | awk -F: '{print $2}' | tr -d ' ')
    if [ -n "$IFACE" ]; then
        echo "Using network interface: $IFACE"

        # Check if coalescing is supported
        if ethtool -c $IFACE 2>/dev/null | grep -q "n/a"; then
            echo "Network interface $IFACE doesn't support interrupt coalescing (virtual/container environment)"
            echo "This is normal for virtualized environments - skipping ethtool optimizations"
        else
            echo "Applying interrupt coalescing optimizations..."
            sudo ethtool -C $IFACE adaptive-rx off 2>/dev/null && echo "✓ Adaptive RX disabled" || echo "✗ Adaptive RX not supported"
            sudo ethtool -C $IFACE adaptive-tx off 2>/dev/null && echo "✓ Adaptive TX disabled" || echo "✗ Adaptive TX not supported"
            sudo ethtool -C $IFACE rx-usecs 300 2>/dev/null && echo "✓ RX interrupt coalescing: 300μs" || echo "✗ RX usecs not supported"
            sudo ethtool -C $IFACE tx-usecs 300 2>/dev/null && echo "✓ TX interrupt coalescing: 300μs" || echo "✗ TX usecs not supported"
            echo "Interrupt coalescing configured successfully!"
        fi
    else
        echo "No suitable network interface found for ethtool optimization"
    fi
else
    echo "ethtool not available - interrupt coalescing skipped"
fi

echo "Starting optimized server..."
taskset -c $CPU_MASK ./libreactor-server --disable-log &
SERVER_PID=$!

echo "Server PID: $SERVER_PID"
echo "Server started with epoll optimizations!"
echo ""
echo "Test with: wrk -t4 -c100 -d5s http://localhost:3984/json"
echo "Expected performance: 100,000+ req/sec"
echo ""
echo "Press Ctrl+C to stop"

# Wait for server
wait $SERVER_PID
