#include <iostream>
#include "http_server.hpp"
#include "url_encode_decode.hpp"
using namespace cinatra;

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
	const int max_thread_num = 4;
	http_server server(max_thread_num);
	bool r = server.listen("0.0.0.0", "8080");
	if (!r) {
		LOG_INFO << "listen failed";
		return -1;
	}
	
	server.set_http_handler<GET, POST>("/", [](const request& req, response& res)->cinatra::res_content_type {
	    //auto query = req.get_query_value("zh");
        //std::cout<<code_utils::get_string_by_urldecode(query)<<std::endl;
        std::wstring w_abc = L"中文";
        std::wcout<<w_abc<<std::endl;
		res.set_status_and_content(status_type::ok, "<a>hello world</a>");
        return cinatra::res_content_type::string;
    });

	server.set_http_handler<GET, POST>("/gzip", [](const request& req, response& res) {
		auto body = req.body();
		std::cout << body.data() << std::endl;

		res.set_status_and_content(status_type::ok, "hello world", content_encoding::gzip);
	});

	server.run();
	return 0;
}