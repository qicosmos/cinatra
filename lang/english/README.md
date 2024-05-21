# Cinatra - an efficient and easy-to-use C++ HTTP framework

<p align="center">
  <span>English</span> | <a href="https://github.com/qicosmos/cinatra">中文</a>
</p>

## Table of Contents
* [Introduction](#introduction)
* [Usage](#usage)
* [Examples](#examples)
* [Performance](#performance)
* [Caveats](#caveats)
* [Roadmap](#roadmap)
* [Contact](#contact)

## Introduction
Cinatra is a high-performance, easy-to-use http framework developed in Modern C++ (C++20) with the goal of making it easy and quick to develop web applications using the C++ programming language. Its main features are as follows:

1. Unified and simple interface,
2. Header-only,
3. Cross-platform,
4. Efficient
5. Support for AOP (aspect-oriented programming)

Cinatra currently supports HTTP 1.1/1.0, TLS/SSL and [WebSocket](https://www.wikiwand.com/en/WebSocket) protocols. You can use it to easily develop an HTTP server, such as a common database access server, a file upload/download server, real-time message push server, as well as a [MQTT](https://www.wikiwand.com/en/MQTT) server.

Cinatra also provides a C++ 20 coroutine http(https) client, include such functions: get/post, upload(multipart), download(chunked and ranges), websocket, redirect, proxy etc.

## Usage

Cinatra is a header-only library. So you can immediately use it in your code with a simple `#include` directive.

To compile your code with Cinatra, you need the following:

if linux, setting:

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -pthread -std=c++20")

if use g++, setting:

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fno-tree-slp-vectorize")

1. C++20 compiler (gcc 10.2, clang 13, Visual Studio 2022, or later versions)

## Usage:cinatra instruction set optimization

cinatra supports optimizing its internal logic through the instruction set, which controls whether to use the instruction set through macros. Please make sure the cpu support before use.

Use the following command to compile cinatra with simd optimization.Note that only one simd instruction set can be opened, and opening multiple instruction sets will cause compilation failure.

```shell
cmake -DENABLE_SIMD=SSE42 .. # enable sse4.2 instruction set
cmake -DENABLE_SIMD=AVX2 .. # enable avx2 instruction set
cmake -DENABLE_SIMD=AARCH64 .. # enable neon instruction set in aarch64
```

## Examples

### Example 1: A simple "Hello World"

```c++
	#include "include/cinatra.hpp"
	using namespace cinatra;
	
	int main() {
		int max_thread_num = std::thread::hardware_concurrency();
		coro_http_server server(max_thread_num, 8080);
		server.set_http_handler<GET, POST>("/", [](coro_http_request& req, coro_http_response& res) {
			res.set_status_and_content(status_type::ok, "hello world");
		});

		server.sync_start();
		return 0;
	}
```

### Example 2: Access to request header, query parameter, and response

```c++
#include "cinatra.hpp"

struct person_t {
  void foo(coro_http_request &, coro_http_response &res) {
    res.set_status_and_content(status_type::ok, "ok");
  }
};

async_simple::coro::Lazy<void> basic_usage() {
  coro_http_server server(1, 9001);
  server.set_http_handler<GET>(
      "/get", [](coro_http_request &req, coro_http_response &resp) {
        resp.set_status_and_content(status_type::ok, "ok");
      });

  server.set_http_handler<GET>(
      "/coro",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_content(status_type::ok, "ok");
        co_return;
      });

  server.set_http_handler<GET>(
      "/in_thread_pool",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        // will respose in another thread.
        co_await coro_io::post([&] {
          // do your heavy work here when finished work, response.
          resp.set_status_and_content(status_type::ok, "ok");
        });
      });

  server.set_http_handler<POST, PUT>(
      "/post", [](coro_http_request &req, coro_http_response &resp) {
        auto req_body = req.get_body();
        resp.set_status_and_content(status_type::ok, std::string{req_body});
      });

  server.set_http_handler<GET>(
      "/headers", [](coro_http_request &req, coro_http_response &resp) {
        auto name = req.get_header_value("name");
        auto age = req.get_header_value("age");
        assert(name == "tom");
        assert(age == "20");
        resp.set_status_and_content(status_type::ok, "ok");
      });

  server.set_http_handler<GET>(
      "/query", [](coro_http_request &req, coro_http_response &resp) {
        auto name = req.get_query_value("name");
        auto age = req.get_query_value("age");
        assert(name == "tom");
        assert(age == "20");
        resp.set_status_and_content(status_type::ok, "ok");
      });

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/users/:userid/subscriptions/:subid",
      [](coro_http_request &req, coro_http_response &response) {
        assert(req.params_["userid"] == "ultramarines");
        assert(req.params_["subid"] == "guilliman");
        response.set_status_and_content(status_type::ok, "ok");
      });

  person_t person{};
  server.set_http_handler<GET>("/person", &person_t::foo, person);

  server.async_start();
  std::this_thread::sleep_for(300ms);  // wait for server start

  coro_http_client client{};
  auto result = co_await client.async_get("http://127.0.0.1:9001/get");
  assert(result.status == 200);
  assert(result.resp_body == "ok");
  for (auto [key, val] : result.resp_headers) {
    std::cout << key << ": " << val << "\n";
  }

  result = co_await client.async_get("/coro");
  assert(result.status == 200);

  result = co_await client.async_get("/in_thread_pool");
  assert(result.status == 200);

  result = co_await client.async_post("/post", "post string",
                                      req_content_type::string);
  assert(result.status == 200);
  assert(result.resp_body == "post string");

  client.add_header("name", "tom");
  client.add_header("age", "20");
  result = co_await client.async_get("/headers");
  assert(result.status == 200);

  result = co_await client.async_get("/query?name=tom&age=20");
  assert(result.status == 200);

  result = co_await client.async_get(
      "http://127.0.0.1:9001/users/ultramarines/subscriptions/guilliman");
  assert(result.status == 200);

  // make sure you have installed openssl and enable CINATRA_ENABLE_SSL
#ifdef CINATRA_ENABLE_SSL
  coro_http_client client2{};
  result = co_await client2.async_get("https://baidu.com");
  assert(result.status == 200);
#endif
}

int main() {
  async_simple::coro::syncAwait(basic_usage());
}
```

### Example 3: Aspect-oriented HTTP server

```c++
	#include "cinatra.hpp"
	using namespace cinatra;

	//日志切面
	struct log_t
	{
		bool before(coro_http_request& req, coro_http_response& res) {
			std::cout << "before log" << std::endl;
			return true;
		}
	
		bool after(coro_http_request& req, coro_http_response& res) {
			std::cout << "after log" << std::endl;
			return true;
		}
	};
	
	//校验的切面
	struct check  {
		bool before(coro_http_request& req, coro_http_response& res) {
			std::cout << "before check" << std::endl;
			if (req.get_header_value("name").empty()) {
				res.set_status_and_content(status_type::bad_request);
				return false;
			}
			return true;
		}
	
		bool after(coro_http_request& req, coro_http_response& res) {
			std::cout << "after check" << std::endl;
			return true;
		}
	};

	//将信息从中间件传输到处理程序
	struct get_data  {
		bool before(coro_http_request& req, coro_http_response& res) {
			req.set_aspect_data("hello world");
			return true;
		}
	}

	int main() {
		coro_http_server server(std::thread::hardware_concurrency(), 8080);
		server.set_http_handler<GET, POST>("/aspect", [](coro_http_request& req, coro_http_response& res) {
			res.set_status_and_content(status_type::ok, "hello world");
		}, check{}, log_t{});

		server.set_http_handler<GET,POST>("/aspect/data", [](coro_http_request& req, coro_http_response& res) {
      auto &val = req.get_aspect_data();
			std::string& hello = val[0];
			res.set_status_and_content(status_type::ok, std::move(hello));
		}, get_data{});

		server.sync_start();
		return 0;
	}
```

In this example, there are two aspects: one is to check the validity of the http request, and the other is the logging aspect. You can add pass as many aspects as needed to the `set_http_handler` method.

The order of execution of the aspects depends on the order that they are passed to the `set_http_handler` method. In this example, the aspect to check the validity of the http request is called first. If the request is not valid, it will return a Bad Request error. If it is valid, the next aspect (that is, the log aspect) will be called. The log aspect, through the `before` method, will print a log indicating the processing before entering the business logic. After the business logic is completed, the log aspect's `after` method will  print to indicate the processing after the end of the business logic.

### Example 4: File upload, download, websocket

see[example](../../example/main.cpp)

### Example : set RESTful API path parameters

This code demonstrates how to use RESTful path parameters. Two RESTful APIs are set up below. When accessing the first API, such as the url `http://127.0.0.1:8080/numbers/1234/test/5678`, the server can get the two parameters of 1234 and 5678, the first RESTful API The parameter is `(\d+)`, which is a regex expression, which means that the parameter can only be a number. The code to get the first parameter is `req.get_matches ()[1]`. Because each req is different, each matched parameter is placed in the `request` structure.

At the same time, it also supports RESTful API with any character, that is, the second RESTful API in the example, and the path parameter is set to `"/string/{:id}/test/{:name}"`. To get the corresponding parameters, use the `req.get_query_value` function. The parameters can only be registered variables (if you access non-registered variables, it will run but report an error). In the example, the parameter names are id and name. To get the id parameter, call `req.get_query_value("id")` will do. After the sample code is displayed, when accessing `http://127.0.0.1:8080/string/params_1/test/api_test`, the browser will return the `api_test` string.

```cpp
#include "cinatra.hpp"
using namespace cinatra;

int main() {
	int max_thread_num = std::thread::hardware_concurrency();
	http_server server(max_thread_num);
	server.listen("0.0.0.0", "8080");

	server.set_http_handler<GET, POST>(
		R"(/numbers/(\d+)/test/(\d+))", [](request &req, response &res) {
			std::cout << " matches[1] is : " << req.get_matches()[1]
					<< " matches[2] is: " << req.get_matches()[2] << std::endl;

			res.set_status_and_content(status_type::ok, "hello world");
		});

	server.set_http_handler<GET, POST>(
		"/string/{:id}/test/{:name}", [](request &req, response &res) {
			std::string id = req.get_query_value("id");
			std::cout << "id value is: " << id << std::endl;
			std::cout << "name value is: " << std::string(req.get_query_value("name")) << std::endl;
			res.set_status_and_content(status_type::ok, std::string(req.get_query_value("name")));
		});

	server.run();
	return 0;
}
```

### Example : cinatra client usage

#### sync_send get/post message

```
void test_sync_client() {
  {
    std::string uri = "http://www.baidu.com";
    coro_http_client client{};
    auto result = client.get(uri);
    assert(!result.net_err);
    print(result.resp_body);

    result = client.post(uri, "hello", req_content_type::json);
    print(result.resp_body);
  }

  {
    coro_http_client client{};
    std::string uri = "http://cn.bing.com";
    auto result = client.get(uri);
    assert(!result.net_err);
    print(result.resp_body);

    result = client.post(uri, "hello", req_content_type::json);
    print(result.resp_body);
  }
}

#ifdef CINATRA_ENABLE_SSL
void test_coro_http_client() {
  using namespace cinatra;
  coro_http_client client{};
  client.init_ssl("../../include/cinatra", "server.crt");
  auto data = client.get("https://www.bing.com");
  std::cout << data.resp_body << "\n";
  data = client.get("https://www.bing.com");
  std::cout << data.resp_body << "\n";
}
#endif
```

#### async get/post message

```
async_simple::coro::Lazy<void> test_async_client() {
  std::string uri = "http://www.baidu.com";

  {
    coro_http_client client{};
    auto data = co_await client.async_get(uri);
    print(data.status);

    data = co_await client.async_get(uri);
    print(data.status);

    data = co_await client.async_post(uri, "hello", req_content_type::string);
    print(data.status);
  }

#ifdef CINATRA_ENABLE_SSL
  std::string uri2 = "https://www.baidu.com";
  std::string uri3 = "https://cn.bing.com";
  coro_http_client client{};
  client.init_ssl("../../include/cinatra", "server.crt");
  data = co_await client.async_get(uri2);
  print(data.status);

  data = co_await client.async_get(uri3);
  print(data.status);
#endif
}
```

#### upload(multipart) file
```cpp
void start_server() {
  coro_http_server server(1, 9001);
  server.set_http_handler<POST>(
      "/form_data",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        assert(req.get_content_type() == content_type::multipart);
        auto boundary = req.get_boundary();
        multipart_reader_t multipart(req.get_conn());
        while (true) {
          auto part_head = co_await multipart.read_part_head(boundary);
          if (part_head.ec) {
            co_return;
          }

          std::cout << part_head.name << "\n";
          std::cout << part_head.filename << "\n";// if form data, no filename

          auto part_body = co_await multipart.read_part_body(boundary);
          if (part_body.ec) {
            co_return;
          }

          std::cout << part_body.data << "\n";

          if (part_body.eof) {
            break;
          }
        }

        resp.set_status_and_content(status_type::ok, "multipart finished");
      });
  server.start();      
}
```
```cpp
async_simple::coro::Lazy<void> test_upload() {
  std::string uri = "http://127.0.0.1:9001/form_data";
  coro_http_client client{};

  client.add_str_part("hello", "coro_http_client");
  client.add_file_part("test", "yourfile.jpg");
  result = co_await client.async_upload_multipart(uri);
  print(result.status);
  std::cout << "upload finished\n";
}
```

#### download file(ranges and chunked)

```
async_simple::coro::Lazy<void> test_download() {
  coro_http_client client{};
  std::string uri =
      "http://www.httpwatch.com/httpgallery/chunked/chunkedimage.aspx";
  std::string filename = "test.jpg";

  std::error_code ec{};
  std::filesystem::remove(filename, ec);
  auto r = co_await client.async_download(uri, filename);
  assert(!r.net_err);
  assert(r.status == 200);
  std::cout << "download finished\n";
}
```

#### web socket
```c++
async_simple::coro::Lazy<void> test_websocket() {
  coro_http_client client{};
  auto r = co_await client.connect("ws://localhost:8090/ws");
  if (r.net_err) {
    co_return;
  }

  co_await client.write_websocket("hello websocket");
  auto data = co_await client.read_websocket();
  CHECK(data.resp_body == "hello websocket");
  co_await client.write_websocket("test again");
  data = co_await client.read_websocket();
  CHECK(data.resp_body == "test again");
  co_await client.write_websocket("ws close");
  data = co_await client.read_websocket();
  CHECK(data.net_err == asio::error::eof);
  CHECK(data.resp_body == "ws close");
}
```

### press tool usage

cinatra_press_tool is a modern HTTP benchmarking tool based on coro_http_client.

#### Basic Usage

```shell
./cinatra_press_tool -t 4 -c 40 -d 30s http://127.0.0.1
```

This runs a benchmark for 30 seconds, using 4 threads, and keeping 40 HTTP connections open(each connection corresponds to a coroutine).

Output:

```
Running 30s test @ http://127.0.0.1
  4 threads and 40 connections
  Thread Status   Avg   Max   Variation   Stdev
    Latency   4.12ms     8.15ms     3.367ms     1.835ms
  462716 requests in 30.001s, 592.198250MB read, total: 462716, errors: 0
Requests/sec:     15423.86666667
Transfer/sec:     19.739390MB
```

#### Command Line Options
```
 -c, --connections    total number of HTTP connections to keep open with 
 					  each thread handling N = connections/threads (int)
 -d, --duration       duration of the test, e.g. 2s, 2m, 2h (string [=15s])
 -t, --threads        total number of threads to use (int [=1])
 -H, --headers        HTTP headers to add to request, e.g. "User-Agent: coro_http_press"
            		  add multiple http headers in a request need to be separated by ' && '
            		  e.g. "User-Agent: coro_http_press && x-frame-options: SAMEORIGIN" (string [=])
 -r, --readfix        read fixed response (int [=0])
 -?, --help           print this message.
```

There are two parameters here that are different from wrk.

The `-H` parameter means adding an http header to the http request. This parameter can add one http header or use the ` && ` symbol (4 characters) as a separator to add multiple http headers to the request.st.
e.g. `-H User-Agent: coro_http_press` is to add an http header, and `-H User-Agent: coro_http_press && x-frame-options: SAMEORIGIN` is to add two http headers which is `User-Agent: coro_http_press` and `x-frame-options: same origin`.Adding three or more are all similar.


`-r `parameter, which indicates whether to read a fixed-length response, this parameter can avoid frequent parsing of the response to optimize performance, some servers may return different lengths for the same request, in this case, do not set -r to 1, or do not set this parameter.

## Performance

![qps](../qps.png "qps")

![qps-pipeline](../qps-pipeline.png "qps-pipeline")

## Caveats

When using WebSocket, the `request.on` method will be called multiple times, so you need to pay attention when writing your business logic. It is recommended to do it in the same manner as the example.

## Contact

purecpp@163.com

qq：340713904

[http://purecpp.org/](http://purecpp.org/ "purecpp")

[https://github.com/qicosmos/cinatra](https://github.com/qicosmos/cinatra "cinatra")

