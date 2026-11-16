# cinatra HttpArena benchmark

cinatra's C++20 coroutine server on standalone Asio and async_simple. The local
Dockerfile builds the framework and cases from the current checkout, using
GCC 14, Release optimization and LTO. The upstream HttpArena entry pins a complete
cinatra revision and explicitly enables x86-64-v3 for its benchmark host.
Local CMake and Docker builds leave that CPU requirement disabled by default.
The entry is marked `tuned`
because the typed payload uses request-local PMR storage, and small baseline
replies and TLS responses use the contiguous-buffer option. Gzip uses level 3,
matching dusty's native benchmark configuration.

The entry requests the `flagship` maturity tier based on existing downstream use:
[yalantinglibs documents cinatra as its coro_http implementation](https://github.com/alibaba/yalantinglibs/blob/54f9c0642386e7860e8e11ada698c484dc562d7d/README.md#standalone-sublibraries),
and [Mooncake uses coro_http for its HTTP metadata service](https://github.com/kvcache-ai/Mooncake/blob/504f2ae4a307abe095d3baa75d40ed2661b6481b/mooncake-store/src/http_metadata_server.cpp).
The `tuned` mode describes the benchmark configuration independently of maturity.

The server keeps the automatic Date header and disables only Server with
`set_server_header(false)`, including WebSocket handshakes. This matches
[dusty's native Date behavior](https://github.com/lalinsky/dusty/commit/ba2236da78ff120618550703e0d2fa59ba78b9ba),
included in v0.6.0. HttpArena's historical dusty v0.5.0 entry omits Date, so its
recorded response lengths describe a different configuration. Ordinary
HTTP replies carry no extra Connection header. Content-Type matches dusty's
native values, including `charset=UTF-8` for text and JSON; JSON also includes
Vary: Accept-Encoding. Date-header checks use dusty's upstream implementation
without added header overlays. Check both
body bytes and complete HTTP response lengths, including TLS responses after
decryption; equal decoded JSON alone does not establish equal transfer sizes.

## Implementation

- Routes and path/query parameters use cinatra's router and request APIs.
- `/baseline11` sums all query values; POST also consumes and adds the body.
  Chunked requests use cinatra's chunk reader. Both methods use a contiguous
  response buffer for their small replies.
- `/pipeline` returns `ok` through cinatra's response API. Only this constant
  endpoint borrows a static string literal.
- `/delay/{ms}` uses `coro_io::sleep_for` on the connection's executor.
- `/json/{count}?m=N` selects the first `count` dataset items and computes every
  `total = price * quantity * N` on each request. String views and request-local
  PMR vectors avoid copying source strings or sharing mutable state. Every
  field is serialized by iguana on each request, with normal escaping enabled.
- JSON uses `response.set_content(writer, encoding, accept_encoding, 3)`, with
  `iguana::to_json` as its writer. The response API handles compression and its
  headers. The case selects `none` when Accept-Encoding is absent, as Arena
  requires; otherwise the header is passed to cinatra. There is no custom
  negotiation parser, gzip wrapper or response encoder in the case.
  No JSON, compressed bodies or full responses are cached.
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
No custom CPU pinning, socket implementation, timeouts or replacement allocator
are added.
SIGINT/SIGTERM stop both listeners before the dataset is destroyed.

The 16 KiB PMR buffer is temporary typed application data, not an HTTP buffer or a
response cache; it falls back to the standard allocator if a dataset requires
more space. All fields, JSON escaping, arithmetic and compression run afresh.

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

The local case also implements `static-tls` on the existing TLS listener.
The upstream Arena subscription can be updated after local review. Database,
template, HTTP/2, HTTP/3 and gRPC profiles are not included.
