#!/bin/bash

# Compile libreactor with ASan/LSan for memory leak detection

set -e  # Exit on error

echo "=== Compiling Libreactor with ASan/LSan (Address/Leak Sanitizer) ==="

# ASan/LSan flags
ASAN_FLAGS="-fsanitize=address,leak -fno-omit-frame-pointer -g -O1"

# Clean everything first
echo "Cleaning previous build..."
make clean 2>/dev/null || true
rm -rf third_party/libreactor/.libs third_party/libclo/.libs 2>/dev/null || true

# Step 1: Build libreactor with ASan
echo ""
echo "Step 1/3: Building libreactor with ASan..."
cd third_party/libreactor
make clean 2>/dev/null || true
./autogen.sh >/dev/null 2>&1 || true
./configure CFLAGS="$ASAN_FLAGS -march=core2" \
            LDFLAGS="-fsanitize=address,leak" \
            --enable-static \
            --disable-shared >/dev/null
make -j$(nproc) >/dev/null
cd ../..
echo "✓ libreactor compiled with ASan"

# Step 2: Build libclo with ASan
echo ""
echo "Step 2/3: Building libclo with ASan..."
cd third_party/libclo
make clean 2>/dev/null || true
./autogen.sh >/dev/null 2>&1 || true
./configure CFLAGS="$ASAN_FLAGS -march=core2" \
            LDFLAGS="-fsanitize=address,leak" \
            --enable-static \
            --disable-shared >/dev/null
make -j$(nproc) >/dev/null
cd ../..
echo "✓ libclo compiled with ASan"

# Step 3: Build main application
echo ""
echo "Step 3/3: Building libreactor-server with ASan..."

# Create build directories
mkdir -p build/{platform,domain,infrastructure,main}

# Compile flags
CFLAGS="-std=gnu2x -Wall -Wextra -Wpedantic $ASAN_FLAGS -march=core2"
INCLUDES="-Isrc/include -Isrc/include/platform -Isrc/include/domain -Isrc/include/infrastructure -Ithird_party/libreactor/src -Ithird_party/libdynamic"
LDFLAGS="-fsanitize=address,leak"
LIBS="-lssl -lcrypto -ldl -lpthread"

# Compile all source files (exclude libreactor.c to avoid main conflict)
for src in src/platform/*.c src/domain/*.c src/infrastructure/*.c src/main/libreactor-server.c; do
    obj="build/$(echo $src | sed 's|src/||' | sed 's|\.c$|.o|')"
    echo "Compiling $(basename $src)..."
    gcc $CFLAGS $INCLUDES -MMD -MP -c $src -o $obj
done

# Link everything
echo ""
echo "Linking libreactor-server..."
gcc $LDFLAGS -o libreactor-server \
    build/platform/*.o \
    build/domain/*.o \
    build/infrastructure/*.o \
    build/main/*.o \
    third_party/libreactor/.libs/libreactor.a \
    third_party/libclo/.libs/libclo.a \
    $LIBS

# Check if successful
if [ -f libreactor-server ]; then
    SIZE=$(du -h libreactor-server | cut -f1)
    echo ""
    echo "✅ ASan/LSan Compilation successful!"
    echo "Binary: libreactor-server ($SIZE)"
    echo ""
    echo "⚠️  IMPORTANT: This build includes AddressSanitizer and LeakSanitizer"
    echo "   - Memory leaks will be detected automatically at exit"
    echo "   - Use-after-free and buffer overflows will be caught"
    echo "   - Performance: ~2x slower than normal build (5x faster than Valgrind)"
    echo "   - Memory usage: ~3x higher due to shadow memory"
    echo ""
    echo "Environment variables for tuning:"
    echo "  export ASAN_OPTIONS='detect_leaks=1:symbolize=1:abort_on_error=0'"
    echo "  export LSAN_OPTIONS='report_objects=1:print_suppressions=0'"
    echo ""
    echo "Usage:"
    echo "  ./libreactor-server                    # Run with leak detection"
    echo "  ASAN_OPTIONS='log_path=asan.log' ./libreactor-server  # Log to file"
    echo ""
    echo "On exit or crash, ASan will show detailed reports like:"
    echo "  Direct leak of 32768 byte(s) in 1 object(s)"
    echo "      #0 0x7f123... in malloc"
    echo "      #1 0x456... in your_function src/file.c:123"
else
    echo ""
    echo "❌ Compilation failed!"
    exit 1
fi
