# Compiler and flags
CC       = gcc
# Optimized flags based on AMD uProf analysis:
# -O3: Aggressive optimization
# -march=native: AMD EPYC-specific optimizations
# -flto: Link-time optimization
# -fno-omit-frame-pointer: Better profiling support
# -ffast-math: Floating-point optimizations
# -funroll-loops: Loop unrolling
CFLAGS   = -std=gnu2x -Wall -Wextra -Wpedantic -O3 -g -march=native -flto -fno-omit-frame-pointer -ffast-math -funroll-loops -MMD -MP -DHAVE_MSGPACK
CPPFLAGS = -Isrc/include -Isrc/include/platform -Isrc/include/domain -Isrc/include/infrastructure -Ithird_party/libreactor/src -Ithird_party/libdynamic
LDADD    = third_party/libreactor/.libs/libreactor.a third_party/libclo/.libs/libclo.a -lssl -lcrypto -ldl -luring -lmsgpackc

# Build directory
BUILD_DIR = build

# Source files by module
PLATFORM_SRCS = \
	src/platform/system.c \
	src/platform/process.c \
	src/platform/socket.c \
	src/platform/log.c \
	src/platform/signals.c

DOMAIN_SRCS = \
	src/domain/http_response.c \
	src/domain/http_server.c

INFRASTRUCTURE_SRCS = \
	src/infrastructure/server_infrastructure.c \
	src/infrastructure/binary_server.c

MAIN_SRCS = \
	src/main/libreactor.c \
	src/main/libreactor-server.c

# binary protocol server (isolated from HTTP path)
BINARY_MAIN_SRC = src/main/libreactor-binary-server.c
BINARY_MAIN_OBJ = $(BINARY_MAIN_SRC:src/%.c=$(BUILD_DIR)/%.o)

# Object files (in build directory)
PLATFORM_OBJS = $(PLATFORM_SRCS:src/%.c=$(BUILD_DIR)/%.o)
DOMAIN_OBJS = $(DOMAIN_SRCS:src/%.c=$(BUILD_DIR)/%.o)
INFRASTRUCTURE_OBJS = $(INFRASTRUCTURE_SRCS:src/%.c=$(BUILD_DIR)/%.o)
MAIN_OBJS = $(MAIN_SRCS:src/%.c=$(BUILD_DIR)/%.o)

# All objects
ALL_OBJS = $(PLATFORM_OBJS) $(DOMAIN_OBJS) $(INFRASTRUCTURE_OBJS) $(MAIN_OBJS)

# Build targets
.PHONY: all clean third_party

all: third_party libreactor libreactor-server libreactor-binary-server binary-loadgen binary-loadgen-uring

# Build third_party libraries
third_party:
	@echo "=== Building Third-Party Libraries ==="
	@if [ ! -f third_party/libdynamic/Makefile ]; then \
		echo "  [CONF] libdynamic"; \
		cd third_party/libdynamic && ./autogen.sh > /dev/null 2>&1 && ./configure > /dev/null 2>&1; \
	fi
	@echo "  [MAKE] libdynamic"
	@$(MAKE) -C third_party/libdynamic > /dev/null 2>&1
	@echo "  ✓ libdynamic"
	@if [ ! -f third_party/libreactor/Makefile ]; then \
		echo "  [CONF] libreactor"; \
		cd third_party/libreactor && autoreconf -i > /dev/null 2>&1 && ./configure > /dev/null 2>&1; \
	fi
	@echo "  [MAKE] libreactor"
	@$(MAKE) -C third_party/libreactor > /dev/null 2>&1
	@echo "  ✓ libreactor"
	@if [ ! -f third_party/libclo/Makefile ]; then \
		echo "  [CONF] libclo"; \
		cd third_party/libclo && ./autogen.sh > /dev/null 2>&1 && ./configure --prefix=/usr AR=gcc-ar NM=gcc-nm RANLIB=gcc-ranlib > /dev/null 2>&1; \
	fi
	@echo "  [MAKE] libclo"
	@$(MAKE) -C third_party/libclo > /dev/null 2>&1
	@echo "  ✓ libclo"
	@echo "=== Third-Party Libraries Complete ==="

# Main executables
libreactor: $(PLATFORM_OBJS) $(DOMAIN_OBJS) $(INFRASTRUCTURE_OBJS) $(BUILD_DIR)/main/libreactor.o
	@echo "  [LD]  $@"
	@$(CC) -o $@ $^ $(LDADD)
	@echo "✓ Built: $@"

libreactor-server: $(PLATFORM_OBJS) $(DOMAIN_OBJS) $(INFRASTRUCTURE_OBJS) $(BUILD_DIR)/main/libreactor-server.o
	@echo "  [LD]  $@"
	@$(CC) -o $@ $^ $(LDADD)
	@echo "✓ Built: $@"

libreactor-binary-server: $(PLATFORM_OBJS) $(DOMAIN_OBJS) $(INFRASTRUCTURE_OBJS) $(BINARY_MAIN_OBJ)
	@echo "  [LD]  $@"
	@$(CC) -o $@ $^ $(LDADD)
	@echo "✓ Built: $@"

# Binary protocol load generator (standalone pthread TCP client)
binary-loadgen: bench/binary_loadgen.c
	@echo "  [CC/LD] $@"
	@$(CC) -O3 -Wall -pthread -o $@ $< -DHAVE_MSGPACK $(shell pkg-config --cflags --libs msgpack 2>/dev/null || echo "-lmsgpackc")
	@echo "✓ Built: $@"

# Binary protocol load generator (io_uring, pipelined — measures server ceiling)
binary-loadgen-uring: bench/binary_loadgen_uring.c
	@echo "  [CC/LD] $@"
	@$(CC) -O3 -Wall -std=gnu2x -pthread -o $@ $< -DHAVE_MSGPACK -luring -lmsgpackc
	@echo "✓ Built: $@"

# Compilation rules with nice output
$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	@echo "  [CC]  $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Include dependency files
-include $(ALL_OBJS:.o=.d)

# Rebuild target - clean and build everything
.PHONY: rebuild
rebuild: clean all

# Build info
.PHONY: info
info:
	@echo "=== Build Configuration ==="
	@echo "Compiler:    $(CC)"
	@echo "CFLAGS:      $(CFLAGS)"
	@echo "Build Dir:   $(BUILD_DIR)"
	@echo "Targets:     libreactor libreactor-server"
	@echo "=========================="

clean:
	rm -rf $(BUILD_DIR) libreactor libreactor-server *.a
	@if [ -f third_party/libdynamic/Makefile ]; then $(MAKE) -C third_party/libdynamic clean; fi
	@if [ -f third_party/libreactor/Makefile ]; then $(MAKE) -C third_party/libreactor clean; fi
	@if [ -f third_party/libclo/Makefile ]; then $(MAKE) -C third_party/libclo clean; fi
