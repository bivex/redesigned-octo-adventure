#!/bin/bash
# System tuning script for libreactor performance optimization
# Based on AMD uProf analysis showing significant kernel overhead

set -e

echo "=== Libreactor Performance Tuning Script ==="
echo "This script requires root privileges"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
   echo "Please run as root (sudo ./tune_system.sh)"
   exit 1
fi

echo "[1/5] Increasing socket buffers to reduce copy overhead..."
sysctl -w net.core.rmem_max=16777216
sysctl -w net.core.wmem_max=16777216
sysctl -w net.core.rmem_default=262144
sysctl -w net.core.wmem_default=262144

echo "[2/5] Enabling TCP fast open..."
sysctl -w net.ipv4.tcp_fastopen=3

echo "[3/5] Reducing TIME_WAIT sockets..."
sysctl -w net.ipv4.tcp_tw_reuse=1
sysctl -w net.ipv4.tcp_fin_timeout=15

echo "[4/5] CRITICAL: Bypassing netfilter for localhost (saves 1.77s CPU time)..."
echo "  This disables connection tracking for loopback interface"
# Add iptables rules to bypass connection tracking for lo interface
iptables -t raw -I PREROUTING -i lo -j NOTRACK 2>/dev/null || echo "  (rule may already exist)"
iptables -t raw -I OUTPUT -o lo -j NOTRACK 2>/dev/null || echo "  (rule may already exist)"

echo "[5/5] Increasing connection limits..."
sysctl -w net.core.somaxconn=4096
sysctl -w net.ipv4.tcp_max_syn_backlog=4096

echo "[5/5] Setting port range for SO_REUSEPORT..."
sysctl -w net.ipv4.ip_local_port_range="10000 65535"

echo ""
echo "=== Tuning Complete ==="
echo "Performance improvements applied:"
echo "  - Larger socket buffers (reduces copy overhead)"
echo "  - TCP fast open enabled"
echo "  - Netfilter bypassed for localhost (saves ~1.77s CPU)"
echo "  - Increased connection limits"
echo ""
echo "To make these changes persistent, add them to /etc/sysctl.conf"
echo "To persist iptables rules, use iptables-save"
