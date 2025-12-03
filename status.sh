#!/bin/bash
# Check libreactor status

echo "=== Libreactor Status ==="

# Check if processes are running
LIBREACTOR_PIDS=$(ps aux | grep "libreactor-server" | grep -v grep | awk '{print $2}')
LIBREACTOR_COUNT=$(echo "$LIBREACTOR_PIDS" | wc -l)

if [ -z "$LIBREACTOR_PIDS" ]; then
    echo "❌ No libreactor processes running"
else
    echo "✅ $LIBREACTOR_COUNT libreactor process(es) running:"
    ps aux | grep "libreactor-server" | grep -v grep | while read line; do
        pid=$(echo "$line" | awk '{print $2}')
        cpu=$(echo "$line" | awk '{print $3}')
        rss=$(echo "$line" | awk '{print $6}')
        start=$(echo "$line" | awk '{print $9}')
        echo "  PID $pid: CPU ${cpu}%, RSS ${rss}KB, started $start"
    done
fi

echo ""

# Check port 2342
if netstat -tlnp 2>/dev/null | grep ":2342 " > /dev/null; then
    echo "✅ Port 2342 is in use by:"
    netstat -tlnp 2>/dev/null | grep ":2342 " | while read line; do
        process=$(echo "$line" | awk '{print $7}')
        echo "  $process"
    done
else
    echo "❌ Port 2342 is free"
fi

echo ""

# Quick functionality test
if curl -s --max-time 1 http://localhost:2342/plaintext > /dev/null 2>&1; then
    echo "✅ HTTP server responding on port 2342"
else
    echo "❌ HTTP server not responding on port 2342"
fi

echo ""

# Show CPU and memory usage
echo "System resources:"
echo "  CPU cores: $(nproc)"
echo "  Load average: $(uptime | awk -F'load average:' '{print $2}')"
echo "  Memory: $(free -h | grep "^Mem:" | awk '{print "Total: "$2", Used: "$3", Free: "$4}')"
