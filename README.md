# cinatra--一个高效易用的c++ http框架

<p align="center">
  <a href="https://github.com/qicosmos/cinatra/tree/master/lang/english">English</a> | <span>中文</span>
</p>

| OS (Compiler Version)                          | Status                                                                                                   |
|------------------------------------------------|----------------------------------------------------------------------------------------------------------|
| Ubuntu 22.04 (clang 14.0.0)                    | ![win](https://github.com/qicosmos/cinatra/actions/workflows/linux_clang.yml/badge.svg?branch=master) |
| Ubuntu 22.04 (gcc 11.2.0)                      | ![win](https://github.com/qicosmos/cinatra/actions/workflows/linux_gcc.yml/badge.svg?branch=master)   |
| macOS Monterey 12 (AppleClang 14.0.0.14000029) | ![win](https://github.com/qicosmos/cinatra/actions/workflows/mac.yml/badge.svg?branch=master)         |
| Windows Server 2022 (MSVC 19.33.31630.0)       | ![win](https://github.com/qicosmos/cinatra/actions/workflows/windows.yml/badge.svg?branch=master)     |

# 目录

## [使用cinatra常见问题汇总(FAQ)](https://github.com/qicosmos/cinatra/wiki)

* [cinatra简介](#cinatra简介)
* [如何使用](#如何使用)
* [快速示例](#快速示例)
* [性能测试](#性能测试)
* [注意事项](#注意事项)
* [roadmap](#roadmap)
* [联系方式](#联系方式)

# cinatra简介
[cinatra](https://github.com/qicosmos/cinatra)是一个高性能易用的http框架，它是用modern c++(c++20)开发的，它的目标是提供一个快速开发的c++ http框架。它的主要特点如下：

1. 统一而简单的接口
2. header-only
3. 跨平台
4. 高效
5. 支持面向切面编程

cinatra目前支持了http1.1/1.0, ssl和websocket, 你可以用它轻易地开发一个http服务器，比如常见的数据库访问服务器、文件上传下载服务器、实时消息推送服务器，你也可以基于cinatra开发一个mqtt服务器。
cinatra是世界上性能最好的http服务器之一，性能测试详见[性能测试](#性能测试)

除此之外，cinatra 还提供了一个基于C++20 协程的http(https) client，包括普通get/post请求、文件上传下载和web socket、redirect、proxy等功能。

## 谁在用cinatra

cinatra目前被很多公司在使用，在这里可以看到[谁在用cinatra](https://github.com/qicosmos/cinatra/wiki/%E8%B0%81%E5%9C%A8%E7%94%A8cinatra).

# 如何使用

## 编译器版本

1. C++20 编译器 (gcc 10.2, clang 13, Visual Studio 2022,或者更高的版本)

## 使用
cinatra是header-only的，直接引用头文件既可。


# 快速示例

## 示例1：一个简单的hello world

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

5行代码就可以实现一个简单http服务器了，用户不需要关注多少细节，直接写业务逻辑就行了。

## 示例2：展示如何取header和query以及错误返回

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

## 示例3：面向切面的http服务器

	#include "cinatra.hpp"
	using namespace cinatra;

	//日志切面
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
	
	//校验的切面
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

	//将信息从中间件传输到处理程序
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
本例中有两个切面，一个校验http请求的切面，一个是日志切面，这个切面用户可以根据需求任意增加。本例会先检查http请求的合法性，如果不合法就会返回bad request，合法就会进入下一个切面，即日志切面，日志切面会打印出一个before表示进入业务逻辑之前的处理，业务逻辑完成之后会打印after表示业务逻辑结束之后的处理。

## 示例4：文件上传
cinatra目前支持了multipart和octet-stream格式的上传。

### multipart文件上传

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

短短几行代码就可以实现一个http文件上传的服务器了，包含了异常处理和错误处理。

### octet-stream文件上传

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

## 示例5：文件下载

    cinatra提供下载功能非常简单，不需要编写代码，具体方法：
    1. 启动cinatra server
    2. 将要下载的文件放到http server同一级的www目录下即可。
    3. 如何下载：如果你把test.txt放到www之后，那么直接通过http://127.0.0.1:8090/test.txt下载即可。
	//chunked download
	//cinatra will send you the file, if the file is big file(more than 5M) the file will be downloaded by chunked. support continues download

## 示例6：websocket

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

## 示例7：io_service_inplace
本代码演示如何使用io_service_inplace，然后自己控制http server的运行线程以及循环。
使用 [http://[::1]:8080/close] （IPv6） 或者 [http://127.0.0.1:8080/close] (IPv4) 来关闭http server。

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

## cinatra客户端使用
### sync_send get/post message

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

### async get/post message

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

### upload(multipart) file
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

### download file(ranges and chunked)

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

### web socket
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

  bool r = co_await client.async_connect("ws://localhost:8090/ws");
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


# 性能测试
## 测试用例：

![qps](lang/qps.png "qps")

![qps-pipeline](lang/qps-pipeline.png "qps-pipeline")

# 注意事项

websocket的业务函数是会多次进入的，因此写业务逻辑的时候需要注意，推荐按照示例中的方式去做。


# 联系方式

purecpp@163.com

qq群：545605838

[http://purecpp.cn/](http://purecpp.cn/ "purecpp")

[https://github.com/qicosmos/cinatra](https://github.com/qicosmos/cinatra "cinatra")




