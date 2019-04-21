# Cinatra - an efficient and easy-to-use C++ HTTP framework

## Table of Contents
* [Introduction](#introduction)
* [Usage](#usage)
* [Examples](#examples)
* [Performance](#performance)
* [Caveats](#caveats)
* [Roadmap](#roadmap)
* [Contact](#contact)

## Introduction
Cinatra is a high-performance, easy-to-use http framework developed in Modern C++ (C++17) with the goal of making it easy and quick to develop web applications using the C++ programming language. Its main features are as follows:

1. Unified and simple interface,
2. Header-only,
3. Cross-platform,
4. Efficient
5. Support for AOP (aspect-oriented programming)

Cinatra currently supports HTTP 1.1/1.0, TLS/SSL and [WebSocket](https://www.wikiwand.com/en/WebSocket) protocols. You can use it to easily develop an HTTP server, such as a common database access server, a file upload/download server, real-time message push server, as well as a [MQTT](https://www.wikiwand.com/en/MQTT) server.

## Usage

Cinatra is a header-only library. So you can immediately use it in your code with a simple `#include` directive.

To compile your code with Cinatra, you need the following:

1. C++17 compiler (gcc 7.2, clang 4.0, Visual Studio 2017 update 15.5, or later versions)
2. Boost.Asio (or standalone Asio)
3. Boost.System
4. A UUID library (objbase.h for Windows, uuid.h for Linux, CFUUID.h for Mac)

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

int main() {
	http_server server(std::thread::hardware_concurrency());
	server.listen("0.0.0.0", "8080");
	server.set_http_handler<GET, POST>("/aspect", [](request& req, response& res) {
		res.set_status_and_content(status_type::ok, "hello world");
	}, check{}, log_t{});

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

```
TO BE DONE
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
			req.get_conn()->send_ws_string(std::move(str));
			std::cout << part_data.data() << std::endl;
		});

		req.on(ws_close, [](request& req) {
			std::cout << "websocket close" << std::endl;
		});

		req.on(ws_error, [](request& req) {
			std::cout << "websocket error" << std::endl;
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

## Performance

We conducted a simple performance test using the [Apache HTTP benchmarking tool, ab](http://httpd.apache.org/docs/2.2/programs/ab.html).

```bash
ab -c100 -n5000 127.0.0.1:8080/
```

The server simply returns a hello.  It was tested on an 8-core 16G cloud host with 9000 and 13000 qps (queries per second).

In comparison with Boost.Beast with a similar code, the qps values are quite similar, probably because both cinatra and Boost.Beast are based on Boost.Asio. We have not yet done any special performance optimization with cinatra and there is room for improvement.

## Caveats

File upload and download, WebSocket business functions will enter multiple times, so you need to pay attention when writing business logic, it is recommended to do it in the same manner as the example.

Cinatra is currently in use in the production environment, and is still in the stage of development and improvement. There may be some bugs. Therefore, it is not recommended to use it directly in the production environment at this stage. It is recommended to try it first in the test environment (which is what you normally do for any new code) prior to using it in a production environment.

If you find any problems during the trial, please feel free to contact us or email me.

After testing and stable use, cinatra will release the official version.

## Roadmap

1. Add a basic client for communication between servers

I hope that more and more people will use cinatra and like it. I also hope that cinatra will become more and more perfect in the process of use. It will become a powerful and easy-to-use http framework. We welcome everyone to actively participate in the cinatra project. You can send an email to suggest, or you can do a pull request, the form is not limited.

In the recent refactoring of Cinatra, it was almost rewritten, the code is more than 30% less than the previous one, the interface is unified, http and business are separated, with better scalability and maintainability.

## Contact

purecpp@163.com

qq：340713904

[http://purecpp.org/](http://purecpp.org/ "purecpp")

[https://github.com/qicosmos/cinatra](https://github.com/qicosmos/cinatra "cinatra")

