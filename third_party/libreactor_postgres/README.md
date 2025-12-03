# libreactor_postgres

`libreactor_postgres` is an abstraction layer designed to integrate asynchronous PostgreSQL operations with the `libreactor` event-driven framework (version 3.0.0).

## Purpose

The primary goal of this library is to enable non-blocking, event-driven interaction with PostgreSQL databases within applications built on `libreactor`. This is achieved by:

*   **Asynchronous PostgreSQL Connectivity:** Utilizing `libpq`'s asynchronous API (`PQconnectStartParams`, `PQconnectPoll`) for non-blocking database connections.
*   **Event-Driven I/O:** Seamlessly integrating PostgreSQL socket events (read and write readiness) with the `libreactor` event loop using `reactor_add` and `reactor_modify`.
*   **Query Management:** Providing functions (`reactor_postgres_send`) to send queries and handle results asynchronously. It dispatches specific events such as `REACTOR_POSTGRES_EVENT_RESULT` upon receiving query results and `REACTOR_POSTGRES_EVENT_QUERY_DONE` when a query completes.
*   **State Management:** Maintaining and transitioning the internal state of the PostgreSQL connection (e.g., `CONNECTING`, `BUSY`, `AVAILABLE`, `ERROR`, `CLOSED`) based on `libpq`'s polling status and detected I/O events.
*   **Error Handling:** Dispatching `REACTOR_POSTGRES_EVENT_ERROR` to notify the application of connection or query failures.
*   **Resource Management:** Managing the lifecycle of PostgreSQL connections, including establishment, closure, and proper resource cleanup through reference counting (`reactor_postgres_hold`, `reactor_postgres_release`).

By leveraging `libreactor_postgres`, applications can achieve high-performance and scalable database interactions without blocking the main event loop, making it suitable for demanding network services and concurrent applications.

## Compatibility

This version is adapted for `libreactor` **3.0.0** and includes the following API changes:

*   Replaced `reactor_user` with `reactor_handler`
*   Updated callback signatures to use `reactor_event *`
*   Replaced `reactor_core_fd_*` functions with `reactor_add`/`reactor_modify`/`reactor_delete`
*   Fixed event mask translation (`DESCRIPTOR_READ` → `EPOLLIN`, `DESCRIPTOR_WRITE` → `EPOLLOUT`)

## Building and Installation

### Prerequisites

*   PostgreSQL development libraries (`libpq-dev`)
*   `libreactor` 3.0.0
*   GNU autotools (autoconf, automake, libtool)

### Build Steps

```bash
# Generate configure script
./autogen.sh

# Configure build
./configure --prefix=/usr/local

# Build library and examples
make

# Install (optional)
make install
```

## Examples

### query_lowlevel.c - Basic Connection Example

Demonstrates low-level PostgreSQL operations with direct connection management:

```bash
cd example
./query_lowlevel
```

Features:
* Direct PostgreSQL connection establishment
* INSERT and SELECT operations
* Real-time result processing
* Connection state management

### queries.c - Connection Pool Example

Shows advanced usage with connection pooling for concurrent queries:

```bash
cd example

# Select all data from test_data table
./queries 1 1 "SELECT id, message FROM test_data"

# Insert new data
./queries 1 1 "INSERT INTO test_data (message) VALUES ('new message')"

# Run 5 parallel SELECT queries using 2 connections
./queries 2 5 "SELECT * FROM test_data"
```

Features:
* Connection pool management
* Parallel query execution
* Real-time statistics display
* Automatic resource cleanup

## Testing

The examples have been tested with:

*   PostgreSQL 13.22
*   Real database operations (INSERT/SELECT)
*   Connection pooling with multiple parallel queries
*   Proper error handling and resource cleanup

### Test Database Setup

```sql
-- Create test user
CREATE USER pguser WITH PASSWORD 'qwertt111';

-- Create test table
CREATE TABLE test_data (
    id SERIAL PRIMARY KEY,
    message TEXT
);

-- Grant permissions
GRANT ALL PRIVILEGES ON TABLE test_data TO pguser;
GRANT USAGE, SELECT ON SEQUENCE test_data_id_seq TO pguser;
```

## API Reference

### Low-Level API

*   `reactor_postgres_open()` - Establish connection with parameters
*   `reactor_postgres_send()` - Send SQL query
*   `reactor_postgres_close()` - Close connection

### Client Pool API

*   `reactor_postgres_client_open()` - Open connection pool
*   `reactor_postgres_client_query_open()` - Execute query via pool
*   `reactor_postgres_client_close()` - Close connection pool

### Events

*   `REACTOR_POSTGRES_EVENT_READY` - Connection ready for queries
*   `REACTOR_POSTGRES_EVENT_RESULT` - Query result received
*   `REACTOR_POSTGRES_EVENT_QUERY_DONE` - Query completed
*   `REACTOR_POSTGRES_EVENT_ERROR` - Connection/query error
*   `REACTOR_POSTGRES_EVENT_CLOSE` - Connection closed

## License

This library follows the same license as the original `libreactor_postgres` project.
