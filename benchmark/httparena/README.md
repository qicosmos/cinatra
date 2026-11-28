# cinatra HttpArena benchmark

cinatra's C++20 coroutine server on standalone Asio and async_simple. The local
Dockerfile builds the framework and cases from the current checkout, using
GCC 14, Release optimization and LTO. The upstream HttpArena entry pins a complete
cinatra revision and explicitly enables x86-64-v3 for its benchmark host.
Local CMake and Docker builds leave that CPU requirement disabled by default.
The entry is marked `tuned`. It uses contiguous buffers for small baseline
replies and by default for TLS responses, and gzip level 1 for compressed JSON.

The entry requests the `flagship` maturity tier based on existing downstream use:
[yalantinglibs documents cinatra as its coro_http implementation](https://github.com/alibaba/yalantinglibs/blob/54f9c0642386e7860e8e11ada698c484dc562d7d/README.md#standalone-sublibraries),
and [Mooncake uses coro_http for its HTTP metadata service](https://github.com/kvcache-ai/Mooncake/blob/504f2ae4a307abe095d3baa75d40ed2661b6481b/mooncake-store/src/http_metadata_server.cpp).
The `tuned` mode describes the benchmark configuration independently of maturity.

The server keeps the automatic Date header and disables only Server with
`set_server_header(false)`, including WebSocket handshakes. This matches
[dusty's native Date behavior](https://github.com/lalinsky/dusty/commit/ba2236da78ff120618550703e0d2fa59ba78b9ba),
included in v0.6.0. HttpArena's historical dusty v0.5.0 entry omits Date, so its
recorded response lengths describe a different configuration. Ordinary
HTTP replies carry no extra Connection header. Text uses
`text/plain; charset=UTF-8`. JSON uses `application/json` and includes
`Vary: Accept-Encoding`. Date-header checks use dusty's upstream implementation
without added header overlays. Check both
body bytes and complete HTTP response lengths, including TLS responses after
decryption; equal decoded JSON alone does not establish equal transfer sizes.

## Implementation

- Routes and path/query parameters use cinatra's router and request APIs.
- `/baseline11` sums all query values; POST also consumes and adds the body.
  Chunked requests use cinatra's chunk reader. Both methods use a contiguous
  response buffer for their small replies. `CON-1M` and `CON-1M-TLS` use
  the existing GET route on their respective plain and TLS listeners.
- `/pipeline` returns `ok` through cinatra's response API. Only this constant
  endpoint borrows a static string literal.
- `/delay/{ms}` uses `coro_io::sleep_for` on the connection's executor.
- `/json/{count}?m=N` selects the first `count` dataset items and computes every
  `total = price * quantity * N` on each request. The dataset owns the decoded
  strings and each row refers to them through views. Iguana serializes every
  field through `to_json_buffered` with normal escaping.
  The case selects gzip when allowed by `Accept-Encoding` and uses
  `response.set_buffered_content` for serialization and compression.
- `/echo` borrows an ordinary request's body until the framework completes its
  reply. Chunked bodies are collected into response-owned storage.
- `/static/{file}` reads the mounted `/data/static` directory (`STATIC_DIR`
  overrides it locally) through `coro_io::coro_file` and moves the bytes into
  cinatra's response. It selects on-disk `.br`/`.gz` variants by the client's
  encoding preference, retaining the original MIME type. Files are read per
  request, including replacements of the same size; no entry-level cache or
  compression implementation is added. With `HTTPARENA_FILE_IO_URING=ON`, the
  file uses native io_uring on the connection's executor, so completion and
  subsequent socket I/O stay on that executor. The portable build retains the
  framework's file thread-pool fallback. TLS static replies use the native
  contiguous response buffer; `CINATRA_TLS_MULTI_BUFFER=1` selects the buffer
  sequence path for local comparisons.
- `/ws` uses cinatra's WebSocket handshake, reader and writer. Text and binary
  opcodes are preserved; control frames are handled by the framework.
  `set_websocket_auto_flush(false)` enables native bounded write batching.
  The reader drains prefetched frames and flushes pending output before waiting
  for more input, so a single-message client never waits for a fixed batch size.
  Headers and payloads are copied into connection-owned storage, capped at
  64 KiB of queued frame bytes; larger frames and control frames flush directly.
  Applications that send without reading can await `flush_websocket()` explicitly.
  The default writer still sends immediately. Reads, writes and flushes must be
  serialized on the connection's executor, and a read result remains valid until
  the next read.

The listeners are HTTP on `:8080` and optional HTTP/1.1 TLS on `:8081`, using
the mounted `/certs/server.crt` and `/certs/server.key`. The dataset defaults to
`/data/dataset.json`; `DATASET_PATH`, `TLS_CERT` and `TLS_KEY` can override these
paths for native checks.

Each listener uses cinatra's normal pool with one event loop per available
CPU. On Linux the count comes from `sched_getaffinity`, so a container cpuset
is respected, including non-contiguous masks. `CINATRA_THREADS` can override
the count for local comparisons. The framework creates its own SO_REUSEPORT
acceptors and retains its default TCP_NODELAY. Baseline replies and TLS JSON,
echo and static-file replies use a contiguous response buffer. Other plain HTTP
replies retain the default buffer sequence path. `CINATRA_TLS_MULTI_BUFFER=1`
selects the buffer sequence path for TLS JSON/echo/static-file comparisons.
The socket BIO and Asio TLS backends use the same cinatra listener and Asio
executor.
SIGINT/SIGTERM stop both listeners before the dataset is destroyed.

## Local checks

The implementation lives here in cinatra. The HttpArena entry should fetch a
fixed cinatra commit and build this directory instead of copying the cases.

This target always enables SSL and GZIP because the subscribed profiles require
both. OpenSSL and zlib are required even when the parent project is configured
with `CINATRA_ENABLE_SSL=OFF`. An SSL=OFF label in the parent CI matrix therefore
does not describe this benchmark target's TLS and gzip coverage.

Build through the main project (disabled by default):

```sh
cmake -S . -B build-httparena -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_HTTPARENA=ON -DBUILD_UNIT_TESTS=OFF \
  -DBUILD_EXAMPLES=OFF -DBUILD_PRESS_TOOL=OFF \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
cmake --build build-httparena --target httparena_server -j
DATASET_PATH=/path/to/HttpArena/data/dataset.json \
  ./build-httparena/bin/httparena_server
```

The directory also supports a standalone build:

```sh
cmake -S benchmark/httparena -B build-httparena -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
cmake --build build-httparena -j
```

Build the container from the repository root, including the iguana submodule:

```sh
git submodule update --init iguana
docker build -f benchmark/httparena/Dockerfile -t httparena-cinatra .
```

For a CPU that supports x86-64-v3, opt in with
`-DHTTPARENA_X86_64_V3=ON` in CMake or
`--build-arg HTTPARENA_X86_64_V3=ON` in Docker. A binary built with this option
requires that CPU feature level at runtime.

The Linux Docker build enables `HTTPARENA_FILE_IO_URING` by default and installs
liburing. It requires a kernel supporting io_uring (5.10 or later) and a container
policy permitting it; the Arena harness uses `--security-opt seccomp=unconfined`.
Use `--build-arg HTTPARENA_FILE_IO_URING=OFF` for the portable file-I/O fallback.
Standalone CMake leaves this option off; enable it with
`-DHTTPARENA_FILE_IO_URING=ON` after installing the liburing development package.

From a checkout of HttpArena with the cinatra entry installed:

```sh
./scripts/validate.sh cinatra
./scripts/benchmark.sh cinatra baseline
./scripts/benchmark.sh cinatra json-comp
```

On a benchmark host provisioned for one million simultaneous sockets and
250,000 requests per second, run the two full-scale profiles.

```sh
./scripts/benchmark.sh cinatra connections-1m
./scripts/benchmark.sh cinatra connections-1m-tls
```

### Gzip and TLS buffering options

`HTTPARENA_LIBDEFLATE=ON` links the optional libdeflate backend. Serializers still
run for every request. A complete payload buffered below 16 KiB is compressed
synchronously with libdeflate; larger streaming payloads retain the zlib path.
Only a thread-local compressor is reused, and no input, compressed response or
application dictionary is cached. Compressed bytes and sizes can differ from
zlib, so performance comparisons must include actual response sizes.

`CINATRA_GZIP_LEVEL` configures the handler's native gzip level in the range
-1 through 9. Both standalone builds and the Docker image default to 1.
Compression-ratio weighting must be included when comparing these settings.

Each TLS listener loads its credentials once when it starts and shares that
initialized context across its connections. Recreate the server with the new
configuration to load replaced credential files.

The root CMake option `CINATRA_ENABLE_LIBDEFLATE` enables this compression
backend for applications and tests and defaults to OFF. The Linux workflow
checks Release, ASan and TSan separately, including TLS on a shared executor.

`HTTPARENA_TLS_BUFFERING` enables a 16 KiB TLS request read window and allows
the handler to borrow a body that arrived in that window. Its standalone default
follows `HTTPARENA_FILE_IO_URING`. The benchmark Docker image leaves the window
disabled to reduce memory per held connection. Root project builds can enable
`CINATRA_TLS_BUFFERING` for all targets. The header-size limit is independent of
read capacity. The connection owns the borrowed body until the handler and
response finish.

On Linux with OpenSSL 3.0 or later, `HTTPARENA_OPENSSL_SOCKET_BIO=ON`
lets the benchmark TLS server read and write through a socket BIO, with
Asio handling socket readiness. The benchmark Docker image enables this
option. Standalone CMake builds default to Asio TLS. The Docker
`HTTPARENA_OPENSSL_SOCKET_BIO` and `HTTPARENA_TLS_BUFFERING` build arguments
can select either backend and enable the larger read window.

The JSON dataset owns its decoded strings. The HttpArena response writer
computes each item's total as it writes the item with Iguana.
`iguana::to_json_buffered` writes the JSON for both response modes.
Cinatra reuses response string capacity for plain JSON. For gzip it serializes
into a temporary string and passes the complete bytes to the gzip writer in
one append.
