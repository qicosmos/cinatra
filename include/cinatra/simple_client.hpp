#pragma once
#include <string>
#include <unordered_map>
#include <future>
#include "use_asio.hpp"
#include "response_parser.hpp"
#include "define.h"
#include "response_cv.hpp"
#include "utils.hpp"
#include "mime_types.hpp"

namespace cinatra {
	//short connection
	class simple_client : public std::enable_shared_from_this<simple_client> {
	public:
		simple_client(boost::asio::io_service& io_context, std::string addr, std::string port) : ios_(io_context),
			socket_(io_context), resolver_(io_context), addr_(std::move(addr)), port_(std::move(port)) {
		}

		template<res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT=3000, http_method METHOD = POST>
		std::string send_msg(std::string api, std::string msg) {
			build_message<METHOD, CONTENT_TYPE>(std::move(api), std::move(msg));

			promis_ = std::promise<std::string>();
			std::future<std::string> future = promis_.get_future();

			boost::asio::ip::tcp::resolver::query query(addr_, port_);
			auto self = this->shared_from_this();
			resolver_.async_resolve(query, [this, self](boost::system::error_code ec, const boost::asio::ip::tcp::resolver::iterator& it) {
				if (ec) {
					std::cout << ec.message() << std::endl;
					return;
				}

				boost::asio::async_connect(socket_, it, [this, self](boost::system::error_code ec, const boost::asio::ip::tcp::resolver::iterator&) {
					if (!ec) {
						do_read();

						do_write();
					}
					else {
						std::cout << ec.message() << std::endl;
						close();
					}
				});
			});

			//ios_.run();

			auto status = future.wait_for(std::chrono::milliseconds(TIMEOUT));
			if (status == std::future_status::timeout || status == std::future_status::deferred) {
				throw std::out_of_range("timeout or deferred");
			}

			return future.get();
		}

		template<res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT = 3000, http_method METHOD = POST>
		void async_send_msg(std::string api, std::string msg, std::function<void()> error_callback = [] {}) {
			build_message<METHOD, CONTENT_TYPE>(std::move(api), std::move(msg));
			boost::asio::ip::tcp::resolver::query query(addr_, port_);
			auto self = this->shared_from_this();
			resolver_.async_resolve(query, [this, self, callback = std::move(error_callback)](boost::system::error_code ec, 
				const boost::asio::ip::tcp::resolver::iterator& it) {
				if (ec) {
					std::cout << ec.message() << std::endl;
					callback();
					return;
				}

				boost::asio::async_connect(socket_, it, [this, self, callback = std::move(callback)](boost::system::error_code ec,
					const boost::asio::ip::tcp::resolver::iterator&) {
					if (!ec) {
						do_read();

						do_write(std::move(callback));
					}
					else {
						std::cout << ec.message() << std::endl;
						callback();
						close();
					}
				});
			});
		}

		void add_header(std::string key, std::string value) {
			headers_.emplace_back(std::move(key), std::move(value));
		}

		void set_url_preifx(std::string prefix) {
			prefix_ = std::move(prefix);
		}

		//set http version to 1.0, default 1.1
		void set_version() {
			version_ = " HTTP/1.0\r\n";
		}

		std::string_view get_header_value(std::string_view key) {
			return parser_.get_header_value(key);
		}

		void upload_file(std::string api, std::string filename, std::function<void(boost::system::error_code ec)> error_callback) {
			file_.open(filename, std::ios::binary);
			if (!file_) {
				error_callback(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
				return;
			}

			file_extension_ = std::filesystem::path(filename).extension().string();
			std::error_code ec;
			size_t size = std::filesystem::file_size(filename, ec);
			if (ec || size == 0) {
				error_callback(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
				return;
			}

			file_size_ = size;

			prefix_ = build_head<http_method::POST, res_content_type::multipart>(api, size + 148+ file_extension_.size());
			api_ = std::move(api);
			boost::asio::ip::tcp::resolver::query query(addr_, port_);
			auto self = this->shared_from_this();
			resolver_.async_resolve(query, [this, self, callback = std::move(error_callback)](boost::system::error_code ec,
				const boost::asio::ip::tcp::resolver::iterator& it) {
				if (ec) {
					std::cout << ec.message() << std::endl;
					callback(ec);
					return;
				}

				boost::asio::async_connect(socket_, it, [this, self, callback = std::move(callback)](boost::system::error_code ec,
					const boost::asio::ip::tcp::resolver::iterator&) {
					if (!ec) {
						do_read();

						do_write_file(std::move(callback));
					}
					else {
						callback(ec);
						close();
					}
				});
			});
		}

		void on_progress(std::function<void(std::string)> progress) {
			progress_cb_ = std::move(progress);
		}
	private:
		template<http_method METHOD, res_content_type CONTENT_TYPE>
		void build_message(std::string api, std::string msg) {
			std::string prefix = build_head<METHOD, CONTENT_TYPE>(std::move(api), std::move(msg));
			prefix.append(std::move(msg));
			write_message_ = std::move(prefix);
		}

		template<http_method METHOD, res_content_type CONTENT_TYPE>
		std::string build_head(std::string api, size_t content_length) {
			std::string method = get_method_str<METHOD>();
			std::string prefix = method.append(" ").append(std::move(prefix_)).append(std::move(api)).append(std::move(version_));
			auto host = get_inner_header_value("Host");
			if (host.empty()) {
				add_header("Host", addr_);
			}

			auto content_type = get_inner_header_value("content-type");
			if (content_type.empty())
				build_content_type<CONTENT_TYPE>();

			if (content_type.find("application/x-www-form-urlencoded") == std::string_view::npos) {
				auto conent_length = get_inner_header_value("content-length");
				if (conent_length.empty())
					build_content_length(content_length);
			}

			prefix.append(build_headers()).append("\r\n");
			return prefix;
		}

		std::string build_multipart_binary(std::string content) {
			if (prefix_.empty()) {
				return content;
			}

			std::string body;
			body.append("--" + boundary_ + CRLF);
			body.append("Content-Disposition: form-data; name=\"" + std::string("test") + "\"; filename=\"" + std::string("filename") + file_extension_ + "\"" + CRLF);
			body.append(CRLF);
			body.append(std::move(content));
			
			return body;
		}

		void multipart_end(std::string& body) {
			body.append(CRLF);
			body.append("--" + boundary_ + "--" + CRLF);
			body.append(CRLF);
		}

		std::string_view get_inner_header_value(std::string_view key) {
			for (auto& head : headers_) {
				if (iequal(head.first.data(), head.first.size(), key.data()))
					return std::string_view(head.second);
			}

			return {};
		}

		template<http_method METHOD>
		std::string get_method_str() {
			return std::string{ method_name(METHOD) };
		}

		template<res_content_type CONTENT_TYPE>
		void build_content_type() {
			if (CONTENT_TYPE != res_content_type::none) {
				auto iter = res_mime_map.find(CONTENT_TYPE);
				if (iter != res_mime_map.end()) {
					if (CONTENT_TYPE == res_content_type::multipart) {
						auto str = std::string(iter->second);
						str += boundary_;
						add_header("Content-Type", std::move(str));
					}
					else {
						add_header("Content-Type", std::string(iter->second));
					}					
				}
			}
		}

		std::string build_headers() {
			std::string str;
			for (auto& h : headers_) {
				str.append(h.first);
				str.append(name_value_separator);
				str.append(h.second);
				str.append(crlf);
			}

			return str;
		}

		void build_content_length(size_t content_len) {
			char temp[20] = {};
			itoa_fwd((int)content_len, temp);
			add_header("Content-Length", temp);
		}

		void do_write(std::function<void()> error_callback = []{}) {
			auto self = this->shared_from_this();
			boost::asio::async_write(socket_, boost::asio::buffer(write_message_.data(), write_message_.length()),
				[this, self, error_callback = std::move(error_callback)](boost::system::error_code ec, std::size_t length) {
				if (!ec) {
					std::cout << "send ok " << std::endl;
				}
				else {
					std::cout << "send failed " << ec.message() << std::endl;
					error_callback();
					close();
				}
			});
		}

		//true:file end, false:not file end, should be continue
		bool make_file_data(std::function<void(boost::system::error_code ec)>& error_callback) {
			bool eof = file_.peek() == EOF;
			if (eof) {
				file_.close();
				error_callback({});
				return true;
			}

			std::string content;
			const size_t size = 3 * 1024 * 1024;
			content.resize(size);
			file_.read(&content[0], size);
			int64_t read_len = (int64_t)file_.gcount();
			assert(read_len > 0);
			eof = file_.peek() == EOF;

			if (read_len < size) {
				content.resize(read_len);
			}

			content = build_multipart_binary(content);
			if (eof) {
				multipart_end(content);
			}

			if (!prefix_.empty()) {
				write_message_ = std::move(prefix_);
				write_message_ += std::move(content);
			}
			else {
				write_message_ = std::move(content);
			}

			return false;
		}

		void do_write_file(std::function<void(boost::system::error_code ec)> error_callback) {
			if (make_file_data(error_callback)) {
				return;
			}
			
			auto self = this->shared_from_this();
			boost::asio::async_write(socket_, boost::asio::buffer(write_message_.data(), write_message_.length()),
				[this, self, error_callback = std::move(error_callback)](boost::system::error_code ec, std::size_t length) {
				if (!ec) {
					writed_size_ += length;
					double persent = (double)writed_size_ / file_size_;
					progress_cb_(std::to_string(persent));
					do_write_file(error_callback);
				}
				else {
					std::cout << "send failed " << ec.message() << std::endl;
					error_callback(ec);
					close();
				}
			});
		}

		void close() {
			auto self = this->shared_from_this();
			ios_.dispatch([this, self] {
#ifdef _DEBUG
				std::cout << "close" << std::endl;
#endif				
				boost::system::error_code ec;
				socket_.close(ec);
			});
		}

		void do_read() {
			auto self = this->shared_from_this();
			socket_.async_read_some(boost::asio::buffer(parser_.buffer(), parser_.left_size()),
				[this, self](boost::system::error_code ec, std::size_t bytes_transferred) {
				if (!ec) {
					auto last_len = parser_.current_size();
					bool at_capacity = parser_.update_size(bytes_transferred);
					if (at_capacity) {
						set_response_msg("out of range from local server");
						return;
					}

					int ret = parser_.parse((int)last_len);
					if (ret == -2) {
						do_read();
					}
					else if (ret == -1) {//error
						set_response_msg("parse response error from local server");
					}
					else {
						if (parser_.total_len() > MAX_RESPONSE_SIZE) {
							set_response_msg("response message too long, more than " + std::to_string(MAX_RESPONSE_SIZE)+" from local server");
							return;
						}

						if (parser_.has_body()) {
							//if total>maxsize return
							if (parser_.has_recieved_all())
								handle_response();
							else
								do_read_body();
						}
						else {
							handle_response();
						}
					}
				}
				else {
					close();
				}
			});
		}

		void handle_response() {
			if (parser_.status() != 200) {
				//send sms faild
#ifdef _DEBUG
				std::cout << parser_.message() << " " << parser_.body() << std::endl;
#endif
			}

			promis_.set_value(std::string(parser_.body()));

			close();
		}

		void set_response_msg(std::string msg) {
			promis_.set_value(std::move(msg));
			close();
		}

		void do_read_body() {
			auto self = this->shared_from_this();
			boost::asio::async_read(socket_, boost::asio::buffer(parser_.buffer(), parser_.total_len() - parser_.current_size()),
				[this, self](boost::system::error_code ec, std::size_t length) {
				if (ec) {
					close();
					return;
				}

				handle_response();
			});
		}

		boost::asio::io_service& ios_;
		std::string addr_;
		std::string port_;
		boost::asio::ip::tcp::resolver resolver_;
		boost::asio::ip::tcp::socket socket_;
		std::string write_message_;
		response_parser parser_;
		std::vector<std::pair<std::string, std::string>> headers_;

		std::string prefix_;
		std::promise<std::string> promis_;
		std::string version_ = " HTTP/1.1\r\n";

		std::string boundary_ = "--CinatraBoundary2B8FAF4A80EDB307";
		std::string CRLF = "\r\n";
		std::string multipart_str_;
		std::string api_;
		std::ifstream file_;
		std::string file_extension_;
		size_t file_size_ = 0;
		size_t writed_size_ = 0;
		std::function<void(std::string)> progress_cb_;
	};
}