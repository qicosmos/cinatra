#include <iostream>
#include "http_server.hpp"
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
	nanolog::initialize(nanolog::GuaranteedLogger(), "/tmp/", "nanolog", 1);
	http_server server(std::thread::hardware_concurrency());
	bool r = server.listen("0.0.0.0", "8080");
	if (!r) {
		LOG_INFO << "listen failed";
		return -1;
	}

	server.set_http_handler<GET, POST>("/", [](const request& req, response& res) {
		res.set_status_and_content(status_type::ok, "hello world");
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

	//http download(chunked)
	server.set_http_handler<GET, POST>("/download_chunked", [](const request& req, response& res) {
		auto state = req.get_state();
		switch (state)
		{
		case cinatra::data_proc_state::data_begin:
		{
			std::string filename = "2.xml";
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
			size_t read_len = (size_t)in->gcount();
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