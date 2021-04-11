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

### Example 8: cinatra client usage

#### sync_send get/post message

```
void print(const response_data& result) {
    print(result.ec, result.status, result.resp_body, result.resp_headers.second);
}

void test_sync_client() {
    auto client = cinatra::client_factory::instance().new_client();
    std::string uri = "http://www.baidu.com";
    std::string uri1 = "http://cn.bing.com";
    std::string uri2 = "https://www.baidu.com";
    std::string uri3 = "https://cn.bing.com";
    
    response_data result = client->get(uri);
    print(result);

    response_data result1 = client->get(uri1);
    print(result1);

    print(client->post(uri, "hello"));
    print(client->post(uri1, "hello"));

#ifdef CINATRA_ENABLE_SSL
    response_data result2 = client->get(uri2);
    print(result2);

    response_data result3 = client->get(uri3);
    print(result3);

    response_data result4 = client->get(uri3);
    print(result4);

    response_data result5 = client->get(uri2);
    print(result5);
#endif
}
```

#### sync_send get/post message

```
void test_async_client() {
    
    std::string uri = "http://www.baidu.com";
    std::string uri1 = "http://cn.bing.com";
    std::string uri2 = "https://www.baidu.com";
    std::string uri3 = "https://cn.bing.com";

    {
        auto client = cinatra::client_factory::instance().new_client();
        client->async_get(uri, [](response_data data) {
            print(data);
        });
    }
    
    {
        auto client = cinatra::client_factory::instance().new_client();
        client->async_get(uri1, [](response_data data) {
            print(data);
        });
    }

    {
        auto client = cinatra::client_factory::instance().new_client();
        client->async_post(uri, "hello", [](response_data data) {
            print(data);
        });
    }

#ifdef CINATRA_ENABLE_SSL
    {
        auto client = cinatra::client_factory::instance().new_client();
        client->async_get(uri2, [](response_data data) {
            print(data);
        });
    }

    {
        auto client = cinatra::client_factory::instance().new_client();
        client->async_get(uri3, [](response_data data) {
            print(data);
        });
    }
#endif
}
```

#### upload(multipart) file
```
void test_upload() {
    std::string uri = "http://cn.bing.com/";
    auto client = cinatra::client_factory::instance().new_client();
    client->upload(uri, "boost_1_72_0.7z", [](response_data data) {
        if (data.ec) {
            std::cout << data.ec.message() << "\n";
            return;
        }

        std::cout << data.resp_body << "\n"; //finished upload
    });
}
```

#### download file

```
void test_download() {
    std::string uri = "http://www.httpwatch.com/httpgallery/chunked/chunkedimage.aspx";

    {
        auto client = cinatra::client_factory::instance().new_client();
        client->download(uri, "test.jpg", [](response_data data) {
            if (data.ec) {
                std::cout << data.ec.message() << "\n";
                return;
            }

            std::cout << "finished download\n";
        });
    }

    {
        auto client = cinatra::client_factory::instance().new_client();
        client->download(uri, [](auto ec, auto data) {
            if (ec) {
                std::cout << ec.message() << "\n";
                return;
            }

            if (data.empty()) {
                std::cout << "finished all \n";
            }
            else {
                std::cout << data.size() << "\n";
            }
        });
    }
}
```

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

