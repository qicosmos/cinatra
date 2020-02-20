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

cinatra support chunked download files.

Make sure the files are in your resource dictionary(which you could set in the server, such as "./public/static"), and then you could download the fiels directly.

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
			req.get_conn()->send_ws_string(std::move(str));
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

### Example 8: cinatra client usage

#### send get/post message

```
auto client = cinatra::client_factory::instance().new_client("127.0.0.1", "8080");
client->send_msg("/string", "hello"); //post json, default timeout is 3000ms
client->send_msg<TEXT>("/string", "hello"); //post string, default timeout is 3000ms

client->send_msg<TEXT, 2000>("/string", "hello"); //post string, timeout is 2000ms

client->send_msg<TEXT, 3000, GET>("/string", "hello"); //get string, timeout is 3000ms
```

#### upload file
```
auto client = cinatra::client_factory::instance().new_client("127.0.0.1", "8080");
client->on_progress([](std::string progress) {
	std::cout << progress << "\n";
});

client->upload_file("/upload_multipart", filename, [](auto ec) {
	if (ec) {
		std::cout << "upload failed, reason: "<<ec.message();
	}
	else {
		std::cout << "upload successful\n";
	}
});
```

***upload multiple files***

```
	for (auto& filename : v) {

		auto client = cinatra::client_factory::instance().new_client("127.0.0.1", "8080");
		client->on_progress([](std::string progress) {
			std::cout << progress << "\n";
		});

		client->upload_file("/upload_multipart", filename, [](auto ec) {
			if (ec) {
				std::cout << "upload failed, reason: "<<ec.message();
			}
			else {
				std::cout << "upload successful\n";
			}
		});

	}
```

#### download file

```
auto client = cinatra::client_factory::instance().new_client("127.0.0.1", "8080");
auto s = "/public/static/test1.png";
auto filename = std::filesystem::path(s).filename().string();
client->download_file("temp", filename, s, [](auto ec) {
	if (ec) {
		std::cout << ec.message() << "\n";
	}
	else {
		std::cout << "ok\n";
	}
});
```

```
client->on_length([](size_t length){
	std::cout<<"recieved data length: "<<length<<"\n";
});

client->on_data([](std::string_view data){
	std::cout<<"recieved data: "<<data<<"\n";
});
```

### simple_client https

```
	boost::asio::ssl::context ctx(boost::asio::ssl::context::sslv23);
	ctx.set_default_verify_paths();

	auto client = cinatra::client_factory::instance().new_client("127.0.0.1", "https", ctx);
	client->on_length([](size_t _length) {
		std::cout << "download file: on_length: " << _length << std::endl;
	});
	client->download_file("test.jpg", "/public/static/test.jpg", [](boost::system::error_code ec) {
		std::cout << "download file: on_complete: " << (!ec ? "true - " : "false - ") << (ec ? ec.message() : "") << std::endl;
	});

	std::string ss;
	std::cin >> ss;
```

## Performance

![qps](../qps.png "qps")

![qps-pipeline](../qps-pipeline.png "qps-pipeline")

## Caveats

When using WebSocket, the `request.on` method will be called multiple times, so you need to pay attention when writing your business logic. It is recommended to do it in the same manner as the example.

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

