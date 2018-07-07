#include <iostream>
#include "http_server.hpp"
#include "http_client.hpp"
using namespace cinatra;

struct log_t
{
	bool before(const request& req, response& res) {
		std::cout << "before log" << std::endl;
		return true;
	}

	bool after(const request& req, response& res) {
		std::cout << "after log" << std::endl;
		res.add_header("aaaa", "bbcc");
		return true;
	}
};

struct check {
	bool before(const request& req, response& res) {
		/*std::cout << "before check" << std::endl;
		if (req.get_header_value("name").empty()) {
			res.set_status_and_content(status_type::bad_request);
			return false;
		}*/

		return true;
	}

	bool after(const request& req, response& res) {
		std::cout << "after check" << std::endl;

		return true;
	}
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
	server.set_http_handler<GET, POST>("/", [](const request& req, response& res) {
		res.set_status_and_content(status_type::ok, "hello world");
	},enable_cache{false});

	server.set_http_handler<GET, POST>("/login", [](const request& req, response& res) {
		auto session = res.start_session();
		session->set_data("userid", std::string("1"));
		session->set_max_age(-1);
		res.set_status_and_content(status_type::ok, "login");
	},enable_cache{false});

	server.set_http_handler<GET, POST>("/islogin", [](const request& req, response& res) {
		auto ptr = req.get_session();
		auto session = ptr.lock();
		if (session == nullptr || session->get_data<std::string>("userid") != "1") {
			res.set_status_and_content(status_type::ok, "没有登录", res_content_type::string);
			return;
		}
		res.set_status_and_content(status_type::ok, "已经登录", res_content_type::string);
	},enable_cache{false});

	server.set_http_handler<GET, POST>("/html", [](const request& req, response& res) {
		inja::json json;
        res.set_attr("number",1024);
        res.set_attr("test_text","hello,world");
        res.set_attr("header_text","你好 cinatra");
//		json["test_text"] = "hello,world";
//		json["header_text"] = "你好 cinatra";
		res.render_view("./www/test.html");
		/*
		 * ---------------------test.html---------------------------
		 * <html>
	<head>
	  <meta charset="utf-8">
	</head>
	<body>
		{% include "./header/header.html" %}
			<h1>{{test_text}}</h1>
	</body>
</html>

		 ----------------------------------header.html---------------------
		 <div>{{header_text}}</div>
*/
	});
//client 测试代码 begin
//	server.set_http_handler<GET,POST>("/test_multipart",[](const request& req, response& res){
//        http_client client("localhost:8060");
//        multipart_form form;
//        form.append("text","caaaaaddd");
//        form.append("file1",multipart_file("./test.png"));
//        auto client_res = client.request<POST>("/upload_small_file",form);
//		res.set_status_and_content(status_type::ok, client_res.get_content(),res_content_type::string);
//	});
//
//    server.set_http_handler<GET,POST>("/test_client",[](const request& req, response& res){
//        http_client client("localhost:8060");
//        client.add_header("name","007");
//        auto client_res = client.request<POST>("/test?id=1");
//        res.set_status_and_content(status_type::ok, client_res.get_content(),res_content_type::string);
//    });
//
//    server.set_http_handler<GET,POST>("/clientpost_json",[](const request& req, response& res){
//        http_client client("localhost:8060");
//        inja::json json;
//        json["success"] = true;
//        json["message"] = "just json test";
//        auto client_res = client.request<POST>("/parsejson",json.dump());
//        res.set_status_and_content(status_type::ok, client_res.get_content(),res_content_type::string);
//    });
//
//    server.set_http_handler<GET,POST>("/test_client_header",[](const request& req, response& res){
//        http_client client("localhost:8060");
//        auto client_res = client.request<POST>("/aspect");
//        auto header_pair_vec = client_res.get_header("aaaa");
//        std::cout<<header_pair_vec[0].second<<std::endl;
//        res.set_status_and_content(status_type::ok, std::string(header_pair_vec[0].second),res_content_type::string);
//    });
//
//    server.set_http_handler<GET,POST>("/test_multipart_asyn",[](const request& req, response& res){
//        http_client client("localhost:8060");
//        multipart_form form;
//        form.append("text","caaaaaddd");
//        form.append("file1",multipart_file("./test.png"));
//         client.request<POST>("/upload_small_file",form,[](const http_client::client_response& response,const http_client::error_code& error){
//             std::cout<<response.get_content()<<std::endl;
//         });
//        client.run();
//		res.set_status_and_content(status_type::ok, "OK",res_content_type::string);
//	});
//
//    server.set_http_handler<GET,POST>("/test_client_json_asyn",[](const request& req, response& res){
//        http_client client("localhost:8060");
//        inja::json json;
//        json["success"] = true;
//        json["message"] = "just json test";
//        client.request<POST>("/parsejson",json.dump(),[](const http_client::client_response& response,const http_client::error_code& error){
//            std::cout<<response.get_content()<<std::endl;
//        });
//        client.run();
//        res.set_status_and_content(status_type::ok, "OK",res_content_type::string);
//    });
//
//    server.set_http_handler<GET,POST>("/test_client_asyn",[](const request& req, response& res){
//        http_client client("localhost:8060");
//        client.add_header("name","007");
//        client.request<POST>("/test?id=1",[](const http_client::client_response& response,const http_client::error_code& error){
//             std::cout<<response.get_content()<<std::endl;
//        });
//        client.run();
//        res.set_status_and_content(status_type::ok, "OK",res_content_type::string);
//    });
//client 测试代码 end


	server.set_http_handler<GET, POST>("/json", [](const request& req, response& res) {
		inja::json json;
		json["abc"] = "abc";
		json["success"] = true;
		json["number"] = 100.005;
		json["name"] = "中文";
		res.render_json(json);
	}, enable_cache{ false });

	server.set_http_handler<GET, POST>("/parsejson", [](const request& req, response& res) {
		auto json_str = req.body();
		auto json = inja::json::parse(json_str);
		json["add_str"] = "parse_OK";
		res.render_json(json);
	}, enable_cache{ false });


	server.set_http_handler<GET,POST>("/redirect",[](const request& req, response& res){
		res.redirect("http://www.baidu.com"); // res.redirect("/json");
	});

	server.set_http_handler<GET, POST>("/pathinfo/*", [](const request& req, response& res) {
		auto s = req.get_query_value(0);
		res.set_status_and_content(status_type::ok, std::string(s.data(), s.length()),res_content_type::string);
	});

	server.set_http_handler<GET, POST>("/restype", [](const request& req, response& res) {
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

	server.set_http_handler<GET, POST>("/getzh", [](const request& req, response& res) {
		auto zh = req.get_query_value("zh");
        std::cout<<zh<<std::endl;
		res.set_status_and_content(status_type::ok, std::string(zh.data(),zh.size()), res_content_type::string);
	});

	server.set_http_handler<GET, POST>("/gzip", [](const request& req, response& res) {
		auto body = req.body();
		std::cout << body.data() << std::endl;

		res.set_status_and_content(status_type::ok, "hello world", res_content_type::none, content_encoding::gzip);
	});


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

	//aspect
	server.set_http_handler<GET, POST>("/aspect", [](const request& req, response& res) {
		res.set_status_and_content(status_type::ok, "hello world");
	}, check{}, log_t{});

	//web socket
	server.set_http_handler<GET, POST>("/ws", [](const request& req, response& res) {
		assert(req.get_content_type() == content_type::websocket);
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
			std::string str = std::string(part_data.data(), part_data.length());
			req.get_conn()->send_ws_msg(std::move(str), opcode::text);
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

	std::atomic_int n = 0;

	//http upload(multipart) for small file
	server.set_http_handler<GET, POST>("/upload_small_file", [&n](const request& req, response& res) {
		assert(req.get_content_type() == content_type::multipart);
        auto state = req.get_state();
        switch(state){
            case cinatra::data_proc_state::data_begin:
            {

            }
                break;
            case cinatra::data_proc_state::file_begin:
            {
                  //req.set_continue(false);  //不接受文件
            }
                break;
            case cinatra::data_proc_state::data_continue:
            {

            }
                break;
            case cinatra::data_proc_state::file_continue:
            {

            }
                break;
            case cinatra::data_proc_state::data_end:
            {

            }
                break;
            case cinatra::data_proc_state::file_end:
            {

            }
                break;
            case cinatra::data_proc_state::data_all_end:
            {
                auto text = req.get_query_value("text");
                std::cout<<text<<std::endl;
                auto& files = req.get_upload_files();
                for (auto& file : files) {
                    std::cout << file.get_file_path() << " " << file.get_file_size() << std::endl;
                }
                auto file_path = files.empty()==false?files[0].get_file_path():"";
                res.set_status_and_content(status_type::ok, std::move(file_path));
            }
                break;
            case cinatra::data_proc_state::data_error:
            {
                //network error
            }
            break;
        }
	});

	//http upload(octet-stream)
	server.set_http_handler<GET, POST>("/upload_octet_stream", [&n](const request& req, response& res) {
		assert(req.get_content_type() == content_type::octet_stream);
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

	//http download(chunked)
	server.set_http_handler<GET, POST>("/download_chunked", [](const request& req, response& res) {
		auto state = req.get_state();
		switch (state)
		{
		case cinatra::data_proc_state::data_begin:
		{
			std::string filename = "3.jpg";
			auto in = std::make_shared<std::ifstream>(filename, std::ios::binary);
			if (!in->is_open()) {
				req.get_conn()->on_close();
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
			const size_t len = 2 * 1024;
			str.resize(len);

			in->read(&str[0], len);
			size_t read_len = (size_t)in->gcount();
			if (read_len != len) {
				str.resize(read_len);
			}
			bool eof = (read_len == 0 || read_len != len);
			conn->write_chunked_data(std::move(str), eof);
		}
		break;
		case cinatra::data_proc_state::data_end:
		{
			std::cout << "chunked send finish" << std::endl;
			auto conn = req.get_conn();
			conn->on_close();
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