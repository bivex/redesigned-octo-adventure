# Benchmarks for libreactor_postgres

This directory contains performance benchmarks for testing latency and throughput of database operations using reactor_postgres.

## Available Benchmarks

### benchmark_writes
Tests write performance (INSERT operations) with concurrent queries.

**Usage:**
```bash
./bench/benchmark_writes <num_queries> <concurrent>
```

**Parameters:**
- `num_queries`: Total number of INSERT queries to execute
- `concurrent`: Number of concurrent queries to maintain

**Example:**
```bash
# Test 10,000 INSERT operations with 50 concurrent queries
./bench/benchmark_writes 10000 50

# Test 1,000 INSERT operations with 10 concurrent queries
./bench/benchmark_writes 1000 10
```

**What it measures:**
- Queries per second (QPS)
- Average, minimum, and maximum latency
- Error rate
- Progress reporting during execution

**Database setup:**
The benchmark automatically creates a `benchmark_test` table with the following schema:
```sql
CREATE TABLE benchmark_test (
  id SERIAL PRIMARY KEY,
  user_id INTEGER,
  ad_id INTEGER,
  action TEXT,
  timestamp TIMESTAMP DEFAULT NOW()
);
```

This simulates typical ads system transition logging where you track user interactions with advertisements.

## Running Benchmarks

Make sure PostgreSQL is running and accessible with the default connection parameters:
- Host: 127.0.0.1
- User: pguser
- Password: qwertt111
- Database: postgres
- Port: 5432

## Alternative Tools

For comparison, you can also use:

### pgbench (built-in PostgreSQL tool)
```bash
# Initialize test database
pgbench -i -s 10 postgres

# Run write-heavy benchmark
pgbench -c 50 -j 4 -T 60 -S -M prepared postgres
```

### sysbench (if installed)
```bash
# Install sysbench with PostgreSQL support
apt-get install sysbench

# Run PostgreSQL benchmark
sysbench --db-driver=pgsql --pgsql-host=127.0.0.1 --pgsql-user=pguser \
         --pgsql-password=qwertt111 --pgsql-db=postgres \
         --table-size=1000000 --threads=50 --time=60 \
         oltp_write_only --report-interval=10 run
```</contents>
</xai:function_call">Теперь давайте создадим простой скрипт для запуска различных тестов. 

```bash
<xai:function_call name="write">
<parameter name="file_path">/var/www/rads/third_party/libreactor_postgres/bench/run_benchmarks.sh
