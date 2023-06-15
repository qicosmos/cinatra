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

1. C++20 compiler (gcc 10.2, clang 13, Visual Studio 2022, or later versions)

## Examples

### Example 1: A simple "Hello World"

```cpp
#include "cinatra.hpp"
using namespace cinatra;

int main() {
	int max_thread_num = std::thread::hardware_concurrency();
	http_server server(max_thread_num);
	server.listen("0.0.0.0", "8080");
	server.set_http_handler<GET, POST>("/", [](request& req, response& res) {
		res.set_status_and_content(status_type::ok, "hello world");
	});

	server.run();
	return 0;
}
```

### Example 2: Access to request header, query parameter, and response

```cpp
#include "cinatra.hpp"
using namespace cinatra;

int main() {
	http_server server(std::thread::hardware_concurrency());
	server.listen("0.0.0.0", "8080");
	server.set_http_handler<GET, POST>("/test", [](request& req, response& res) {
		auto name = req.get_header_value("name");
		if (name.empty()) {
			res.set_status_and_content(status_type::bad_request, "no name");
			return;
		}

		auto id = req.get_query_value("id");
		if (id.empty()) {
			res.set_status_and_content(status_type::bad_request);
			return;
		}

		res.set_status_and_content(status_type::ok, "hello world");
	});

	server.run();
	return 0;
}
```

### Example 3: Aspect-oriented HTTP server

```cpp
#include "cinatra.hpp"
using namespace cinatra;

// Logging aspect
struct log_t
{
	bool before(request& req, response& res) {
		std::cout << "before log" << std::endl;
		return true;
	}

	bool after(request& req, response& res) {
		std::cout << "after log" << std::endl;
		return true;
	}
};

// Checking aspect
struct check {
	bool before(request& req, response& res) {
		std::cout << "before check" << std::endl;
		if (req.get_header_value("name").empty()) {
			res.set_status_and_content(status_type::bad_request);
			return false;
		}
		return true;
	}

	bool after(request& req, response& res) {
		std::cout << "after check" << std::endl;
		return true;
	}
};

// transfer data from aspect to http handler
struct get_data {
	bool before(request& req, response& res) {
		req.set_aspect_data("hello", std::string("hello world"));
		return true;
	}
}

int main() {
	http_server server(std::thread::hardware_concurrency());
	server.listen("0.0.0.0", "8080");
	server.set_http_handler<GET, POST>("/aspect", [](request& req, response& res) {
		res.set_status_and_content(status_type::ok, "hello world");
	}, check{}, log_t{});

	server.set_http_handler<GET,POST>("/aspect/data", [](request& req, response& res) {
		std::string hello = req.get_aspect_data<std::string>("hello");
		res.set_status_and_content(status_type::ok, std::move(hello));
	}, get_data{});

	server.run();
	return 0;
}
```

In this example, there are two aspects: one is to check the validity of the http request, and the other is the logging aspect. You can add pass as many aspects as needed to the `set_http_handler` method.

The order of execution of the aspects depends on the order that they are passed to the `set_http_handler` method. In this example, the aspect to check the validity of the http request is called first. If the request is not valid, it will return a Bad Request error. If it is valid, the next aspect (that is, the log aspect) will be called. The log aspect, through the `before` method, will print a log indicating the processing before entering the business logic. After the business logic is completed, the log aspect's `after` method will  print to indicate the processing after the end of the business logic.

### Example 4: File upload

Cinatra currently supports uploading of multipart and octet-stream formats.

#### Multi-part file upload

```cpp
#include <atomic>
#include "cinatra.hpp"
using namespace cinatra;

int main() {
	http_server server(std::thread::hardware_concurrency());
	server.listen("0.0.0.0", "8080");

	//http upload(multipart)
	server.set_http_handler<GET, POST>("/upload_multipart", [](request& req, response& res) {
		assert(req.get_content_type() == content_type::multipart);
		
		auto& files = req.get_upload_files();
		for (auto& file : files) {
			std::cout << file.get_file_path() << " " << file.get_file_size() << std::endl;
		}

		res.set_status_and_content(status_type::ok, "multipart finished");
	});

	server.run();
	return 0;
}
```

As you can see, a few lines of code can be used to implement a http file upload server, including exception handling and error handling.

#### Octet-stream file upload

```cpp
#include <atomic>
#include "cinatra.hpp"
using namespace cinatra;

int main() {
	http_server server(std::thread::hardware_concurrency());
	server.listen("0.0.0.0", "8080");

	//http upload(octet-stream)
	server.set_http_handler<GET, POST>("/upload_octet_stream", [](request& req, response& res) {
		assert(req.get_content_type() == content_type::octet_stream);
		auto& files = req.get_upload_files();
		for (auto& file : files) {
			std::cout << file.get_file_path() << " " << file.get_file_size() << std::endl;
		}

		res.set_status_and_content(status_type::ok, "octet-stream finished");
	});

	server.run();
	return 0;
}
```

### Example 5: File download

cinatra support chunked download files.

Make sure the files are in your resource dictionary(which you could set in the server, such as "./public/static"), and then you could download the files directly.

Here is the example:

Assume the file "show.jpg" is in the "./purecpp/static/" of the server, you just need to typing the address of the image, and you could download the image immediately.
```
//chunked download
http://127.0.0.1:8080/purecpp/static/show.jpg
//cinatra will send you the file, if the file is big file(more than 5M) the file will be downloaded by chunked. support continues download
```

### Example 6: WebSocket

```cpp
#include "cinatra.hpp"
using namespace cinatra;

int main() {
	http_server server(std::thread::hardware_concurrency());
	server.listen("0.0.0.0", "8080");

	//web socket
	server.set_http_handler<GET, POST>("/ws", [](request& req, response& res) {
		assert(req.get_content_type() == content_type::websocket);

		req.on(ws_open, [](request& req){
			std::cout << "websocket start" << std::endl;
		});

		req.on(ws_message, [](request& req) {
			auto part_data = req.get_part_data();
			//echo
			std::string str = std::string(part_data.data(), part_data.length());
			req.get_conn<cinatra::NonSSL>()->send_ws_string(std::move(str));
			std::cout << part_data.data() << std::endl;
		});

		req.on(ws_error, [](request& req) {
			std::cout << "websocket pack error or network error" << std::endl;
		});
	});

	server.run();
	return 0;
}
```

### Example 7: io_service_inplace

This code demonstrates how to use io_service_inplace and then control the running thread and loop of the http server itself. Use http://[::1]:8080/close (IPv6) or http://127.0.0.1:8080/close (IPv4) to shut down the http server.

```cpp
#include "cinatra.hpp"
using namespace cinatra;

int main() {

	bool is_running = true;
	http_server_<io_service_inplace> server;
	server.listen("8080");

	server.set_http_handler<GET, POST>("/", [](request& req, response& res) {
		res.set_status_and_content(status_type::ok, "hello world");
	});

	server.set_http_handler<GET, POST>("/close", [&](request& req, response& res) {
		res.set_status_and_content(status_type::ok, "will close");

		is_running = false;
		server.stop();
	});

	while(is_running)
		server.poll_one();

	return 0;
}
```

### Example 8: set RESTful API path parameters

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

### Example 9: cinatra client usage

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
```
async_simple::coro::Lazy<void> test_upload() {
  std::string uri = "http://example.com/";
  coro_http_client client{};
  auto result = co_await client.async_upload(uri, "test", "yourfile.jpg");
  print(result.status);
  std::cout << "upload finished\n";

  client.add_str_part("hello", "coro_http_client");
  client.add_file_part("test", "yourfile.jpg");
  result = co_await client.async_upload(uri);
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
  client.on_ws_close([](std::string_view reason) {
    std::cout << "web socket close " << reason << std::endl;
  });
  client.on_ws_msg([](resp_data data) {
    if (data.net_err) {
      std::cout << data.net_err.message() << "\n";
      return;
    }
    std::cout << data.resp_body << std::endl;
  });

  bool r = co_await client.async_ws_connect("ws://localhost:8090/ws");
  if (!r) {
    co_return;
  }

  auto result =
      co_await client.async_send_ws("hello websocket");  // mask as default.
  std::cout << result.status << "\n";
  result = co_await client.async_send_ws("test again", /*need_mask = */ false);
  std::cout << result.status << "\n";
  result = co_await client.async_send_ws_close("ws close");
  std::cout << result.status << "\n";
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

