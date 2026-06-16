# Libreactor - Extreme HTTP Performance Server

Optimized HTTP server based on libreactor with extreme performance.

Updated by 2025 year. 

Tested on Contabo 3vcpu 8gb ram, Debian11

Easy to 180k rps on localhost.

![f](/images/Screenshot_1.png)

![f](/images/Screenshot_2.png)

![f](/images/Screenshot_3.png)

## 🚀 Quick Start

### Native Build
```bash
# Compile with optimizations
./compile.sh

# Start the server
./run-optimized.sh

# Benchmark performance
wrk -t16 -c512 -d10s http://localhost:3984/plaintext

# Profile with AMD uProf
./profile-amd-uprof.sh

# Check status
./status.sh

# Stop the server
./stop.sh
```

### Docker (Maximum Performance)
```bash
# Build optimized container
docker build -f libreactor-server.dockerfile -t libreactor-server:latest .

# Run with extreme performance optimizations
docker run -d --rm \
  --network host \
  --security-opt seccomp=unconfined \
  --cap-add SYS_NICE \
  --cap-add SYS_RESOURCE \
  --cap-add NET_ADMIN \
  --init \
  libreactor-server:latest

# Benchmark (adjust IP as needed)
wrk -t8 -c512 -d10s http://localhost:3984/plaintext
```

## 📊 Benchmarking

```bash
# Quick test
wrk -t8 -c512 -d10s http://localhost:2342/plaintext

# Full benchmark
/var/www/benchmark-libreactor.sh
```

## 🐳 Docker Overhead Analysis

### Performance Impact
Our testing shows **55-65% performance degradation** in Docker vs native execution on KVM:

| Environment | Plaintext | JSON | Performance |
|-------------|-----------|------|-------------|
| **Native** | 40,142 RPS | 46,697 RPS | 100% baseline |
| **Docker** | 18,000 RPS | 16,000 RPS | ~35% of native |
| **Overhead** | 50% ↓ | 60% ↓ | **55-65% total** |

### Overhead Sources
1. **Network Stack**: NAT/bridge networking adds ~10-30% latency
2. **Syscall Filtering**: cgroups add ~5-15% CPU overhead
3. **Filesystem**: OverlayFS slower than native (~5-20%)
4. **Memory**: Copy-on-write increases usage (~5-10%)
5. **CPU Scheduling**: cgroups add ~2-5% scheduling overhead
6. **KVM Virtualization**: Additional virtualization layer amplifies all overheads

### Optimization Strategies
- ✅ **Host networking** (`--network host`) - eliminates network overhead
- ✅ **CPU pinning** (`--cpuset-cpus`) - reduces scheduling overhead
- ❌ **Remove CPU limits** - avoid `--cpus` flag
- ❌ **Use host volumes** - eliminate filesystem overhead
- ❌ **Minimal base images** - reduce memory footprint

### KVM Environment Impact
Our KVM virtualization significantly worsens Docker overhead:
- Limited CPU cores (3 vs unlimited)
- Memory ballooning effects
- Nested virtualization penalties
- Network virtualization layers

**On bare metal**: expect 10-30% overhead instead of 55-65%.

## 📦 Dependencies & Versions

- **libdynamic**: v2.3.0 (latest, high-performance data structures)
- **libclo**: v1.0.0 (JSON encoding, latest stable)
- **libreactor**: v2.0.0-alpha (latest release, event-driven framework)
- **GCC**: 12 (C17, better optimizations)
- **Ubuntu**: 22.04 LTS (kernel 5.15+, modern features)

## ⚡ Performance Optimizations

### Application Level Code
- **SO_REUSEPORT + BPF filter** - connection distribution across CPUs
- **Busy Poll (SO_BUSY_POLL)** - low latency for network operations
- **TCP_NODELAY** - disabling Nagle algorithm
- **SO_KEEPALIVE = 0** - disabling keepalive for performance
- **Multi-process architecture** - process per CPU with CPU pinning

### Compilation
- `-O3 -march=native -flto` - maximum optimizations
- `-DNDEBUG -fomit-frame-pointer -funroll-loops` - additional optimizations
- **GCC 12** - latest compiler with improved C features

### Docker Runtime Optimizations
- `--network host` - bypass Docker NAT for zero networking overhead
- `--security-opt seccomp=unconfined` - disable seccomp for syscall performance
- `--cap-add SYS_NICE/SYS_RESOURCE/NET_ADMIN` - allow kernel parameter tuning
- `--init` - proper signal handling and zombie process cleanup
- **In-container optimizations** (Extreme HTTP Performance Tuning):
  - Speculative execution mitigations disabled (nospectre_v1/v2, PTI off)
  - Syscall auditing disabled (auditd stopped)
  - Network stack tuned (16MB buffers, TCP optimizations)
  - Interrupt moderation (ethtool rx/tx-usecs)
  - CPU governor set to performance
  - Non-critical services stopped (rsyslog, postfix, chronyd, rpcbind)
  - Kernel scheduler optimized (granularity, dirty ratios, swappiness=0)

### System Level (Host)
- **Kernel parameters**: `nospectre_v1 nospectre_v2 pti=off mds=off tsx_async_abort=off`
- **Network sysctl**: 16MB buffers, busy poll, TCP optimizations
- **Nftables** instead of iptables (minimal overhead)
- **ethtool interrupt moderation**: static rx/tx-usecs for consistent latency

## 📁 Project Structure

```
/var/www/rads/
├── build/                     # Build directory (generated during compilation)
├── src/                       # Source code
│   ├── domain/                # HTTP domain logic
│   │   ├── http_response.c
│   │   └── http_server.c
│   ├── include/               # Header files
│   │   ├── compat/           # Compatibility headers
│   │   │   ├── dynamic.h
│   │   │   └── reactor.h
│   │   ├── domain/           # Domain headers
│   │   │   ├── http_response.h
│   │   │   └── http_server.h
│   │   ├── infrastructure/   # Infrastructure headers
│   │   │   └── server_infrastructure.h
│   │   └── platform/         # Platform headers
│   │       ├── log.h
│   │       ├── process.h
│   │       ├── signals.h
│   │       ├── socket.h
│   │       └── system.h
│   ├── infrastructure/        # Server infrastructure
│   │   └── server_infrastructure.c
│   ├── main/                  # Main application files
│   │   ├── libreactor-server.c
│   │   └── libreactor.c
│   └── platform/              # Platform utilities
│       ├── log.c
│       ├── process.c
│       ├── signals.c
│       ├── socket.c
│       └── system.c
├── compile.sh                 # Compilation with optimizations
├── run-optimized.sh          # Start with CPU pinning
├── stop.sh                   # Stop and cleanup
├── status.sh                 # Check status
├── Makefile                  # Alternative makefile
├── drop_changes.sh           # Git changes reset
├── fast_commits.sh           # Fast commits
├── switch_branch.sh          # Branch switching
├── git-init.sh               # Git repo initialization
├── libreactor-server.dockerfile # Dockerfile for server
├── libreactor.dockerfile     # Dockerfile for libreactor
└── README.md                 # This file
```

## 🎯 Performance

### Verified Results (Lima VM, native build, loopback)

Measured with `wrk` inside an Ubuntu 24.04 aarch64 Lima VM (4 vCPU, kernel 6.8.0-117-generic),
`libreactor-server` built natively with `-O3 -march=armv8-a -flto`, 4 worker processes
(one per CPU, `SO_REUSEPORT` + CBPF load balancing). The server listens on **port 3984**
(see `src/main/libreactor-server.c`).

| Endpoint | Threads / Connections | RPS | Avg latency | Throughput |
|----------|-----------------------|-----|-------------|------------|
| `/plaintext` | 4 / 256 | 475,089 | 542 µs | 60 MB/s |
| `/plaintext` | 4 / 512 | **542,612** | 0.99 ms | 68 MB/s |
| `/json` | 4 / 256 | 472,561 | 549 µs | 69 MB/s |

Peak throughput is **~540k RPS** on `/plaintext` at 512 connections.

Reproduce from inside the VM:

```bash
wrk -t4 -c256 -d20s http://127.0.0.1:3984/plaintext  # plaintext
wrk -t4 -c512 -d20s http://127.0.0.1:3984/plaintext  # plaintext (peak)
wrk -t4 -c256 -d20s http://127.0.0.1:3984/json       # json
```

> **Note on host-side numbers.** When benchmarking from the macOS host, the Lima
> SSH port-forward (`127.0.0.1:3984` → guest) adds tunnel overhead and caps results
> at ~126k RPS. Always benchmark on the loopback interface where the server runs for
> true throughput.
>
> **Note on routing.** Only `/plaintext` and `/json` are valid routes
> (see `http_server_parse_route` in `src/domain/http_server.c`). Any other path,
> including `/`, falls through to `ROUTE_UNKNOWN` and returns the 9-byte `"Not Found"`
> body with HTTP 200 — so benchmarking `/` measures the not-found path, not plaintext.

- **CPU spent on sendto()** (useful work)
- **Minimal locks and context switches**

### Historical Results (Docker, 3 CPUs, KVM)
- **18k+ req/sec** on plaintext (port 3984)
- **16k+ req/sec** on JSON responses (port 3984)

### Expected with Full Optimizations (4+ CPUs, bare metal)
Based on [Extreme HTTP Performance Tuning](https://talawah.io/blog/extreme-http-performance-tuning-one-point-two-million/):
- **1.2M req/sec** on plaintext (target from article)
- **Latency**: sub-millisecond p99
- **CPU utilization**: 100% on useful work
- **Scaling**: linear with CPU cores

## 🔧 API

### Endpoints
- `GET /plaintext` - returns "Hello, World!"
- `GET /json` - returns `{"message":"Hello, World!"}`

### Example Request
```bash
curl http://localhost:3984/plaintext
# Hello, World!

curl http://localhost:3984/json
# {"message":"Hello, World!"}
```

## 🛠️ Development

### Recompilation
```bash
make clean
make CFLAGS="-O3 -march=native -flto -DNDEBUG" libreactor-server
```

### Debug Build
```bash
make CFLAGS="-O0 -g" libreactor-server
```

## 📈 Monitoring

### CPU Profiling
```bash
perf record -F 99 -g -p $(pgrep libreactor-server | head -1) -o perf.data -- sleep 10
perf report -i perf.data
```

### System Calls
```bash
bpftrace -e 'tracepoint:syscalls:sys_enter_sendto { @[comm] = count(); } interval:s:1 { print(@); clear(@); }'
```

## 🔗 Links

- [Libreactor](https://github.com/fredrikwidlund/libreactor)
- [Extreme HTTP Performance Tuning](https://talawah.io/blog/extreme-http-performance-tuning-one-point-two-million/)
- [SO_REUSEPORT](https://lwn.net/Articles/542629/)
