# cinatra

# 目录

* [cinatra简介](#cinatra简介)
* [如何使用](#如何使用)
* [快速示例](#快速示例)
* [性能测试](#性能测试)
* [注意事项](#注意事项)
* [roadmap](#roadmap)
* [联系方式](#联系方式)

# cinatra简介
cinatra是一个高性能易用的http框架，它是用modern c++(c++17)开发的，它的目标是提供一个快速开发的c++ http框架。它的主要特点如下：

1. 统一而简单的接口
2. header-only
3. 跨平台
4. 高效
5. 支持面向切面编程

cinatra目前支持了http1.1/1.0和websocket, 你可以用它轻易地开发一个http服务器，比如常见的数据库访问服务器、文件上传下载服务器、实时消息推送服务器。

# 如何使用

## 编译依赖
cinatra是基于boost.asio开发的，所以需要boost库，同时也需要支持c++17的编译器，依赖项：

1. boost.asio
2. c++17编译器(gcc7.2,clang4.0, vs2017 update15.5)

## 使用
cinatra是header-only的，直接引用头文件既可。


# 快速示例

## 示例1：一个简单的hello world

	#include "http_server.hpp"
	using namespace cinatra;
	
	int main() {
		http_server server(std::thread::hardware_concurrency());
		server.listen("0.0.0.0", "8080");
		server.set_http_handler<GET, POST>("/", [](const request& req, response& res) {
			res.set_status_and_content(status_type::ok, "hello world");
		});

		server.run();
		return 0;
	}

5行代码就可以实现一个简单http服务器了，用户不需要关注多少细节，直接写业务逻辑就行了。

## 示例2：展示如何取header和query以及错误返回

	#include "http_server.hpp"
	using namespace cinatra;
	
	int main() {
		http_server server(std::thread::hardware_concurrency());
		server.listen("0.0.0.0", "8080");
		server.set_http_handler<GET, POST>("/test", [](const request& req, response& res) {
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

	#include "http_server.hpp"
	using namespace cinatra;

	//日志切面
	struct log_t
	{
		bool before(const request& req, response& res) {
			std::cout << "before log" << std::endl;
			return true;
		}
	
		bool after(const request& req, response& res) {
			std::cout << "after log" << std::endl;
			return true;
		}
	};
	
	//校验的切面
	struct check {
		bool before(const request& req, response& res) {
			std::cout << "before check" << std::endl;
			if (req.get_header_value("name").empty()) {
				res.set_status_and_content(status_type::bad_request);
				return false;
			}
			
			return true;
		}
	
		bool after(const request& req, response& res) {
			std::cout << "after check" << std::endl;
			return true;
		}
	};
	
	int main() {
		http_server server(std::thread::hardware_concurrency());
		server.listen("0.0.0.0", "8080");
		server.set_http_handler<GET, POST>("/aspect", [](const request& req, response& res) {
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
	#include "http_server.hpp"
	using namespace cinatra;
	
	int main() {
		http_server server(std::thread::hardware_concurrency());
		server.listen("0.0.0.0", "8080");

		std::atomic_int n = 0;
		//http upload(multipart)
		server.set_http_handler<GET, POST>("/upload_multipart", [&n](const request& req, response& res) {
			assert(req.get_http_type() == http_type::multipart);
			auto state = req.get_state();
			switch (state)
			{
			case cinatra::data_proc_state::data_begin:
			{
				auto file_name_s = req.get_multipart_file_name();
				auto extension = get_extension(file_name_s);
				
				std::string file_name = std::to_string(n++) + std::string(extension.data(), extension.length());
				auto file = std::make_shared<std::ofstream>(file_name, std::ios::binary);
				if (!file->is_open()) {
					res.set_continue(false);
					return;
				}
				req.get_conn()->set_tag(file);
			}
			break;
			case cinatra::data_proc_state::data_continue:
			{
				if (!res.need_continue()) {
					return;
				}
	
				auto file = std::any_cast<std::shared_ptr<std::ofstream>>(req.get_conn()->get_tag());
				auto part_data = req.get_part_data();
				file->write(part_data.data(), part_data.length());
			}
			break;
			case cinatra::data_proc_state::data_end:
			{
				std::cout << "one file finished" << std::endl;
			}
			break;
			case cinatra::data_proc_state::data_all_end:
			{
				//all the upstream end
				std::cout << "all files finished" << std::endl;
				res.set_status_and_content(status_type::ok);
			}
			break;
			case cinatra::data_proc_state::data_error:
			{
				//network error
			}
			break;
			}
		});

		server.run();
		return 0;
	}

短短几行代码就可以实现一个http文件上传的服务器了，包含了异常处理和错误处理。

### octet-stream文件上传

	#include <atomic>
	#include "http_server.hpp"
	using namespace cinatra;
	
	int main() {
		http_server server(std::thread::hardware_concurrency());
		server.listen("0.0.0.0", "8080");

		std::atomic_int n = 0;
		//http upload(octet-stream)
		server.set_http_handler<GET, POST>("/upload_octet_stream", [&n](const request& req, response& res) {
			assert(req.get_http_type() == http_type::octet_stream);
			auto state = req.get_state();
			switch (state)
			{
			case cinatra::data_proc_state::data_begin:
			{
				std::string file_name = std::to_string(n++);;
				auto file = std::make_shared<std::ofstream>(file_name, std::ios::binary);
				if (!file->is_open()) {
					res.set_continue(false);
					return;
				}
				req.get_conn()->set_tag(file);
			}
			break;
			case cinatra::data_proc_state::data_continue:
			{
				if (!res.need_continue()) {
					return;
				}
	
				auto file = std::any_cast<std::shared_ptr<std::ofstream>>(req.get_conn()->get_tag());
				auto part_data = req.get_part_data();
				file->write(part_data.data(), part_data.length());
			}
			break;
			case cinatra::data_proc_state::data_end:
			{
				std::cout << "one file finished" << std::endl;
			}
			break;
			case cinatra::data_proc_state::data_error:
			{
				//network error
			}
			break;
			}
		});

		server.run();
		return 0;
	}

## 示例5：文件下载

	#include "http_server.hpp"
	using namespace cinatra;
	
	int main() {
		http_server server(std::thread::hardware_concurrency());
		server.listen("0.0.0.0", "8080");

		//http download(chunked)
		server.set_http_handler<GET, POST>("/download_chunked", [](const request& req, response& res) {
			auto state = req.get_state();
			switch (state)
			{
			case cinatra::data_proc_state::data_begin:
			{
				std::string filename = "2.jpg";
				auto in = std::make_shared<std::ifstream>(filename, std::ios::binary);
				if (!in->is_open()) {
					req.get_conn()->on_error();
					return;
				}
	
				auto conn = req.get_conn();
				conn->set_tag(in);
				auto extension = get_extension(filename.data());
				auto mime = get_mime_type(extension);
				conn->write_chunked_header(mime);
			}
			break;
			case cinatra::data_proc_state::data_continue:
			{
				auto conn = req.get_conn();
				auto in = std::any_cast<std::shared_ptr<std::ifstream>>(conn->get_tag());
				
				std::string str;
				const size_t len = 2*1024;
				str.resize(len);
	
				in->read(&str[0], len);
				size_t read_len = in->gcount();
				if (read_len != len) {
					str.resize(read_len);
				}
				bool eof = (read_len==0|| read_len != len);
				conn->write_chunked_data(std::move(str), eof);
			}
			break;
			case cinatra::data_proc_state::data_end:
			{
				std::cout << "chunked send finish" << std::endl;
			}
			break;
			case cinatra::data_proc_state::data_error:
			{
				//network error
			}
			break;
			}
		});

		server.run();
		return 0;
	}

## 示例6：websocket

	#include "http_server.hpp"
	using namespace cinatra;
	
	int main() {
		http_server server(std::thread::hardware_concurrency());
		server.listen("0.0.0.0", "8080");

		//web socket
		server.set_http_handler<GET, POST>("/ws", [](const request& req, response& res) {
			assert(req.get_http_type() == http_type::websocket);
			auto state = req.get_state();
			switch (state)
			{
			case cinatra::data_proc_state::data_begin:
			{
				std::cout << "websocket start" << std::endl;
			}
			break;
			case cinatra::data_proc_state::data_continue:
			{
				auto part_data = req.get_part_data();
				//echo
				req.get_conn()->send_ws_msg(std::string(part_data.data(), part_data.length()));
				std::cout << part_data.data() << std::endl;
			}
			break;
			case cinatra::data_proc_state::data_close:
			{
				std::cout << "websocket close" << std::endl;
			}
			break;
			case cinatra::data_proc_state::data_error:
			{
				std::cout << "network error" << std::endl;
			}
			break;
			}
		});

		server.run();
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

1. 支持ssl
2. 支持断点续传
3. 支持session和cookie
4. 接口优化、性能优化

我希望cinatra有越来越多的人使用并喜欢它，也希望在在使用过程中越来越完善，变成一个强大易用、快速开发的http框架，欢迎大家积极参与cinatra项目，可以提issue也可以发邮件提建议，也可以提pr，形式不限。

# 联系方式

purecpp@163.com

[http://purecpp.org/](http://purecpp.org/ "purecpp")

[https://github.com/qicosmos/cinatra](https://github.com/qicosmos/cinatra "cinatra")




