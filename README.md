# cinatra--一个高效易用的c++ http框架

<p align="center">
  <a href="https://github.com/alvarotrigo/fullPage.js/tree/master/lang/english">English</a> | <span>中文</span>
</p>

# 目录

* [cinatra简介](#cinatra简介)
* [如何使用](#如何使用)
* [快速示例](#快速示例)
* [性能测试](#性能测试)
* [注意事项](#注意事项)
* [roadmap](#roadmap)
* [联系方式](#联系方式)

# cinatra简介
[cinatra](https://github.com/qicosmos/cinatra)是一个高性能易用的http框架，它是用modern c++(c++17)开发的，它的目标是提供一个快速开发的c++ http框架。它的主要特点如下：

1. 统一而简单的接口
2. header-only
3. 跨平台
4. 高效
5. 支持面向切面编程

cinatra目前支持了http1.1/1.0, ssl和websocket, 你可以用它轻易地开发一个http服务器，比如常见的数据库访问服务器、文件上传下载服务器、实时消息推送服务器，你也可以基于cinatra开发一个mqtt服务器。

# 如何使用

## 编译依赖
cinatra是基于boost.asio开发的，所以需要boost库。不过，cinatra同时也支持了ASIO_STANDALONE，你不必一定需要boost库。

cinatra需要支持c++17的编译器，依赖项：

1. boost.asio
2. c++17编译器(gcc7.2,clang4.0, vs2017 update15.5)

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
	
	int main() {
		http_server server(std::thread::hardware_concurrency());
		server.listen("0.0.0.0", "8080");
		server.set_http_handler<GET, POST>("/aspect", [](request& req, response& res) {
			res.set_status_and_content(status_type::ok, "hello world");
		}, check{}, log_t{});

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

	//chunked download
	//http://127.0.0.1:8080/assets/show.jpg
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


# 性能测试
## 测试用例：

ab测试：ab -c100 -n5000 127.0.0.1:8080/

服务器返回一个hello。

在一个8核心16G的云主机上测试，qps在9000-13000之间。

## 对比测试
通过ab测试和boost.beast做对比，二者qps相当，大概是因为二者都是基于boost.asio开发的的原因。cinatra目前还没做专门的性能优化，还有提升空间。


# 注意事项

文件上传下载，websocket的业务函数是会多次进入的，因此写业务逻辑的时候需要注意，推荐按照示例中的方式去做。

cinatra目前刚开始在生产环境中使用, 还处于开发完善阶段，可能还有一些bug，因此不建议现阶段直接用于生产环境，建议先在测试环境下试用。

试用没问题了再在生产环境中使用，试用过程中发现了问题请及时提issue反馈或者邮件联系我。

测试和使用稳定之后cinatra会发布正式版。

# roadmap

1. 增加一个基本的client用于server之间的通信

我希望有越来越多的人使用cinatra并喜欢它，也希望cinatra在使用过程中越来越完善，变成一个强大易用、快速开发的http框架，欢迎大家积极参与cinatra项目，可以提issue也可以发邮件提建议，也可以提pr，形式不限。

这次重构的cinatra几乎是重写了一遍，代码比之前的少了30%以上，接口统一了，http和业务分离，具备更好的扩展性和可维护性。

# 联系方式

purecpp@163.com

qq群：340713904

[http://purecpp.org/](http://purecpp.org/ "purecpp")

[https://github.com/qicosmos/cinatra](https://github.com/qicosmos/cinatra "cinatra")




