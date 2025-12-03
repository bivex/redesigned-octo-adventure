#!/bin/bash
# Compile libreactor with optimizations

echo "=== Compiling Libreactor with Optimizations ==="

# Clean previous build
make clean

# Build third-party dependencies first
echo "Building third-party dependencies..."
make third_party

# Compile with maximum optimizations
make CFLAGS="-std=gnu11 -Wall -Wextra -Wpedantic -O3 -march=native -flto -DNDEBUG -fomit-frame-pointer -funroll-loops" libreactor-server

# Check if compilation succeeded
if [ $? -eq 0 ]; then
    echo ""
    echo "✅ Compilation successful!"
    echo "Binary: libreactor-server ($(ls -lh libreactor-server 2>/dev/null | awk '{print $5}' || echo 'size unknown'))"
    echo ""
    echo "Available commands:"
    echo "  ./run-optimized.sh    # Start optimized server"
    echo "  ./stop.sh            # Stop server and cleanup"
    echo "  ./status.sh          # Check server status"
    echo "  ./compile.sh         # Recompile with optimizations"
    echo ""
    echo "Benchmark commands:"
    echo "  wrk -t8 -c512 -d10s http://localhost:2342/plaintext"
    echo "  /var/www/benchmark-libreactor.sh"
else
    echo ""
    echo "❌ Compilation failed!"
    exit 1
fi
