FROM ubuntu:22.04 AS builder

# Установка зависимостей для сборки с GCC 12
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        wget \
        git \
        make \
        automake \
        libtool \
        file \
        gcc-12 \
        g++-12 \
        ca-certificates && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Настройка GCC 12 с поддержкой LTO
ENV CC=gcc-12 \
    CXX=g++-12 \
    AR=gcc-ar-12 \
    NM=gcc-nm-12 \
    RANLIB=gcc-ranlib-12

# Сборка libdynamic v2.3.0
RUN wget -q https://github.com/fredrikwidlund/libdynamic/releases/download/v2.3.0/libdynamic-2.3.0.tar.gz && \
    tar xzf libdynamic-2.3.0.tar.gz && \
    cd libdynamic-2.3.0 && \
    ./configure CFLAGS="-O3 -march=x86-64-v2 -mtune=generic -flto -DNDEBUG -fomit-frame-pointer" && \
    make -j$(nproc) && \
    make install && \
    ldconfig

# Сборка libclo v1.0.0
RUN wget -q https://github.com/fredrikwidlund/libclo/releases/download/v1.0.0/libclo-1.0.0.tar.gz && \
    tar xzf libclo-1.0.0.tar.gz && \
    cd libclo-1.0.0 && \
    sed -i '/#include <dynamic.h>/d' ./src/clo.c && \
    ./configure --prefix=/usr CFLAGS="-O3 -march=x86-64-v2 -mtune=generic -flto -DNDEBUG -fomit-frame-pointer" && \
    make -j$(nproc) && \
    make install && \
    ldconfig

# Сборка libreactor v2.0.0-alpha
RUN git clone --depth 1 --single-branch --branch release-2.0 \
        https://github.com/fredrikwidlund/libreactor libreactor-2 && \
    cd libreactor-2 && \
    git checkout v2.0.0-alpha && \
    ./autogen.sh && \
    ./configure CFLAGS="-O3 -march=x86-64-v2 -mtune=generic -flto -DNDEBUG -fomit-frame-pointer" && \
    make -j$(nproc) && \
    make install && \
    ldconfig

# Копирование исходников приложения
COPY src/ /build/src/
COPY Makefile /build/Makefile

# Сборка libreactor приложения
RUN make libreactor CFLAGS="-O3 -march=x86-64-v2 -mtune=generic -flto -DNDEBUG -fomit-frame-pointer"

# ============================================================================
# Финальный образ
# ============================================================================
FROM ubuntu:22.04

# Установка runtime зависимостей
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ethtool \
        iproute2 \
        procps \
        gosu \
        libgcc-s1 && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Создание непривилегированного пользователя
RUN groupadd -r -g 1000 libreactor && \
    useradd --no-log-init -r -u 1000 -g libreactor libreactor

WORKDIR /app

# Копирование бинарника и библиотек
COPY --from=builder /build/libreactor ./
COPY --from=builder /usr/local/lib/*.so* /usr/local/lib/
RUN ldconfig

# Скрипт запуска с оптимизациями
COPY <<'EOF' /app/entrypoint.sh
#!/bin/bash
set -e

echo "=== Applying performance optimizations ==="

# Network stack optimizations
if [ -w /proc/sys ]; then
    # Connection queue limits
    sysctl -w net.core.somaxconn=65535 2>/dev/null || true
    sysctl -w net.ipv4.tcp_max_syn_backlog=65535 2>/dev/null || true
    sysctl -w net.core.netdev_max_backlog=65535 2>/dev/null || true
    
    # ARP cache optimization
    sysctl -w net.ipv4.neigh.default.gc_thresh1=128 2>/dev/null || true
    sysctl -w net.ipv4.neigh.default.gc_interval=300 2>/dev/null || true
    
    # Port range and socket reuse
    sysctl -w net.ipv4.tcp_tw_reuse=1 2>/dev/null || true
    sysctl -w net.ipv4.ip_local_port_range="1024 65535" 2>/dev/null || true
    
    # Buffer sizes
    sysctl -w net.core.rmem_max=16777216 2>/dev/null || true
    sysctl -w net.core.wmem_max=16777216 2>/dev/null || true
    sysctl -w net.ipv4.tcp_rmem="4096 87380 16777216" 2>/dev/null || true
    sysctl -w net.ipv4.tcp_wmem="4096 65536 16777216" 2>/dev/null || true
    
    # VM memory management
    sysctl -w vm.swappiness=10 2>/dev/null || true
    sysctl -w vm.dirty_ratio=15 2>/dev/null || true
    sysctl -w vm.dirty_background_ratio=5 2>/dev/null || true
    
    # Scheduler tuning (conservative values)
    sysctl -w kernel.sched_min_granularity_ns=10000000 2>/dev/null || true
    sysctl -w kernel.sched_wakeup_granularity_ns=15000000 2>/dev/null || true
    
    echo "Kernel parameters applied"
else
    echo "Warning: Cannot write to /proc/sys - running without kernel optimizations"
    echo "Run container with --sysctl or --privileged for full performance"
fi

# NIC interrupt coalescing (if ethtool available and NIC exists)
if command -v ethtool >/dev/null 2>&1; then
    for iface in eth0 ens* enp*; do
        if [ -e "/sys/class/net/$iface" ]; then
            ethtool -C "$iface" adaptive-rx off 2>/dev/null || true
            ethtool -C "$iface" rx-usecs 300 2>/dev/null || true
            ethtool -C "$iface" tx-usecs 300 2>/dev/null || true
            echo "NIC $iface optimized"
            break
        fi
    done
fi

# CPU governor (if accessible)
for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    if [ -w "$gov" ]; then
        echo performance > "$gov" 2>/dev/null || true
    fi
done

echo "=== Starting libreactor application ==="
echo "User: $(id)"

# Запуск от имени непривилегированного пользователя
exec gosu libreactor ./libreactor "$@"
EOF

RUN chmod +x /app/entrypoint.sh && \
    chown -R libreactor:libreactor /app

EXPOSE 8080

# Запускаем от root для возможности применить sysctl
USER root
ENTRYPOINT ["/app/entrypoint.sh"]
CMD []
