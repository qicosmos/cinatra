# cinatra HttpArena benchmark

cinatra's C++20 coroutine server on standalone Asio and async_simple. The local
Dockerfile builds the framework and cases from the current checkout, using
GCC 14, Release optimization and LTO. The upstream HttpArena entry pins a complete
cinatra revision and explicitly enables x86-64-v3 for its benchmark host.
Local CMake and Docker builds leave that CPU requirement disabled by default.
The entry is marked `tuned`
because the typed payload uses request-local PMR storage, and small baseline
replies and TLS responses use the contiguous-buffer option. Compression retains
cinatra's response API defaults.

The entry requests the `flagship` maturity tier based on existing downstream use:
[yalantinglibs documents cinatra as its coro_http implementation](https://github.com/alibaba/yalantinglibs/blob/54f9c0642386e7860e8e11ada698c484dc562d7d/README.md#standalone-sublibraries),
and [Mooncake uses coro_http for its HTTP metadata service](https://github.com/kvcache-ai/Mooncake/blob/504f2ae4a307abe095d3baa75d40ed2661b6481b/mooncake-store/src/http_metadata_server.cpp).
The `tuned` mode describes the benchmark configuration independently of maturity.

Responses include Date, an eleven-byte Server value, Connection and Content-Type;
JSON also includes Vary: Accept-Encoding. These headers use cinatra's response
APIs. For local comparisons, align the other entries' headers, JSON field order
and gzip settings (level 6, window 15, memLevel 8) before measuring. Check both
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
- JSON uses `response.set_content(writer, encoding, accept_encoding)`, with
  `iguana::to_json` as its writer. The response API handles compression and its
  headers. The case selects `none` when Accept-Encoding is absent, as Arena
  requires; otherwise the header is passed to cinatra. There is no custom
  negotiation parser, gzip wrapper or response encoder in the case.
  No JSON, compressed bodies or full responses are cached.
- `/echo` borrows an ordinary request's body until the framework completes its
  reply. Chunked bodies are collected into response-owned storage.
- `/ws` uses cinatra's WebSocket handshake, reader and writer. Text and binary
  opcodes are preserved; control frames are handled by the framework.

The listeners are HTTP on `:8080` and optional HTTP/1.1 TLS on `:8081`, using
the mounted `/certs/server.crt` and `/certs/server.key`. The dataset defaults to
`/data/dataset.json`; `DATASET_PATH`, `TLS_CERT` and `TLS_KEY` can override these
paths for native checks.

Each listener uses cinatra's normal pool with one event loop per available
CPU. On Linux the count comes from `sched_getaffinity`, so a container cpuset
is respected, including non-contiguous masks. `CINATRA_THREADS` can override
the count for local comparisons. The framework creates its own SO_REUSEPORT
acceptors and retains its default TCP_NODELAY. Baseline replies and TLS JSON/echo
use a contiguous response buffer. Other plain HTTP replies retain the default
buffer sequence path. `CINATRA_TLS_MULTI_BUFFER=1` selects the buffer sequence
path for TLS JSON/echo comparisons.
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

From a checkout of HttpArena with the cinatra entry installed:

```sh
./scripts/validate.sh cinatra
./scripts/benchmark.sh cinatra baseline
./scripts/benchmark.sh cinatra json-comp
```

This entry subscribes to HTTP/1.1 and WebSocket profiles. Static-file,
database, template, HTTP/2, HTTP/3 and gRPC profiles are not included.
