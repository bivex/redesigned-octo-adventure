#!/bin/bash
# Benchmark runner script for libreactor_postgres

set -e

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$BENCH_DIR")"

echo "=== Reactor Postgres Benchmarks ==="
echo "Project: $PROJECT_DIR"
echo "Benchmarks: $BENCH_DIR"
echo ""

# Check if PostgreSQL is accessible
echo "Checking PostgreSQL connection..."
if ! pg_isready -h 127.0.0.1 -p 5432 -U pguser >/dev/null 2>&1; then
    echo "❌ PostgreSQL not accessible. Please ensure it's running."
    echo "Expected connection: host=127.0.0.1 port=5432 user=pguser"
    exit 1
fi
echo "✓ PostgreSQL is accessible"
echo ""

# Build benchmarks if needed
echo "Building benchmarks..."
cd "$PROJECT_DIR"
make bench/benchmark_writes >/dev/null 2>&1
echo "✓ Benchmarks built"
echo ""

# Function to run a benchmark test
run_test() {
    local queries="$1"
    local concurrent="$2"
    local description="$3"

    echo "🧪 $description"
    echo "   Queries: $queries, Concurrent: $concurrent"
    echo "   Command: ./bench/benchmark_writes $queries $concurrent"
    echo ""

    ./bench/benchmark_writes "$queries" "$concurrent"

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""
}

# Test scenarios
echo "Running benchmark test scenarios..."
echo ""

# Small test for validation
run_test 100 5 "Small validation test (100 queries, 5 concurrent)"

# Medium load test
run_test 1000 10 "Medium load test (1000 queries, 10 concurrent)"

# High concurrency test
run_test 5000 50 "High concurrency test (5000 queries, 50 concurrent)"

# High throughput test
run_test 10000 20 "High throughput test (10000 queries, 20 concurrent)"

echo "✅ All benchmark tests completed!"
echo ""
echo "📊 Results summary:"
echo "   - Small validation: Basic functionality check"
echo "   - Medium load: Typical application load"
echo "   - High concurrency: Stress test concurrent operations"
echo "   - High throughput: Maximum sustainable throughput"
echo ""
echo "💡 Tips:"
echo "   - Monitor PostgreSQL logs for any issues"
echo "   - Check system resources (CPU, memory, disk I/O)"
echo "   - Compare with pgbench for reference numbers"
echo "   - Adjust concurrent connections based on your system"</contents>
</xai:function_call">Теперь сделаем скрипт исполняемым. 

```bash
<xai:function_call name="run_terminal_cmd">
<parameter name="command">chmod +x /var/www/rads/third_party/libreactor_postgres/bench/run_benchmarks.sh
