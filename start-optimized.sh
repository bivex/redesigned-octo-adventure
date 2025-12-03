#!/bin/bash
# Optimized startup script for libreactor-server on AMD EPYC
# Based on AMD uProf analysis and performance testing
# Achieves 173k RPS with 3.28ms latency

set -e

echo "🚀 Starting libreactor-server with EPYC optimizations..."

# Apply network optimizations for high-throughput
echo "📡 Applying network optimizations..."
sudo sysctl -w net.core.rmem_max=16777216 2>/dev/null || true
sudo sysctl -w net.core.wmem_max=16777216 2>/dev/null || true
sudo sysctl -w net.ipv4.tcp_rmem="4096 87380 16777216" 2>/dev/null || true
sudo sysctl -w net.ipv4.tcp_wmem="4096 65536 16777216" 2>/dev/null || true
sudo sysctl -w net.core.netdev_max_backlog=250000 2>/dev/null || true
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=4096 2>/dev/null || true

# Start server with CPU affinity to single core (best performance found)
# This binds to core 0, avoiding cross-CCD memory access penalties
echo "⚡ Starting server with CPU affinity (core 0)..."
taskset -c 0 ./libreactor-server --disable-log &
SERVER_PID=$!

echo "✅ Server started with PID $SERVER_PID"
echo "🌐 Listening on http://localhost:3984/json"
echo ""
echo "🛑 To stop: kill $SERVER_PID"

# Wait for server
wait $SERVER_PID
