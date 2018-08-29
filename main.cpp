#include <iostream>
#include "http_server.hpp"
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

struct check {
	bool before(request& req, response& res) {
		std::cout << "before check" << std::endl;
		if (req.get_header_value("name").empty()) {
			res.render_404();
			return false;
		}

		return true;
	}

	bool after(request& req, response& res) {
		std::cout << "after check" << std::endl;

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

int main() {
	nanolog::initialize(nanolog::GuaranteedLogger(), "/tmp/", "nanolog", 1);
	const int max_thread_num = 4;
	http_server server(max_thread_num);
#ifdef CINATRA_ENABLE_SSL
	server.init_ssl_context(true, [](auto, auto) { return "123456"; }, "server.crt", "server.key", "dh1024.pem");
	bool r = server.listen("0.0.0.0", "https");
#else
	bool r = server.listen("0.0.0.0", "8080");
#endif
	if (!r) {
		LOG_INFO << "listen failed";
		return -1;
	}

    server.set_base_path("base_path","/feather");
	server.enable_http_cache(false);//set global cache
    server.set_res_cache_max_age(86400);
	server.set_cache_max_age(5);
	server.set_http_handler<GET, POST>("/", [](request& req, response& res) {
		res.set_status_and_content(status_type::ok,"hello world");
	},enable_cache{false});

	person p{ 2 };
	server.set_http_handler<GET, POST>("/a", &person::foo, enable_cache{ false }, log_t{});
//	server.set_http_handler<GET, POST>("/b", &person::foo1, log_t{}, enable_cache{ false });

    server.set_http_handler<GET, POST>("/string", [](request& req, response& res) {
        res.render_string(std::to_string(std::time(nullptr)));
    },enable_cache{true});

    server.set_http_handler<GET, POST>("/404", [](request& req, response& res) {
        res.render_404();
    },enable_cache{false});

    server.set_http_handler<GET, POST>("/404_custom", [](request& req, response& res) {
        res.render_404("./404.html");
    },enable_cache{false});

	server.set_http_handler<GET, POST>("/login", [](request& req, response& res) {
		auto session = res.start_session();
		session->set_data("userid", std::string("1"));
		session->set_max_age(30);
		res.set_status_and_content(status_type::ok, "login");
	},enable_cache{false});

	server.set_http_handler<GET, POST>("/islogin", [](request& req, response& res) {
		auto ptr = req.get_session();
		auto session = ptr.lock();
		if (session == nullptr || session->get_data<std::string>("userid") != "1") {
			res.set_status_and_content(status_type::ok, "没有登录", res_content_type::string);
			return;
		}
		res.set_status_and_content(status_type::ok, "已经登录", res_content_type::string);
	},enable_cache{false});


	server.set_http_handler<GET, POST>("/html", [](request& req, response& res) {
        res.set_attr("number",1024);
        res.set_attr("test_text","hello,world");
        res.set_attr("header_text","你好 cinatra");
		res.render_view("./www/test.html");
	});

	server.set_http_handler<GET, POST,OPTIONS>("/json", [](request& req, response& res) {
		nlohmann::json json;
        res.add_header("Access-Control-Allow-Origin","*");
		if(req.get_method()=="OPTIONS"){
            res.add_header("Access-Control-Allow-Headers","Authorization");
            res.render_string("");
		}else{
            json["abc"] = "abc";
            json["success"] = true;
            json["number"] = 100.005;
            json["name"] = "中文";
            json["time_stamp"] = std::time(nullptr);
            res.render_json(json);
		}
	});

	server.set_http_handler<GET,POST>("/redirect",[](request& req, response& res){
		res.redirect("http://www.baidu.com"); // res.redirect("/json");
	});

	server.set_http_handler<GET, POST>("/pathinfo/*", [](request& req, response& res) {
		auto s = req.get_query_value(0);
		res.render_string(std::string(s.data(), s.length()));
	});

	server.set_http_handler<GET, POST>("/restype", [](request& req, response& res) {
		auto type = req.get_query_value("type");
		auto res_type = cinatra::res_content_type::string;
		if (type == "html")
		{
			res_type = cinatra::res_content_type::html;
		}
		else if (type == "json") {
			res_type = cinatra::res_content_type::json;
		}
		else if (type == "string") {
			//do not anything;
		}
		res.set_status_and_content(status_type::ok, "<a href='http://www.baidu.com'>hello world 百度</a>", res_type);
	});

	server.set_http_handler<GET, POST>("/getzh", [](request& req, response& res) {
		auto zh = req.get_query_value("zh");
		res.render_string(std::string(zh.data(),zh.size()));
	});

	server.set_http_handler<GET, POST>("/gzip", [](request& req, response& res) {
		auto body = req.body();
		std::cout << body.data() << std::endl;
		res.set_status_and_content(status_type::ok, "hello world", res_content_type::none, content_encoding::gzip);
	});


	server.set_http_handler<GET, POST>("/test", [](request& req, response& res) {
		auto name = req.get_header_value("name");
		if (name.empty()) {
			res.render_string("no name");
			return;
		}

		auto id = req.get_query_value("id");
		if (id.empty()) {
			res.render_404();
			return;
		}
		res.render_string("hello world");
	});

	//aspect
	server.set_http_handler<GET, POST>("/aspect", [](request& req, response& res) {
		res.render_string("hello world");
	}, check{}, log_t{});

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

	//http upload(multipart)
	server.set_http_handler<GET, POST>("/upload_multipart", [](request& req, response& res) {
		assert(req.get_content_type() == content_type::multipart);
		auto text = req.get_query_value("text");
		std::cout<<text<<std::endl;
		auto& files = req.get_upload_files();
		for (auto& file : files) {
			std::cout << file.get_file_path() << " " << file.get_file_size() << std::endl;
		}
		res.render_string("multipart finished");
	});

	//http upload(octet-stream)
	server.set_http_handler<GET, POST>("/upload_octet_stream", [](request& req, response& res) {
		assert(req.get_content_type() == content_type::octet_stream);
		auto& files = req.get_upload_files();
		for (auto& file : files) {
			std::cout << file.get_file_path() << " " << file.get_file_size() << std::endl;
		}
		res.render_string("octet-stream finished");
	});

	//chunked download
	//http://127.0.0.1:8080/assets/show.jpg
	//cinatra will send you the file, if the file is big file(more than 5M) the file will be downloaded by chunked
	server.run();
	return 0;
}