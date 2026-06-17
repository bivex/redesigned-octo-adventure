#!/bin/bash
# System tuning script for libreactor performance optimization.
# Validated on Ubuntu 24.04 aarch64 Lima VM (Apple Virtualization Framework).
# Effect: +5-7% RPS at high concurrency (256+ connections) on loopback;
# negligible at low concurrency. All values A/B-measured.

set -e

echo "=== Libreactor Performance Tuning Script ==="
echo "This script requires root privileges"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
   echo "Please run as root (sudo ./tune_system.sh)"
   exit 1
fi

echo "[1/4] Increasing socket buffers to reduce copy overhead..."
sysctl -w net.core.rmem_max=16777216
sysctl -w net.core.wmem_max=16777216
sysctl -w net.core.rmem_default=1048576
sysctl -w net.core.wmem_default=1048576
sysctl -w net.ipv4.tcp_rmem="4096 87380 16777216"
sysctl -w net.ipv4.tcp_wmem="4096 65536 16777216"

echo "[2/4] Connection limits (server calls listen(fd, INT_MAX); kernel clamps to somaxconn)..."
sysctl -w net.core.somaxconn=32768
sysctl -w net.core.netdev_max_backlog=250000
sysctl -w net.ipv4.tcp_max_syn_backlog=32768

echo "[3/4] TCP..."
sysctl -w net.ipv4.tcp_fastopen=3
sysctl -w net.ipv4.tcp_tw_reuse=2
sysctl -w net.ipv4.tcp_no_metrics_save=1

echo "[4/4] Port range for ephemeral clients..."
sysctl -w net.ipv4.ip_local_port_range="10000 65535"

echo ""
echo "=== Tuning Complete ==="
echo ""
echo "NOTE on conntrack bypass (NOTRACK for lo):"
echo "  Tested — REGRESSED at high concurrency (4t/256c: 1089k -> 528k RPS"
echo "  with high variance) on this loopback/vz setup. Do NOT apply."
echo ""
echo "To make persistent, copy to /etc/sysctl.d/99-libreactor.conf"
