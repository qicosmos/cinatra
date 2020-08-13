# cinatra--一个高效易用的c++ http框架

<p align="center">
  <a href="https://github.com/qicosmos/cinatra/tree/master/lang/english">English</a> | <span>中文</span>
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
cinatra是世界上性能最好的http服务器之一，性能测试详见[性能测试](#性能测试)

## 谁在用cinatra

cinatra目前被很多公司在使用，在这里可以看到[谁在用cinatra](https://github.com/qicosmos/cinatra/wiki/%E8%B0%81%E5%9C%A8%E7%94%A8cinatra).

# 如何使用

## 编译依赖
cinatra是基于boost.asio开发的，所以需要boost库。不过，cinatra同时也支持了ASIO_STANDALONE，你不必一定需要boost库。

cinatra需要的依赖项：

1. C++17 编译器 (gcc 7.2, clang 4.0, Visual Studio 2017 update 15.5,或者更高的版本)
2. Boost.Asio(或者独立的 Asio)
3. Boost.System

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

### 同步发get/post消息
同步和异步发送接口都是返回response_data，它有4个字段分别是：网络错误码、http状态码、返回的消息、返回的header。
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

### 异步发get/post消息

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

### 文件上传

异步multipart文件上传。

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


### 文件下载

提供了两个异步chunked下载接口，一个是直接下载到文件，一个是chunk回调给用户，由用户自己去处理下载的chunk数据
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


# 性能测试
## 测试用例：

![qps](lang/qps.png "qps")

![qps-pipeline](lang/qps-pipeline.png "qps-pipeline")

# 注意事项

websocket的业务函数是会多次进入的，因此写业务逻辑的时候需要注意，推荐按照示例中的方式去做。


# 联系方式

purecpp@163.com

qq群：340713904

[http://purecpp.org/](http://purecpp.org/ "purecpp")

[https://github.com/qicosmos/cinatra](https://github.com/qicosmos/cinatra "cinatra")




