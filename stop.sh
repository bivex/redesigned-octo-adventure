#!/bin/bash
# Stop all libreactor processes and clean up port 3984

echo "=== Stopping Libreactor Processes ==="

# Find and kill all libreactor processes
LIBREACTOR_PIDS=$(ps aux | grep "libreactor-server" | grep -v grep | awk '{print $2}')

if [ -z "$LIBREACTOR_PIDS" ]; then
    echo "No libreactor processes found."
else
    echo "Found libreactor processes: $LIBREACTOR_PIDS"
    echo "Stopping processes..."

    # Kill processes gracefully first
    kill $LIBREACTOR_PIDS 2>/dev/null

    # Wait up to 5 seconds for graceful shutdown
    for i in {1..10}; do
        if ! ps -p $LIBREACTOR_PIDS > /dev/null 2>&1; then
            break
        fi
        sleep 0.5
    done

    # Force kill if still running
    if ps -p $LIBREACTOR_PIDS > /dev/null 2>&1; then
        echo "Force killing remaining processes..."
        kill -9 $LIBREACTOR_PIDS 2>/dev/null
        sleep 1
    fi

    echo "✅ All libreactor processes stopped."
fi

# Check if port 3984 is still in use
echo ""
echo "Checking port 3984..."
if netstat -tlnp 2>/dev/null | grep ":3984 " > /dev/null; then
    echo "⚠️  Port 3984 is still in use. Waiting for cleanup..."

    # Wait up to 10 seconds for port to be freed
    for i in {1..20}; do
        if ! netstat -tlnp 2>/dev/null | grep ":3984 " > /dev/null; then
            echo "✅ Port 3984 is now free."
            break
        fi
        sleep 0.5
    done

    # If still in use, show what's using it
    if netstat -tlnp 2>/dev/null | grep ":3984 " > /dev/null; then
        echo "❌ Port 3984 still in use by:"
        netstat -tlnp 2>/dev/null | grep ":3984 "
        echo "You may need to manually stop the process."
        exit 1
    fi
else
    echo "✅ Port 3984 is free."
fi

# Clean up any temporary files if needed
echo ""
echo "Cleaning up temporary files..."
# Add any cleanup commands here if needed

echo ""
echo "=== Libreactor Stopped and Cleaned ==="
echo "Ready to restart with: ./run-optimized.sh"
