#include <iostream>
#include "../include/cinatra.hpp"

using namespace cinatra;

struct log_t
{
	bool before(request& req, response& res) {
		std::cout << "before log" << std::endl;
		return true;
	}

	bool after(request& req, response& res) {
		std::cout << "after log" << std::endl;
		res.add_header("aaaa", "bbcc");
		return true;
	}
};

struct person
{
	void foo(request& req, response& res) 
	{
		std::cout << i << std::endl;
		res.render_string("ok");
	}

	void foo1(request& req, response& res)
	{
		std::cout << i << std::endl;
		res.render_string("ok");
	}

	int i=0;
};

void test_ssl_server(){
#ifdef CINATRA_ENABLE_SSL
    //you should open macro CINATRA_ENABLE_SSL at first
    http_ssl_server server(2);

    server.set_ssl_conf({ "server.crt", "server.key" });
    int r = server.listen("0.0.0.0", "9001");
    if (r < 0) {
        return;
    }

    server.set_http_handler<GET, POST>("/", [](request& req, response& res) {
        auto str = req.get_conn<cinatra::SSL>()->remote_address();
        res.set_status_and_content(status_type::ok, "hello world from 9001");
    });

    server.run();
#endif
}

void test_client() {

    //http
    auto s1 = get("baidu.com");
    auto s2 = post("baidu.com", "your post content");

    auto client = cinatra::client_factory::instance().new_client<cinatra::NonSSL>("baidu.com", "http");
    auto request_str = client->send_msg<cinatra::TEXT, 3000, cinatra::GET>("/", "");
    std::cout << request_str << "\n";

    {
        size_t timeout_second = 5;
        auto client = cinatra::client_factory::instance().new_client<cinatra::NonSSL>("192.168.96.14", "8090", timeout_second);
        client->async_send_msg("/", "", [](auto ec, auto data) {
            if (ec) {
                std::cout << ec.message() << "\n";
                return;
            }

            std::cout << data << "\n";
        });

        std::getchar();
    }

    //https
#ifdef CINATRA_ENABLE_SSL
    auto s3 = get<cinatra::SSL>("baidu.com/");
    auto s4 = post<cinatra::SSL>("baidu.com/hello", "your post content");

    {
        auto client = cinatra::client_factory::instance().new_client<cinatra::SSL>("baidu.com", "https");
        auto request_str = client->send_msg<cinatra::TEXT, 30000, cinatra::POST>("/", "");
        std::cout << request_str << "\n";
    }
#endif
}

int main() {
//    test_client();
    //test_ssl_server();
	http_server server(std::thread::hardware_concurrency());
	bool r = server.listen("0.0.0.0", "8090");
	if (!r) {
		//LOG_INFO << "listen failed";
		return -1;
	}

	server.enable_http_cache(false);//set global cache
    server.set_res_cache_max_age(86400);
	server.set_cache_max_age(5);
	server.set_http_handler<GET, POST>("/", [](request& req, response& res) {
		res.set_status_and_content(status_type::ok,"hello world");
	});

	server.set_http_handler<GET>("/plaintext", [](request& req, response& res) {
		//res.set_status_and_content<status_type::ok, res_content_type::string>("Hello, World!");
		res.set_status_and_content(status_type::ok, "Hello, World!", res_content_type::string);
	});

    //server.set_http_handler<GET, POST>("/delay", [](request& req, response& res) {
    //    auto conn = req.get_conn<NonSSL>();

    //    //monitor an async operation to test response delay
    //    std::thread thd([conn, &res] {
    //        std::this_thread::sleep_for(std::chrono::seconds(3));
    //        if (!conn->has_close()) {
    //            res.set_status_and_content(status_type::ok, "hello world");
    //            conn->response_now();
    //        }
    //        else {
    //            std::cout << "has closed\n";
    //        }
    //    });
    //    thd.detach();
    //    res.set_delay(true);
    //});

//	person p{ 2 };
//	server.set_http_handler<GET, POST>("/a", &person::foo, enable_cache{ false }, log_t{});
////	server.set_http_handler<GET, POST>("/b", &person::foo1, log_t{}, enable_cache{ false });
//
//    server.set_http_handler<GET, POST>("/string", [](request& req, response& res) {
//        res.render_string(std::to_string(std::time(nullptr)));
//    },enable_cache{true});
//
//    server.set_http_handler<GET, POST>("/404", [](request& req, response& res) {
//        res.render_404();
//    },enable_cache{false});
//
//    server.set_http_handler<GET, POST>("/404_custom", [](request& req, response& res) {
//        res.render_404("./404.html");
//    },enable_cache{false});
//
//	server.set_http_handler<GET, POST>("/login", [](request& req, response& res) {
//		auto session = res.start_session();
//		session->set_data("userid", std::string("1"));
//		session->set_max_age(-1);
//		res.set_status_and_content(status_type::ok, "login");
//	},enable_cache{false});
//
//	server.set_http_handler<GET, POST>("/islogin", [](request& req, response& res) {
//		auto ptr = req.get_session();
//		auto session = ptr.lock();
//		if (session == nullptr || session->get_data<std::string>("userid") != "1") {
//			res.set_status_and_content(status_type::ok, "没有登录", res_content_type::string);
//			return;
//		}
//		res.set_status_and_content(status_type::ok, "已经登录", res_content_type::string);
//	},enable_cache{false});
//
//	server.set_http_handler<GET, POST>("/html", [](request& req, response& res) {
//        res.set_attr("number",1024);
//        res.set_attr("test_text","hello,world");
//        res.set_attr("header_text","你好 cinatra");
//		res.render_view("./www/test.html");
//	});
//
//	server.set_http_handler<GET, POST,OPTIONS>("/json", [](request& req, response& res) {
//		nlohmann::json json;
//        res.add_header("Access-Control-Allow-Origin","*");
//		if(req.get_method()=="OPTIONS"){
//            res.add_header("Access-Control-Allow-Headers","Authorization");
//            res.render_string("");
//		}else{
//            json["abc"] = "abc";
//            json["success"] = true;
//            json["number"] = 100.005;
//            json["name"] = "中文";
//            json["time_stamp"] = std::time(nullptr);
//            res.render_json(json);
//		}
//	});
//
//	server.set_http_handler<GET,POST>("/redirect",[](request& req, response& res){
//		res.redirect("http://www.baidu.com"); // res.redirect("/json");
//	});
//
//	server.set_http_handler<GET, POST>("/pathinfo/*", [](request& req, response& res) {
//		auto s = req.get_query_value(0);
//		res.render_string(std::string(s.data(), s.length()));
//	});
//
//	server.set_http_handler<GET, POST>("/restype", [](request& req, response& res) {
//		auto type = req.get_query_value("type");
//		auto res_type = cinatra::res_content_type::string;
//		if (type == "html")
//		{
//			res_type = cinatra::res_content_type::html;
//		}
//		else if (type == "json") {
//			res_type = cinatra::res_content_type::json;
//		}
//		else if (type == "string") {
//			//do not anything;
//		}
//		res.set_status_and_content(status_type::ok, "<a href='http://www.baidu.com'>hello world 百度</a>", res_type);
//	});
//
//	server.set_http_handler<GET, POST>("/getzh", [](request& req, response& res) {
//		auto zh = req.get_query_value("zh");
//		res.render_string(std::string(zh.data(),zh.size()));
//	});
//
//	server.set_http_handler<GET, POST>("/gzip", [](request& req, response& res) {
//		auto body = req.body();
//		std::cout << body.data() << std::endl;
//		res.set_status_and_content(status_type::ok, "hello world", res_content_type::none, content_encoding::gzip);
//	});
//
//
//	server.set_http_handler<GET, POST>("/test", [](request& req, response& res) {
//		auto name = req.get_header_value("name");
//		if (name.empty()) {
//			res.render_string("no name");
//			return;
//		}
//
//		auto id = req.get_query_value("id");
//		if (id.empty()) {
//			res.render_404();
//			return;
//		}
//		res.render_string("hello world");
//	});
//
//	//aspect
//	server.set_http_handler<GET, POST>("/aspect", [](request& req, response& res) {
//		res.render_string("hello world");
//	}, check{}, log_t{});
//
	////web socket
	//server.set_http_handler<GET, POST>("/ws", [](request& req, response& res) {
	//	assert(req.get_content_type() == content_type::websocket);

	//	req.on(ws_open, [](request& req){
	//		std::cout << "websocket start" << std::endl;
	//	});

	//	req.on(ws_message, [](request& req) {
	//		auto part_data = req.get_part_data();
	//		//echo
	//		std::string str = std::string(part_data.data(), part_data.length());
	//		req.get_conn()->send_ws_string(std::move(str));
	//		std::cout << part_data.data() << std::endl;
	//	});

	//	req.on(ws_error, [](request& req) {
	//		std::cout << "websocket pack error or network error" << std::endl;
	//	});
	//});

//	server.set_http_handler<GET, POST>("/vue_html", [](request& req, response& res) {
//		res.render_raw_view("./www/index.html");
//	});
//
//	server.set_http_handler<GET, POST>("/vue_demo", [](request& req, response& res) {
//		res.render_raw_view("./www/dist/index.html");
//	});
//
//	//http upload(multipart)
//	server.set_http_handler<GET, POST>("/upload_multipart", [](request& req, response& res) {
//		assert(req.get_content_type() == content_type::multipart);
//		auto& files = req.get_upload_files();
//		for (auto& file : files) {
//			std::cout << file.get_file_path() << " " << file.get_file_size() << std::endl;
//		}
//		res.render_string("multipart finished");
//	});
//
//	//http upload(octet-stream)
//	server.set_http_handler<GET, POST>("/upload_octet_stream", [](request& req, response& res) {
//		assert(req.get_content_type() == content_type::octet_stream);
//		auto& files = req.get_upload_files();
//		for (auto& file : files) {
//			std::cout << file.get_file_path() << " " << file.get_file_size() << std::endl;
//		}
//		res.render_string("octet-stream finished");
//	});

	//chunked download
	//http://127.0.0.1:8080/assets/show.jpg
	//cinatra will send you the file, if the file is big file(more than 5M) the file will be downloaded by chunked
	server.run();
	return 0;
}