# Rate Limiter

Token bucket rate limiter for limiting request frequency.

## Constructor

```cpp
rate_limiter(double rate, int burst)
```

| Parameter | Description |
|-----------|-------------|
| `rate`    | Token generation rate (tokens per second) |
| `burst`   | Maximum bucket capacity |

The bucket is initialized as full at creation time.

---

## Synchronous Interface

### `bool allow()`
Check if 1 token can be obtained immediately.
- Returns `true` if allowed, `false` if insufficient tokens.

### `bool allow_n_tokens(int n)`
Check if n tokens can be obtained immediately.
- Returns `true` if allowed, `false` if insufficient tokens.
- Use case: non-blocking rate limiting (reject requests when insufficient).

### `void wait()`
Block current thread until 1 token is obtained.
- Automatically waits the required time if tokens are insufficient.

### `void wait_n_tokens(int n)`
Block current thread until n tokens are obtained.
- Automatically waits the required time if tokens are insufficient.

---

## Asynchronous Interface

### `awaitable wait_async()`
Return a coroutine awaitable for asynchronously waiting for 1 token.
- Must be used with `co_await` in a coroutine.

### `awaitable wait_async_n(int n)`
Return a coroutine awaitable for asynchronously waiting for n tokens.
- Must be used with `co_await` in a coroutine.

---

## Example

```cpp
#include "cinatra/rate_limiter.hpp"
#include "async_simple/coro/Lazy.h"

using namespace cinatra;

// Synchronous usage
void sync_example() {
    rate_limiter limiter(10.0, 5);  // 10 tokens/sec, max 5

    // Non-blocking check
    if (limiter.allow()) {
        // Process request
    }

    // Blocking wait
    limiter.wait_n_tokens(3);  // Wait until 3 tokens are available
}

// Asynchronous usage
async_simple::coro::Lazy<void> async_example() {
    rate_limiter limiter(10.0, 5);

    co_await limiter.wait_async_n(3);  // Async wait for 3 tokens
}
```
