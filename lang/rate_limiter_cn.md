# 限流器 (Rate Limiter)

基于令牌桶的限流器，用于限制请求频率。

## 构造函数

```cpp
rate_limiter(double rate, int burst)
```

| 参数 | 说明 |
|------|------|
| `rate` | Token 产生速率（个/秒）|
| `burst` | 桶最大容量 |

创建时桶初始化为满状态。

---

## 同步接口

### `bool allow()`
检查是否能立即获取 1 个 token。
- 允许返回 `true`，token 不足返回 `false`。

### `bool allow_n_tokens(int n)`
检查是否能立即获取 n 个 token。
- 允许返回 `true`，token 不足返回 `false`。
- 适用场景：非阻塞限流（token 不足时拒绝请求）。

### `void wait()`
阻塞当前线程，直到获取到 1 个 token。
- 如 token 不足会自动等待所需时间。

### `void wait_n_tokens(int n)`
阻塞当前线程，直到获取到 n 个 token。
- 如 token 不足会自动等待所需时间。

---

## 异步接口

### `awaitable wait_async()`
返回协程 awaitable 对象，用于异步等待 1 个 token。
- 需要在协程中使用 `co_await` 调用。

### `awaitable wait_async_n(int n)`
返回协程 awaitable 对象，用于异步等待 n 个 token。
- 需要在协程中使用 `co_await` 调用。

---

## 示例

```cpp
#include "cinatra/rate_limiter.hpp"
#include "async_simple/coro/Lazy.h"

using namespace cinatra;

// 同步使用
void sync_example() {
    rate_limiter limiter(10.0, 5);  // 每秒10个token，最大容量5

    // 非阻塞检查
    if (limiter.allow()) {
        // 处理请求
    }

    // 阻塞等待
    limiter.wait_n_tokens(3);  // 等待直到有3个token可用
}

// 异步使用
async_simple::coro::Lazy<void> async_example() {
    rate_limiter limiter(10.0, 5);

    co_await limiter.wait_async_n(3);  // 异步等待3个token
}
```
