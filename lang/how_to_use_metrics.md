# 概述
metric 包括4种指标类型：
- couter：只会增加的指标；
- gauge：可以增加或减少的指标，它派生于counter；
- histogram：直方图，初始化的时候需要设置桶(bucket)；
- summary：分位数指标，初始化的时候需要设置桶和误差；

# label

label：标签，可选，指标可以没有标签。标签是指一个键值对，键是需要在创建指标的时候设置，是静态不可变的。

值可以在创建指标的时候设置，这样的label则被称为静态的label。

值在运行期动态创建，则label被称为动态的label。

动态label的例子：

```cpp
{"method", "url"}
```
这个label只有键没有值，所以这个label是动态的label。后续动态生成label对应的值时，这样做：
```cpp
{"GET", "/"}
{"POST", "/test"}
```
使用的时候填动态的方法名和url就行了：
```cpp
some_counter.inc({std::string(req.method()), req.url()}, 1);
```
如果传入的标签值数量和创建时的label 键的数量不匹配时则会抛异常。

静态label的例子：
```cpp
{{"method", "GET"}, {"url", "/"}}
```
这个label的键值都确定了，是静态的，后面使用的时候需要显式调用静态的标签值使用:
```cpp
some_counter.inc({"GET", "/"}, 1);
```

无标签：创建指标的时候不设置标签，内部只有一个计数。

# counter和gauge的使用

## 创建没有标签的指标
```cpp
    gauge_t g{"test_gauge", "help"};
    g.inc();
    g.inc(1);

    std::string str;
    g.serialize(str);
    CHECK(str.find("test_gauge 2") != std::string::npos);

    g.dec(1);
    CHECK(g.value() == 1);
    g.update(1);

    CHECK_THROWS_AS(g.dec({"test"}, 1), std::invalid_argument);
    CHECK_THROWS_AS(g.inc({"test"}, 1), std::invalid_argument);
    CHECK_THROWS_AS(g.update({"test"}, 1), std::invalid_argument);

    counter_t c{"test_counter", "help"};
    c.inc();
    c.inc(1);
    std::string str1;
    c.serialize(str1);
    CHECK(str1.find("test_counter 2") != std::string::npos);
```
## counter/gauge指标的api

构造函数:
```cpp
// 无标签，调用inc时不带标签，如c.inc()
// name: 指标对象的名称，注册到指标管理器时会使用这个名称
// help: 指标对象的帮助信息
counter_t(std::string name, std::string help);

// labels: 静态标签，构造时需要将标签键值都填完整，如：{{"method", "GET"}, {"url", "/"}}
// 调用inc时必须带静态标签的值，如：c.inc({"GET", "/"}, 1);
counter_t(std::string name, std::string help,
            std::map<std::string, std::string> labels);

// labels_name: 动态标签的键名称，因为标签的值是动态的，而键的名称是固定的，所以这里只需要填键名称，如: {"method", "url"}
// 调用时inc时必须带动态标签的值，如：c.inc({method, url}, 1);
counter_t(std::string name, std::string help,
            std::vector<std::string> labels_name);
```

基本函数：
```cpp
// 获取无标签指标的计数，
double value();

// 根据标签获取指标的计数，参数为动态或者静态标签的值
double value(const std::vector<std::string> &labels_value);

// 无标签指标增加计数，counter的计数只能增加不能减少，如果填入的时负数时会抛异常；如果需要减少计数的指标则应该使用gauge；
void inc(double val = 1);

// 根据标签增加计数，如果创建的指标是静态标签值且和传入的标签值不匹配时会抛异常；如果标签的值的数量和构造指标时传入的标签数量不相等时会抛异常。
void inc(const std::vector<std::string> &labels_value, double value = 1);

// 序列化，将指标序列化成prometheus 格式的字符串
void serialize(std::string &str);

// 返回带标签的指标内部的计数map，map的key是标签的值，值是对应计数，如：{{{"GET", "/"}, 100}, {{"POST", "/test"}, 20}}
std::map<std::vector<std::string>, double,
           std::less<std::vector<std::string>>>
  value_map();
```

注意：如果使用动态标签的时候要注意这个动态的标签值是不是无限多的，如果是无限多的话，那么内部的map也会无限增长，应该避免这种情况，动态的标签也应该是有限的才对。

gauge 派生于counter，相比counter多了一个减少计数的api
```cpp
// 无标签指标减少计数
void dec(double value = 1);

// 根据标签减少计数，如果创建的指标是静态标签值且和传入的标签值不匹配时会抛异常；如果标签的值的数量和构造指标时传入的标签数量不相等时会抛异常。
void dec(const std::vector<std::string>& labels_value, double value = 1);
```

# 基类公共函数
所有指标都派生于metric_t 基类，提供了一些公共方法，如获取指标的名称，指标的类型，标签的键名称等等。

```cpp
  // 获取指标对象的名称
  std::string_view name();

  // 获取指标对象的help 信息
  std::string_view help();

  // 指标类型
  enum class MetricType {
    Counter,
    Gauge,
    Histogram,
    Summary,
    Nil,
  };

  // 获取指标类型
  MetricType metric_type();

  // 获取指标类型的名称，比如counter, gauge, histogram和summary
  std::string_view metric_name();

  // 获取标签的键，如{"method", "url"}
  const std::vector<std::string>& labels_name();

  // 序列化，调用派生类实现序列化
  virtual void serialize(std::string& str);

  // 给summary专用的api，序列化，调用派生类实现序列化
  virtual async_simple::coro::Lazy<void> serialize_async(std::string& out);

  // 将基类指针向下转换到派生类指针，如:
  // std::shared_ptr<metric_t> c = std::make_shared<counter_t>("test", "test");
  // counter_t* t = c->as<counter_t*>();
  // t->value();
  template <typename T>
  T* as();
```

# 指标管理器
如果希望集中管理多个指标时，则需要将指标注册到指标管理器，后面则可以多态调用管理器中的多个指标将各自的计数输出出来。

**推荐在一开始就创建所有的指标并注册到管理器**，后面就可以无锁方式根据指标对象的名称来获取指标对象了。

```cpp
auto c = std::make_shared<counter_t>("qps_count", "qps help");
auto g = std::make_shared<gauge_t>("fd_count", "fd count help");
default_metric_manger::register_metric_static(c);
default_metric_manger::register_metric_static(g);

c->inc();
g->inc();

auto m = default_metric_manger::get_metric_static("qps_count");
CHECK(m->as<counter_t>()->value() == 1);

auto m1 = default_metric_manger::get_metric_static("fd_count");
CHECK(m1->as<gauge_t>()->value() == 1);
```

如果希望动态注册的到管理器则应该调用register_metric_dynamic接口，后面根据名称获取指标对象时则调用get_metric_dynamic接口，dynamic接口内部会加锁。
```cpp
auto c = std::make_shared<counter_t>("qps_count", "qps help");
auto g = std::make_shared<gauge_t>("fd_count", "fd count help");
default_metric_manger::register_metric_dynamic(c);
default_metric_manger::register_metric_dynamic(g);

c->inc();
g->inc();

auto m = default_metric_manger::get_metric_dynamic("qps_count");
CHECK(m->as<counter_t>()->value() == 1);

auto m1 = default_metric_manger::get_metric_dynamic("fd_count");
CHECK(m1->as<gauge_t>()->value() == 1);
```
注意：一旦注册时使用了static或者dynamic，那么后面调用default_metric_manger时则应该使用相同后缀的接口，比如注册时使用了get_metric_static，那么后面调用根据名称获取指标对象的方法必须是get_metric_static，否则会抛异常。同样，如果注册使用register_metric_dynamic，则后面应该get_metric_dynamic，否则会抛异常。

指标管理器的静态api
```cpp
  // 注册metric
  static bool register_metric_static(std::shared_ptr<metric_t> metric);
  static bool register_metric_dynamic(std::shared_ptr<metric_t> metric);

  // 获取注册的所有指标对象
  static std::map<std::string, std::shared_ptr<metric_t>> metric_map_static();
  static std::map<std::string, std::shared_ptr<metric_t>> metric_map_dynamic();

  // 获取注册的指标对象的总数
  static size_t metric_count_static();
  static size_t metric_count_dynamic();

  // 获取注册的指标对象的名称
  static std::vector<std::string> metric_keys_static();
  static std::vector<std::string> metric_keys_dynamic();

  // 根据名称获取指标对象
  static std::shared_ptr<metric_t> get_metric_static(const std::string& name);
  static std::shared_ptr<metric_t> get_metric_dynamic(const std::string& name);

  // 序列化
  static async_simple::coro::Lazy<std::string> serialize_static();
  static async_simple::coro::Lazy<std::string> serialize_dynamic();
```

# histogram

## api
```cpp
//
// name: 对象名称，help：帮助信息
// buckets：桶，如 {1, 3, 7, 11, 23}，后面设置的值会自动落到某个桶中并增加该桶的计数；
// 内部还有一个+Inf 默认的桶，当输入的数据不在前面设置这些桶中，则会落到+Inf 默认桶中。
// 实际上桶的总数为 buckets.size() + 1
// 每个bucket 实际上对应了一个counter指标
histogram_t(std::string name, std::string help, std::vector<double> buckets);

// 往histogram_t 中插入数据，内部会自动增加对应桶的计数
void observe(double value);

// 获取所有桶对应的counter指标对象
std::vector<std::shared_ptr<counter_t>> get_bucket_counts();

// 序列化
void serialize(std::string& str);
```
## 例子
```cpp
  histogram_t h("test", "help", {5.0, 10.0, 20.0, 50.0, 100.0});
  h.observe(23);
  auto counts = h.get_bucket_counts();
  CHECK(counts[3]->value() == 1);
  h.observe(42);
  CHECK(counts[3]->value() == 2);
  h.observe(60);
  CHECK(counts[4]->value() == 1);
  h.observe(120);
  CHECK(counts[5]->value() == 1);
  h.observe(1);
  CHECK(counts[0]->value() == 1);
  std::string str;
  h.serialize(str);
  std::cout << str;
  CHECK(str.find("test_count") != std::string::npos);
  CHECK(str.find("test_sum") != std::string::npos);
  CHECK(str.find("test_bucket{le=\"5") != std::string::npos);
  CHECK(str.find("test_bucket{le=\"+Inf\"}") != std::string::npos);
```

# summary
## api

```cpp
// Quantiles: 百分位和误差, 如：{{0.5, 0.05}, {0.9, 0.01}, {0.95, 0.005}, {0.99, 0.001}}
summary_t(std::string name, std::string help, Quantiles quantiles);

// 往summary_t插入数据，会自动计算百分位的数量
void observe(double value);

// 获取百分位结果
async_simple::coro::Lazy<std::vector<double>> get_rates();

// 获取总和
async_simple::coro::Lazy<double> get_sum();

// 获取插入数据的个数
async_simple::coro::Lazy<uint64_t> get_count();

// 序列化
async_simple::coro::Lazy<void> serialize_async(std::string &str);
```

## 例子
```cpp
  summary_t summary{"test_summary",
                    "summary help",
                    {{0.5, 0.05}, {0.9, 0.01}, {0.95, 0.005}, {0.99, 0.001}}};
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> distr(1, 100);
  for (int i = 0; i < 50; i++) {
    summary.observe(distr(gen));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::string str;
  async_simple::coro::syncAwait(summary.serialize_async(str));
  std::cout << str;
  CHECK(async_simple::coro::syncAwait(summary.get_count()) == 50);
  CHECK(async_simple::coro::syncAwait(summary.get_sum()) > 0);
  CHECK(str.find("test_summary") != std::string::npos);
  CHECK(str.find("test_summary_count") != std::string::npos);
  CHECK(str.find("test_summary_sum") != std::string::npos);
  CHECK(str.find("test_summary{quantile=\"") != std::string::npos);
```
summary 百分位的计算相比其它指标是最耗时的，应该避免在关键路径上使用它以免对性能造成影响。
