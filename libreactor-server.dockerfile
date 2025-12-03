FROM ubuntu:22.04 AS builder

# Установка зависимостей для сборки
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        wget \
        git \
        make \
        automake \
        libtool \
        file \
        gcc \
        g++ \
        ca-certificates \
        build-essential && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Сборка libdynamic v2.3.0
RUN wget -q https://github.com/fredrikwidlund/libdynamic/releases/download/v2.3.0/libdynamic-2.3.0.tar.gz && \
    tar xzf libdynamic-2.3.0.tar.gz && \
    cd libdynamic-2.3.0 && \
    ./configure CFLAGS="-O3 -march=x86-64 -mtune=generic -flto -DNDEBUG -fomit-frame-pointer" && \
    make -j$(nproc) && \
    make install && \
    ldconfig

# Сборка libclo v1.0.0
RUN wget -q https://github.com/fredrikwidlund/libclo/releases/download/v1.0.0/libclo-1.0.0.tar.gz && \
    tar xzf libclo-1.0.0.tar.gz && \
    cd libclo-1.0.0 && \
    sed -i '/#include <dynamic.h>/d' ./src/clo.c && \
    ./configure --prefix=/usr CFLAGS="-O3 -march=x86-64 -mtune=generic -flto -DNDEBUG -fomit-frame-pointer" && \
    make -j$(nproc) && \
    make install && \
    ldconfig

# Сборка libreactor v2.0.0-alpha
RUN git clone https://github.com/fredrikwidlund/libreactor libreactor-2 && \
    cd libreactor-2 && \
    git checkout v2.0.0-alpha && \
    ./autogen.sh && \
    ./configure CFLAGS="-O3 -march=x86-64 -mtune=generic -flto -DNDEBUG -fomit-frame-pointer" && \
    make -j$(nproc) && \
    make install && \
    ldconfig

# Копирование исходников приложения
COPY src/ /build/src/
COPY Makefile /build/Makefile

# Сборка libreactor-server
RUN make libreactor-server CFLAGS="-O3 -march=x86-64 -mtune=generic -flto -DNDEBUG -fomit-frame-pointer"

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
        gosu && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Создание непривилегированного пользователя
RUN groupadd -r -g 1000 libreactor && \
    useradd --no-log-init -r -u 1000 -g libreactor libreactor

WORKDIR /app

# Копирование бинарника
COPY --from=builder /build/libreactor-server ./
RUN ldconfig

# Скрипт запуска с оптимизациями
RUN echo '#!/bin/bash\n\
set -e\n\
\n\
echo "=== Applying system optimizations ===\n\
\n\
# Network optimizations\n\
sysctl -w net.core.somaxconn=65535 2>/dev/null || true\n\
sysctl -w net.ipv4.tcp_max_syn_backlog=65535 2>/dev/null || true\n\
sysctl -w net.core.netdev_max_backlog=65535 2>/dev/null || true\n\
sysctl -w net.ipv4.tcp_tw_reuse=1 2>/dev/null || true\n\
sysctl -w net.ipv4.ip_local_port_range="1024 65535" 2>/dev/null || true\n\
\n\
# Memory buffers\n\
sysctl -w net.core.rmem_max=16777216 2>/dev/null || true\n\
sysctl -w net.core.wmem_max=16777216 2>/dev/null || true\n\
sysctl -w net.ipv4.tcp_rmem="4096 87380 16777216" 2>/dev/null || true\n\
sysctl -w net.ipv4.tcp_wmem="4096 65536 16777216" 2>/dev/null || true\n\
\n\
# VM optimizations\n\
sysctl -w vm.swappiness=10 2>/dev/null || true\n\
sysctl -w vm.dirty_ratio=15 2>/dev/null || true\n\
sysctl -w vm.dirty_background_ratio=5 2>/dev/null || true\n\
\n\
# Kernel scheduler optimizations (Chapter 9)\n\
sysctl -w kernel.sched_min_granularity_ns=10000000 2>/dev/null || true\n\
sysctl -w kernel.sched_wakeup_granularity_ns=15000000 2>/dev/null || true\n\
\n\
# CPU governor (if available)\n\
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null; do\n\
    [ -f "$cpu" ] && echo performance > "$cpu" || true\n\
done\n\
\n\
# Stop non-critical services (Chapter 4)\n\
systemctl stop auditd 2>/dev/null || true\n\
systemctl stop chronyd 2>/dev/null || true\n\
systemctl stop rsyslog 2>/dev/null || true\n\
\n\
# Interrupt moderation (Chapter 6)\n\
ethtool -C eth0 adaptive-rx off 2>/dev/null || true\n\
ethtool -C eth0 rx-usecs 300 2>/dev/null || true\n\
ethtool -C eth0 tx-usecs 300 2>/dev/null || true\n\
\n\
echo "=== Starting libreactor-server ===\n\
\n\
# Запуск от имени непривилегированного пользователя\n\
exec gosu libreactor ./libreactor-server "$@"\n\
' > /app/entrypoint.sh

RUN chmod +x /app/entrypoint.sh && \
    chown -R libreactor:libreactor /app

EXPOSE 3984

# По умолчанию запускаем от root для sysctl, но приложение от libreactor
USER root
ENTRYPOINT ["/app/entrypoint.sh"]
