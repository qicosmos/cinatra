# 流量控制

应用服务器需要实现限制流量过大或过载自我保护等功能时，就需要流量控制的功能。流量控制不仅仅适用于服务端同时也适用于客户端，客户端也可以使用当前实现的令牌桶算法来进行发送速率的限制。

# 令牌桶设计

令牌桶算法核心在于模拟一个桶。

1. 当需要执行某种操作的时候需要从桶中获取令牌才运行执行，当此时桶中令牌数小于1的时候不允许该操作。
2. 该桶会以恒定的速率源源不断地产生令牌。

下图是令牌桶的图示:

![](https://media.geeksforgeeks.org/wp-content/uploads/20240116162804/Blank-diagram-(7).png)

令牌桶的算法逻辑如下:

生成令牌的逻辑: 每 1/r 秒就会向桶中添加一个令牌。

消耗令牌的逻辑:

1. 桶最多可以容纳b个令牌。如果桶满时令牌到达，则将其丢弃。
2. 当一个n占比的行为到达时，如果桶中至少有n个令牌，则从桶中删除n个令牌，并运行该行为。
3. 如果可用令牌少于 n 个，则不会从桶中删除任何令牌，并且该行为被视为不合格。

下面是消耗令牌的伪代码:

```c
when(b):
  Bt = now()
  Wb = (Bt - At) * r
  W = min(W + Wb, C)
  if (W > 1.0):
    W--
    return true
  else:
    return false
```

以服务器为例，当请求到来的时候。

Bt为当前时间，At为上一次令牌消耗时间(或者为初始化令牌桶时的时间)，Wb为应该生成的令牌数。C为令牌桶最大令牌数。

核心代码为`min(W + Wb, C)`，当前可用令牌数取生成的令牌加上此时桶中的令牌以及令牌桶容量两者中的最小值。

这里有一个好处那就是当前令牌的计算是线性的，只于时间有关。

# cinatra中的令牌桶算法设计

为了适用更多场景，采用原子操作来实现。

提供了4个API:

```cpp
/*
 * 尝试消耗一定数量的token。首先根据自上次尝试消耗令牌以来经过的时间将令牌添加到存储桶中。
 * 注意：尝试消耗比桶令牌最大大小更多的令牌总是会失败。
 * @param to_consume 要消耗的令牌数量。
 * @param now_in_seconds 当前时间（以秒为单位）。应从此令牌桶构造函数中指定的 now_in_seconds 单调递增。
 * @return 如果速率限制检查通过则返回 true，否则返回 false。
*/
bool consume(double to_consume, double now_in_seconds = default_clock_now())
/*
 * 与consume类似，但总是消耗一定数量的token。如果桶包含足够的令牌则消耗 to_consume 令牌。否则桶会被清空。
*/
bool consume_or_drain(double to_consume, double now_in_seconds = default_clock_now())
/*
 * 预留令牌同时返回令牌数量到达当前令牌的等待时间(但是立即返回)，以便预留与存储桶配置兼容。
*/
bool consume_with_borrow_nonblocking(double to_consume, double now_in_seconds = default_clock_now())
/*
 * 储备令牌。如果需要的大于当前桶中存储的会阻塞，直到令牌数量得到满足。
*/
bool consume_with_borrow_and_wait(double to_consume, double now_in_seconds = default_clock_now())
```

具体实现和上述伪代码逻辑一致。

# cinatra全局令牌桶逻辑

在coro_http_server.hpp中的`coro_http_server`类中添加令牌桶成员变量:

```cpp
  enum limiter_type limiter_type_ = limiter_type::disable;
  // 100 tokens are generated per second
  // and the maximum number of token buckets is 100
  token_bucket token_bucket_ = token_bucket{100, 100};
```

一个是是否需要速率限制的标记，一个是全局令牌桶。

目前有三个类型:

```cpp
enum class limiter_type {
  disable,
  global,
  perip,
};
```

默认为disable不开启，global为开启全局令牌桶，perip为开启每IP请求限制。


下面是开关令牌桶功能的API:

```cpp
  void set_rate_limiter(enum limiter_type type, int gen_rate = 0,
                        int burst_size = 0) {
    limiter_type_ = type;
    if (limiter_type_ == limiter_type::global) {
      token_bucket_.reset(gen_rate, burst_size);
    }
    else if (limiter_type_ == limiter_type::perip) {
      per_rate_ = gen_rate;
      per_burst_size_ = burst_size;
    }
    else {
      per_rate_ = 0.0;
      per_burst_size_ = 0.0;
    }
  }
```

最后将令牌桶逻辑嵌入到`accept()`函数中,将之前的`start_one(conn).via(&conn->get_executor()).detach()`改为如下:

```cpp
switch (limiter_type_) {
  case limiter_type::disable: {
    start_one(conn).via(&conn->get_executor()).detach();
    break;
  }
  case limiter_type::global: {
    if (token_bucket_.consume(1)) {
      // there are enough tokens to allow request.
      start_one(conn).via(&conn->get_executor()).detach();
    }
    else {
      conn->close();
    }
    break;
  }
  case limiter_type::perip: {
    // per ip功能后文解释,这里省略
    break;
  }
  default: {
    start_one(conn).via(&conn->get_executor()).detach();
    break;
  }
}
```

当没有设置流控时走原来的处理逻辑。当走流控时，每次请求来的时候消耗一个令牌，若桶中没有令牌了则直接关闭连接，此时客户端会收到404错误。

这就是全局令牌桶的逻辑。

# cinatra每ip请求限制功能逻辑

新增三个变量，per_rate_记录每个ip的令牌生成速率(秒为单位)，per_burst_size_为每个ip的令牌桶最大令牌数，一个hash表其中key为ip地址，value为该ip地址所拥有的令牌桶。

```cpp
  double per_rate_ = 0.0;
  double per_burst_size_ = 0.0;
  std::unordered_map<std::string, std::shared_ptr<token_bucket>> req_limiter_;
```

逻辑是当客户端连接到cinatra服务端的时候，如果第一次到则生成该ip令牌桶，将该ip作为key，生成的令牌桶作为value插入到hash表中。

请求时需要根据客户端ip从hash表中取出其令牌桶，当该ip对应的令牌桶中令牌数大于1的时候运行请求，否则禁止其访问。此时客户端会收到404错误。

具体逻辑如下:

```cpp
if (req_limiter_.empty() ||
    req_limiter_.find(conn->remote_ip_address()) ==
        req_limiter_.end()) {
  req_limiter_.insert(
    std::make_pair<std::string, std::shared_ptr<token_bucket>>(
        conn->remote_ip_address(),
        std::make_shared<token_bucket>(per_rate_,
                                        per_burst_size_)));
}

if (req_limiter_[conn->remote_ip_address()]->consume(1)) {
  start_one(conn).via(&conn->get_executor()).detach();
}
else {
  conn->close();
}
```

最后每IP请求限制还提供了一个清空缓存的API:

```cpp
void clear_per_ip_limiter_cache() { req_limiter_.clear(); }
```

缓存是容易被攻击，不停地向主机发送垃圾请求，此时就会因此不停地刷新cache造成每个请求在都会先在IP请求表中进行搜索。所以需要提供清理cache的函数。