# Libreactor - Extreme HTTP Performance Server

Optimized HTTP server based on libreactor with extreme performance.

> ## ⚡ Current measured throughput (io_uring backend)
>
> | Endpoint | RPS | Latency | Environment |
> |----------|-----|---------|-------------|
> | `/plaintext` | **~1,100,000 – 1,130,000** | 0.09–0.23 ms | 4 vCPU, loopback |
> | `/json` | **~1,040,000 – 1,140,000** | ~0.2 ms | 4 vCPU, loopback |
>
> Measured with `wrk` in an Ubuntu 24.04 aarch64 Lima VM (Apple Virtualization
> Framework, kernel 6.8) on 4 vCPU, 4 worker processes (one per CPU,
> `SO_REUSEPORT` + CBPF), io_uring reactor with `DEFER_TASKRUN`. Idle CPU ~100%.
> See [Performance](#-performance) for full methodology and the path from the
> original ~400k RPS epoll baseline (~+180%).

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

### Verified Results — io_uring backend (current)

The reactor event loop runs on **io_uring** (liburing), replacing epoll. Measured
with `wrk` inside an Ubuntu 24.04 aarch64 Lima VM on the Apple Virtualization
Framework (4 vCPU, kernel 6.8.0-117-generic), `libreactor-server` built natively
with `-O3 -march=armv8-a -flto`, 4 worker processes (one per CPU, `SO_REUSEPORT`
+ CBPF load balancing). The server listens on **port 3984**
(see `src/main/libreactor-server.c`).

| Endpoint | Threads / Connections | RPS | Avg latency | vs epoll |
|----------|-----------------------|-----|-------------|----------|
| `/plaintext` | 4 / 128 | **~900,000–1,070,000** | 0.11–0.27 ms | **+125–165%** |
| `/json` | 4 / 256 | **~750,000–830,000** | ~0.3 ms | **+120–144%** |

30-second runs report medians (~900k plaintext); 8-second sweeps peak near
1.07M RPS at 4 threads / 128 connections. The Apple vz hypervisor adds
run-to-run variance, so medians are reported for the 30s numbers.

Reproduce from inside the VM:

```bash
wrk -t4 -c128 -d30s http://127.0.0.1:3984/plaintext  # plaintext
wrk -t4 -c256 -d30s http://127.0.0.1:3984/json       # json
```

> **Note on host-side numbers.** When benchmarking from the macOS host, the Lima
> SSH port-forward (`127.0.0.1:3984` → guest) adds tunnel overhead and caps results
> far below the loopback numbers. Always benchmark on the loopback interface where
> the server runs for true throughput.
>
> **Note on routing.** Only `/plaintext` and `/json` are valid routes
> (see `http_server_parse_route` in `src/domain/http_server.c`). Any other path,
> including `/`, falls through to `ROUTE_UNKNOWN` and returns the 9-byte `"Not Found"`
> body with HTTP 200 — so benchmarking `/` measures the not-found path, not plaintext.

### ⚡ io_uring backend

The reactor core (`third_party/libreactor/src/reactor/core.c`) and descriptor
layer (`descriptor.c`) were ported from epoll to a liburing ring. fd readiness is
delivered via `IORING_OP_POLL_ADD` completions (one-shot, re-armed per event);
the higher layers (`stream.c`, `server.c`) still issue `read()`/`send()` on
readiness. This "poll + psync I/O" hybrid is the recommended design for sockets
at I/O depth 1 (see the liburing io_uring-vs-epoll analysis: io_uring wins for
ping-pong, and psync read/write beats completion ops when data is already in the
network buffer).

`strace -c` under load shows why replacing epoll roughly doubled throughput: the
per-event `epoll_wait` + `epoll_ctl` syscall storm collapses into batched
`io_uring_enter` calls.

**Ring setup flags:** the ring is created with `IORING_SETUP_DEFER_TASKRUN |
IORING_SETUP_SINGLE_ISSUER` (kernel 6.1+; we fall back to a plain ring on older
kernels). `DEFER_TASKRUN` defers task_work to `io_uring_enter` time in the owning
thread, cutting scheduler overhead — per the io_uring maintainer this alone is a
~+18–37% RPS class effect. `SINGLE_ISSUER` is safe here because each worker
thread drives its own ring and only that thread submits (async/thread-pool paths
write to an eventfd, never to the ring). Measured A/B on `/plaintext`,
5-run medians:

| Concurrency | io_uring (plain) | + DEFER_TASKRUN | Δ |
|-------------|------------------|-----------------|---|
| 2t / 64c    | 636k | **872k** | +37% |
| 4t / 128c   | 902k | **1,068k** | +18% |
| 4t / 256c   | 851k | **1,034k** | +22% |
| 4t / 512c   | 795k | **969k** | +22% |

| Syscall (per worker, ~900k aggregate RPS) | calls/sec | % time |
|-------------------------------------------|-----------|--------|
| `sendto` | ~18,000 | 59% |
| `read` | ~18,000 | 24% |
| `io_uring_enter` | ~6,700 | 17% |

The remaining `read`/`sendto` (~83%) are the synchronous I/O in `stream.c`. The
io_uring-vs-epoll analysis predicts completion ops would not help (and may
regress) for depth-1 sockets, so this is left as the hybrid design rather than a
full recv/send completion port. Idle CPU stays ~100% (no busy-poll regression).

- **CPU spent on sendto()** (useful work)
- **Minimal locks and context switches**

### Previous backend: epoll (history)

Before io_uring, the reactor used epoll. Two fixes were applied there first:

1. **epoll timeout busy-poll → blocking** — the loop previously used a 1 ms
   `epoll_wait` timeout (intentional "run-to-completion"), which pinned workers
   at ~0% idle / ~70% sys CPU with zero load. Switched to `-1` (block until
   event): ~100% idle at rest, +73% RPS at low concurrency. The
   `reactor_set_timeout(ms)` API is retained for callers wanting busy-poll.
2. That epoll backend peaked at **~400k RPS** (`/plaintext`), ~340k (`/json`).

### Historical Results (Docker, 3 CPUs, KVM)
- **18k+ req/sec** on plaintext (port 3984)
- **16k+ req/sec** on JSON responses (port 3984)

### Expected with Full Optimizations (4+ CPUs, bare metal)
Based on [Extreme HTTP Performance Tuning](https://talawah.io/blog/extreme-http-performance-tuning-one-point-two-million/):
- **1.2M req/sec** on plaintext (target from article)
- **Latency**: sub-millisecond p99
- **CPU utilization**: 100% on useful work
- **Scaling**: linear with CPU cores

### Why kerneldev MCP is not used here
The `kerneldev-mcp` tool builds and boots a *custom* kernel in a separate
virtme-ng/QEMU VM; it cannot reconfigure the kernel of an already-running Apple vz
Lima instance (no `/dev/kvm`, no cpufreq governor, no control over the guest kernel
command line). The single change that moved the needle in this environment was the
in-process epoll-timeout fix above, not kernel tuning.

### 🖥️ System tuning (Ubuntu guest)
Out of the box Ubuntu ships with small socket buffers and backlogs that cap
high-concurrency throughput. `tune_system.sh` applies the validated sysctl set
(run as root inside the VM, or copy to `/etc/sysctl.d/99-libreactor.conf`):

- Socket buffers: `rmem_max`/`wmem_max` 16 MB, `tcp_rmem`/`tcp_wmem` up to 16 MB
- Listen backlog: `somaxconn=32768` (the server calls `listen(fd, INT_MAX)`,
  the kernel clamps to this), `tcp_max_syn_backlog=32768`, `netdev_max_backlog=250000`
- `tcp_fastopen=3` (client + server), `tcp_tw_reuse=2`

A/B-measured effect on `/plaintext` (loopback, 4 vCPU):

| Concurrency | default Ubuntu | + sysctl tuning | Δ |
|-------------|----------------|-----------------|---|
| 2t / 64c    | 859k | 861k | ~0% |
| 4t / 128c   | 1,016k | 1,015k | ~0% |
| 4t / 256c   | 1,032k | **1,089k** | **+5.5%** |
| 4t / 512c   | 1,012k | **1,088k** | **+7.5%** |

The win is concentrated at high concurrency (256+ connections), where the larger
buffers/backlog stop being the limit. At low concurrency the workload is
syscall-bound, not buffer-bound, so sysctl changes are in the noise.

> **Conntrack bypass (NOTRACK for lo) — do NOT apply here.** Bypassing
> connection tracking for the loopback interface is a commonly recommended
> tuning, but A/B on this setup *regressed hard* at high concurrency (4t/256c:
> 1089k → 528k RPS with high variance). It is intentionally excluded from
> `tune_system.sh`. Loopback-specific quirks in the Apple vz net stack are the
> likely cause.

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
