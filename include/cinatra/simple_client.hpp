#pragma once
#include <string>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <future>
#include "use_asio.hpp"
#include "response_parser.hpp"
#include "define.h"
#include "response_cv.hpp"
#include "utils.hpp"
#include "mime_types.hpp"

#ifdef CINATRA_ENABLE_SSL
#include <boost/asio/ssl.hpp>
#endif

namespace cinatra {
	//short connection
	class simple_client : public std::enable_shared_from_this<simple_client> {
	public:
#ifdef CINATRA_ENABLE_SSL
		simple_client(boost::asio::io_service& io_context, std::string addr, std::string port, boost::asio::ssl::context& context,
			size_t timeout = 30) : ios_(io_context),
			socket_(io_context, context), resolver_(io_context), addr_(std::move(addr)),
			port_(std::move(port)), timer_(io_context), timeout_seconds_(timeout), chunked_size_buf_(10) {
			socket_.set_verify_mode(boost::asio::ssl::verify_peer);
			socket_.set_verify_callback([this](auto b, auto& ctx) { return verify_certificate(b, ctx); });
			chunk_body_.resize(chunk_buf_len + 4);
		}
#else
		simple_client(boost::asio::io_service& io_context, std::string addr, std::string port, size_t timeout = 30) : ios_(io_context),
			socket_(io_context), resolver_(io_context), addr_(std::move(addr)),
			port_(std::move(port)), timer_(io_context), timeout_seconds_(timeout), chunked_size_buf_(10) {
			chunk_body_.resize(chunk_buf_len + 4);
		}
#endif

		~simple_client() {
			close();
		}
		
		template<res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT=3000, http_method METHOD = POST>
		std::string send_msg(std::string api, std::string msg) {
			build_message<METHOD, CONTENT_TYPE>(std::move(api), std::move(msg));

			promis_ = std::make_unique<std::promise<std::string>>();
			std::future<std::string> future = promis_->get_future();

			boost::asio::ip::tcp::resolver::query query(addr_, port_);
			auto self = this->shared_from_this();
			resolver_.async_resolve(query, [this, self](boost::system::error_code ec, const boost::asio::ip::tcp::resolver::iterator& it) {
				if (ec) {
					std::cout << ec.message() << std::endl;
					return;
				}

				boost::asio::async_connect(socket(), it, [this, self](boost::system::error_code ec, const boost::asio::ip::tcp::resolver::iterator&) {
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
			build_message<METHOD, CONTENT_TYPE>(std::move(api), msg);
			boost::asio::ip::tcp::resolver::query query(addr_, port_);
			auto self = this->shared_from_this();
			resolver_.async_resolve(query, [this, self, callback = std::move(error_callback)](boost::system::error_code ec, 
				const boost::asio::ip::tcp::resolver::iterator& it) {
				if (ec) {
					std::cout << ec.message() << std::endl;
					callback();
					return;
				}

				boost::asio::async_connect(socket(), it, [this, self, callback = std::move(callback)](boost::system::error_code ec,
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

		void send_form_data(std::string api, std::vector<std::pair<std::string, std::string>> v, 
			std::function<void()> error_callback) {
			build_form_data(std::move(v));

			prefix_ = build_head<http_method::POST, res_content_type::multipart>(api, total_multipart_size());
			write_message_ = std::move(prefix_) + std::move(multipart_start_) + std::move(multipart_end_);

			api_ = std::move(api);
			boost::asio::ip::tcp::resolver::query query(addr_, port_);
			auto self = this->shared_from_this();
			resolver_.async_resolve(query, [this, self, callback = std::move(error_callback)](boost::system::error_code ec,
				const boost::asio::ip::tcp::resolver::iterator& it) {
				if (ec) {
					std::cout << ec.message() << std::endl;
					callback();
					return;
				}

				boost::asio::async_connect(socket(), it, [this, self, callback = std::move(callback)](boost::system::error_code ec,
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

		void upload_file(std::string api, std::string filename, std::function<void(boost::system::error_code ec)> error_callback) {
			upload_file(std::move(api), std::move(filename), 0, std::move(error_callback));
		}

		void upload_file(std::string api, std::string filename, size_t start, std::function<void(boost::system::error_code ec)> error_callback) {
			if (file_.is_open()) {
				error_callback(boost::asio::error::make_error_code(boost::asio::error::in_progress));
				return;
			}

			file_.open(filename, std::ios::binary);
			if (!file_) {
				error_callback(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
				return;
			}

			std::error_code ec;
			size_t size = std::filesystem::file_size(filename, ec);
			if (ec || size == 0 || start == -1) {
				file_.close();
				error_callback(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
				return;
			}

			file_extension_ = std::filesystem::path(filename).extension().string();
			if (start > 0) {
				file_.seekg(start);
			}			

			multipart_file_start();
			file_size_ = size - start;

			prefix_ = build_head<http_method::POST, res_content_type::multipart>(api, total_multipart_size());
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

				boost::asio::async_connect(socket(), it, [this, self, callback = std::move(callback)](boost::system::error_code ec,
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

		template<http_method METHOD = GET>
		void download_file(std::string filename, std::string resoure_path, std::function<void(boost::system::error_code ec)> error_callback) {
			download_file<METHOD>("", std::move(filename), std::move(resoure_path), std::move(error_callback));
		}

		template<http_method METHOD = GET>
		void download_file(std::string dir, std::string filename, std::string resoure_path, std::function<void(boost::system::error_code ec)> error_callback) {
			if (!check_file(std::move(dir), std::move(filename))) {
				error_callback(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
				return;
			}

			build_download_request<METHOD>(std::move(resoure_path));

			boost::asio::ip::tcp::resolver::query query(addr_, port_);
			auto self = this->shared_from_this();
			resolver_.async_resolve(query, [this, self, callback = std::move(error_callback)](boost::system::error_code ec,
				const boost::asio::ip::tcp::resolver::iterator& it) {
				if (ec) {
					std::cout << ec.message() << std::endl;
					callback(ec);
					return;
				}

				boost::asio::async_connect(socket(), it, [this, self, callback = std::move(callback)](boost::system::error_code ec,
					const boost::asio::ip::tcp::resolver::iterator&) {
					if (!ec) {
#ifdef CINATRA_ENABLE_SSL
						handshake(std::move(callback));
#else
						read_chunk(callback);
						do_write(callback);
#endif
						
					}
					else {
						std::cout << ec.message() << std::endl;
						callback(ec);
						close();
					}
				});
			});
		}

		void on_length(std::function<void(size_t)> on_length) {
			on_length_ = std::move(on_length);
		}

		void on_data(std::function<void(std::string_view)> on_data) {
			on_data_ = std::move(on_data);
		}

	private:
		template<http_method METHOD, res_content_type CONTENT_TYPE>
		void build_message(std::string api, std::string msg) {
			std::string prefix = build_head<METHOD, CONTENT_TYPE>(std::move(api), msg.size());
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
			total_write_size_ = prefix.size() + total_multipart_size();
			return prefix;
		}

		std::string build_multipart_binary(std::string content) {
			if (prefix_.empty()) {
				return content;
			}

			return multipart_start_ + std::move(content);			
		}

		void multipart_file_start() {
			multipart_start_.append("--" + boundary_ + CRLF);
			multipart_start_.append("Content-Disposition: form-data; name=\"" + std::string("test") + "\"; filename=\"" + std::string("filename") + file_extension_ + "\"" + CRLF);
			multipart_start_.append(CRLF);
		}

		void build_form_data(std::vector<std::pair<std::string, std::string>> v) {
			size_t size = v.size();
			for (size_t i = 0; i < v.size(); i++) {
				multipart_start_.append("--" + boundary_ + CRLF);
				multipart_start_.append("Content-Disposition: form-data; name=\"" + std::move(v[i].first) + "\"" + CRLF);
				multipart_start_.append(CRLF);
				multipart_start_.append(std::move(v[i].second));
				if (i < size - 1) {
					multipart_start_.append(CRLF);
				}				
			}			
		}

		void multipart_end(std::string& body) {
			body.append(multipart_end_);
		}

		size_t total_multipart_size() {
			return file_size_ + multipart_start_.size() + multipart_end_.size();
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

		void do_write(std::function<void(boost::system::error_code)> error_callback) {
			auto self = this->shared_from_this();
			boost::asio::async_write(socket_, boost::asio::buffer(write_message_.data(), write_message_.length()),
				[this, self, error_callback = std::move(error_callback)](boost::system::error_code ec, std::size_t length) {
				if (!ec) {
					std::cout << "send ok " << std::endl;
				}
				else {
					std::cout << "send failed " << ec.message() << std::endl;
					error_callback(ec);
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
			
			reset_timer();
			auto self = this->shared_from_this();
			boost::asio::async_write(socket_, boost::asio::buffer(write_message_.data(), write_message_.length()),
				[this, self, error_callback = std::move(error_callback)](boost::system::error_code ec, std::size_t length) {
				if (!ec) {
					cancel_timer();
					writed_size_ += length;
					assert(writed_size_ <= total_write_size_);
					double persent = (double)writed_size_ / total_write_size_;
					progress_callback(persent);
					
					do_write_file(std::move(error_callback));
				}
				else {
					std::cout << "send failed " << ec.message() << std::endl;
					error_callback(ec);
					close();
				}
			});
		}

		void close() {
			if (has_close_) {
				return;
			}

#ifdef _DEBUG
			std::cout << "close" << std::endl;
#endif				
			boost::system::error_code ec;
#ifdef CINATRA_ENABLE_SSL
			socket_.shutdown(ec);
#else
			socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
#endif
			socket().close(ec);

			has_close_ = true;
		}

#ifdef CINATRA_ENABLE_SSL
		bool verify_certificate(bool preverified,
			boost::asio::ssl::verify_context& ctx) {
			// The verify callback can be used to check whether the certificate that is
			// being presented is valid for the peer. For example, RFC 2818 describes
			// the steps involved in doing this for HTTPS. Consult the OpenSSL
			// documentation for more details. Note that the callback is called once
			// for each certificate in the certificate chain, starting from the root
			// certificate authority.

			// In this example we will simply print the certificate's subject name.
			char subject_name[256];
			X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
			X509_NAME_oneline(X509_get_subject_name(cert), subject_name, 256);
			std::cout << "Verifying " << subject_name << "\n";

			return true || preverified;
		}

		void handshake(std::function<void(boost::system::error_code)> callback) {
			socket_.async_handshake(boost::asio::ssl::stream_base::client,
				[this, callback = std::move(callback)](const boost::system::error_code& ec) {
				if (!ec) {
					read_chunk(callback);
					do_write(callback);
				}
				else {
					std::cout << ec.message() << std::endl;
					callback(ec);
					close();
				}
			});
		}
#endif

		tcp_socket& socket() {
#ifdef CINATRA_ENABLE_SSL
			return socket_.next_layer();
#else
			return socket_;
#endif
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

			if (promis_) {
				promis_->set_value(std::string(parser_.body()));
			}			

			close();
		}

		void set_response_msg(std::string msg) {
			if (promis_) {
				promis_->set_value(std::move(msg));
			}
			
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

		void read_chunk(std::function<void(boost::system::error_code ec)> error_callback) {
			reset_timer();
			auto self = this->shared_from_this();
			boost::asio::async_read_until(socket_, read_head_, "\r\n\r\n",
				[this, self, callback = std::move(error_callback)](boost::system::error_code ec, std::size_t bytes_transferred) {
				if (ec) {
					chunked_file_.close();
					return;
				}

				cancel_timer();
				boost::asio::streambuf::const_buffers_type bufs = read_head_.data();
				std::string_view line(boost::asio::buffer_cast<const char*>(bufs), bytes_transferred);
				int ret = parser_.parse(line.data(), line.size(), 0);
				if (ret < 0|| parser_.status() != 200) {//error
					chunked_file_.close();
					callback(boost::asio::error::make_error_code(boost::asio::error::not_found));
					return;
				}

				std::string_view part_body(boost::asio::buffer_cast<const char*>(bufs) + bytes_transferred, boost::asio::buffer_size(bufs) - bytes_transferred);
				
				if (parser_.has_length()) {
					left_chunk_len_ = parser_.body_len();
					if (!part_body.empty()) {
						left_chunk_len_ -= part_body.size();
						write_chunked_data(part_body);						
					}

					if (left_chunk_len_ >= chunk_buf_len) {
						read_stream_body(chunk_buf_len, std::move(callback));
					}
					else {
						read_stream_body(left_chunk_len_, std::move(callback));
					}
					return;
				}

				if (!parser_.is_chunked()) {
					chunked_file_.close();
					callback(boost::asio::error::make_error_code(boost::asio::error::operation_not_supported));
					return;
				}

				handle_chunked(part_body, std::move(callback));
			});			
		}

		void handle_chunked(std::string_view content, std::function<void(boost::system::error_code ec)> error_callback) {
			if (content.empty()) {
				read_chunk_head(std::move(error_callback));
				return;
			}

			struct phr_chunked_decoder dec = { 0 };
			size_t size = content.size();
			std::string body(content);
			auto ret = phr_decode_chunked(&dec, body.data(), &size);
			if (ret == -1) {
				chunked_file_.close();
				error_callback(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
				return;
			}

			if (ret == 0) { //read all chunked data				
				bool r = write_chunked_data0(content);
				chunked_file_.close();
				if (!r) {
					error_callback(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
					return;
				}
				error_callback({});
				return;
			}

			if (dec._state == 0) {
				if (dec.bytes_left_in_chunk == 0) {
					read_chunk_head(std::move(error_callback));
				}
				else {
					part_chunked_size_ = to_hex_string(dec.bytes_left_in_chunk);
					read_chunk_head(std::move(error_callback));
				}
			}
			else if (dec._state == 2) {
				while (true) {
					auto pos = content.find("\r\n");
					if (pos == std::string_view::npos) {
						chunked_file_.close();
						error_callback(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
						return;
					}

					std::string len_sv(content.data(), pos);
					left_chunk_len_ = hex_to_int(len_sv);
					if (left_chunk_len_ < 0) {
						chunked_file_.close();
						error_callback(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
						return;
					}

					auto left = content.size() - pos - 2;
					if (left_chunk_len_ == left) {
						write_chunked_data({ content.data() + pos + 2, left });
						left_chunk_len_ = 0;
						//read_clcf()-->read_head()
						read_crcf(2, std::move(error_callback));
						break;
					}
					else if (left_chunk_len_ > left) {
						write_chunked_data({ content.data() + pos + 2, left });
						left_chunk_len_ -= left;
						if (left_chunk_len_ >= chunk_buf_len) {
							read_chunk_body(chunk_buf_len + 2, false, std::move(error_callback));
						}
						else {
							read_chunk_body(left_chunk_len_ + 2, false, std::move(error_callback));
						}
						break;
					}
					else if (left_chunk_len_ < left) {
						write_chunked_data({ content.data() + pos + 2, (size_t)left_chunk_len_ });
						auto len = left - left_chunk_len_;
						if (len == 1) {
							read_crcf(1, std::move(error_callback));
							break;
						}
						else if (len == 2) {
							read_chunk_head(std::move(error_callback));
							break;
						}
						else {
							content = { content.data()+ left_chunk_len_+2, (size_t)(content.size() - left_chunk_len_ - 2) };
						}
					}
				}
			}
			else if (dec._state == 3) {
				if (content.back() == '\r') {
					write_chunked_data0({ content.data(), content.size() - 1 });
					read_crcf(1, std::move(error_callback));
				}
				else {
					write_chunked_data0(content);
					read_crcf(2, std::move(error_callback));
				}
			}
			else {
				std::cout <<"not support now! " << content<< "\n";
				chunked_file_.close();
				error_callback(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
				return;
			}
		}

		void read_crcf(size_t count, std::function<void(boost::system::error_code ec)> error_callback) {
			auto self = this->shared_from_this();
			boost::asio::async_read(socket_, boost::asio::buffer(crcf_, count), [this, self, callback = std::move(error_callback)]
			(boost::system::error_code ec, std::size_t length) {
				read_chunk_head(std::move(callback));
			});
		}

		bool write_chunked_data0(std::string_view content) {
			while (true) {
				size_t pos = content.find("\r\n");
				if (pos == std::string_view::npos) {
					break;
				}

				std::string len_sv(content.data(), pos);
				auto chunk_len = hex_to_int(len_sv);
				if (chunk_len < 0) {
					return false;
				}

				if (chunk_len == 0) {
					break;
				}

				std::string_view body = { content.data() + pos + 2, (size_t)chunk_len };
				write_chunked_data(body);
				size_t offset = pos + 2 + chunk_len + 2;
				content = { content.data() + offset, content.size() - offset };				
			}
			return true;
		}

		void read_chunk_head(std::function<void(boost::system::error_code ec)> error_callback) {
			reset_timer();
			auto self = this->shared_from_this();
			boost::asio::async_read_until(socket_, chunked_size_buf_, "\r\n",
				[this, self, callback = std::move(error_callback)](boost::system::error_code ec, std::size_t bytes_transferred) {
				if (ec) {
					chunked_file_.close();
					callback(ec);
					return;
				}

				cancel_timer();
				boost::asio::streambuf::const_buffers_type bufs = chunked_size_buf_.data();
				std::string line(boost::asio::buffer_cast<const char*>(bufs), bytes_transferred - 2);
				if (!part_chunked_size_.empty()) {
					line = std::move(part_chunked_size_) + std::move(line);
				}

				left_chunk_len_ = hex_to_int(line);
				if (left_chunk_len_ < 0) {
					chunked_file_.close();
					callback(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
					return;
				}

				if (left_chunk_len_ == 0) {
					chunked_file_.close();
					callback({});
					return;
				}

				std::string_view part_body(boost::asio::buffer_cast<const char*>(bufs) + bytes_transferred, boost::asio::buffer_size(bufs) - bytes_transferred);
				if (part_body.size() > left_chunk_len_) {
					std::string_view chunk_data(part_body.data(), left_chunk_len_);
					write_chunked_data({ chunk_data.data(), chunk_data.size() });
					std::string_view left_data(part_body.data() + left_chunk_len_ + 2, part_body.length() - left_chunk_len_ - 2);
					if (left_data.size() == 5) { //"\r\n0\r\n"
						chunked_file_.close();
						callback({});
						return;
					}

					read_chunk_body(5-left_data.size(), true, std::move(callback));
					return;
				}

				write_chunked_data({ part_body.data(), part_body.size() });
				left_chunk_len_ -= part_body.size();
				chunked_size_buf_.consume(chunked_size_buf_.size()+1);

				if (left_chunk_len_ < 0) {
					chunked_file_.close();
					callback(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
					return;
				}

				if (left_chunk_len_ >= chunk_buf_len) {					
					read_chunk_body(chunk_buf_len+2, false, std::move(callback));
				}
				else {
					read_chunk_body(left_chunk_len_+2, false, std::move(callback));
				}				
			});
		}

		void read_chunk_body(int64_t read_len, bool eof, std::function<void(boost::system::error_code ec)> error_callback) {
			reset_timer();
			auto self = this->shared_from_this();
			boost::asio::async_read(socket_, boost::asio::buffer(chunk_body_.data(), read_len),
				[this, eof, self, callback = std::move(error_callback)](boost::system::error_code ec, std::size_t length) {
				if (ec) {
					chunked_file_.close();
					return;
				}

				cancel_timer();
				if (eof) {
					chunked_file_.close();
					callback({});
					return;
				}

				size_t cur_chunk_len = length - 2;
				left_chunk_len_ -= cur_chunk_len;
				assert(left_chunk_len_ >= 0);
				if (left_chunk_len_ == 0) {
					if (cur_chunk_len > 0) {
						write_chunked_data({ chunk_body_.data(), cur_chunk_len });
					}
					
					read_chunk_head(std::move(callback));
					return;
				}

				if (left_chunk_len_ >= chunk_buf_len) {
					read_chunk_body(chunk_buf_len+2, false, std::move(callback));
				}
				else {
					read_chunk_body(left_chunk_len_+2, false, std::move(callback));
				}
			});
		}

		void read_stream_body(int64_t read_len, std::function<void(boost::system::error_code ec)> error_callback) {
			reset_timer();
			auto self = this->shared_from_this();
			boost::asio::async_read(socket_, boost::asio::buffer(chunk_body_.data(), read_len),
				[this, self, callback = std::move(error_callback)](boost::system::error_code ec, std::size_t length) {
				if (ec) {
					chunked_file_.close();
					callback(ec);
					return;
				}

				cancel_timer();

				left_chunk_len_ -= length;
				assert(left_chunk_len_ >= 0);
				if (left_chunk_len_ < 0) {
					chunked_file_.close();
					callback(boost::asio::error::make_error_code(boost::asio::error::invalid_argument));
					return;
				}
				write_chunked_data({ chunk_body_.data(), length });
				if (left_chunk_len_ == 0) {					
					chunked_file_.close();
					callback({});
					return;
				}

				if (left_chunk_len_ >= chunk_buf_len) {
					read_stream_body(chunk_buf_len, std::move(callback));
				}
				else {
					read_stream_body(left_chunk_len_, std::move(callback));
				}
			});
		}

		void write_chunked_data(std::string_view chunked_data) {
			if (on_length_) {
				on_length_(chunked_data.size());
			}

			if (on_data_) {
				on_data_(chunked_data);
			}
			else {
				chunked_file_.write(chunked_data.data(), chunked_data.length());
			}			
		}

		bool check_file(std::string dir, std::string filename) {
			std::string prefix;
			if(!dir.empty()){
				std::error_code code;
				std::filesystem::create_directories(dir, code);
				if (code) {
					return false;
				}

				prefix = dir + "/";
			}

			chunked_file_.open(prefix + filename, std::ios::binary);
			return chunked_file_.is_open();
		}

		int64_t hex_to_int(std::string_view s) {
			char* p;
			int64_t n = strtoll(s.data(), &p, 16);
			if (*p != 0) {
				return -1;
			}

			return n;
		}

		template<http_method METHOD>
		void build_download_request(std::string resoure_path) {
			std::string method = get_method_str<METHOD>();
			std::string prefix = method.append(" ").append(std::move(resoure_path)).append(std::move(version_));
			auto host = get_inner_header_value("Host");
			if (host.empty()) {
				add_header("Host", addr_);
			}
			prefix.append(build_headers()).append("\r\n");
			write_message_ = std::move(prefix);
		}

		void progress_callback(double persent) {
			if (progress_cb_) {
				char buff[20];
				snprintf(buff, sizeof(buff), "%0.2f", persent*100);
				std::string str = buff;
				progress_cb_(std::move(str));
			}			
		}


		void reset_timer() {
			if (timeout_seconds_ == 0) { 
				return; 
			}

			auto self(this->shared_from_this());
			timer_.expires_from_now(std::chrono::seconds(timeout_seconds_));
			timer_.async_wait([this, self](const boost::system::error_code& ec) {
				if (ec) {
					return;
				}

				//LOG(INFO) << "rpc connection timeout";
				close();
				if (chunked_file_.is_open()) {
					chunked_file_.close();
				}
			});
		}

		void cancel_timer() {
			if (timeout_seconds_ == 0) { 
				return; 
			}

			timer_.cancel();
		}

		boost::asio::io_service& ios_;
		std::string addr_;
		std::string port_;
		boost::asio::ip::tcp::resolver resolver_;
		Socket socket_;
		std::string write_message_;
		response_parser parser_;
		std::vector<std::pair<std::string, std::string>> headers_;

		std::string prefix_;
		std::unique_ptr<std::promise<std::string>> promis_ = nullptr;
		std::string version_ = " HTTP/1.1\r\n";

		std::string boundary_ = "--CinatraBoundary2B8FAF4A80EDB307";
		std::string CRLF = "\r\n";
		std::string multipart_start_;
		std::string multipart_end_ = CRLF + "--" + boundary_ + "--" + CRLF + CRLF;
		std::string api_;
		std::ifstream file_;
		std::string file_extension_;
		size_t file_size_ = 0;
		size_t writed_size_ = 0;
		size_t total_write_size_ = 0;
		std::function<void(std::string)> progress_cb_;

		const int chunk_buf_len = 3 * 1024 * 1024;
		std::string chunk_body_;
		int64_t left_chunk_len_;

		std::string part_chunked_size_;
		boost::asio::streambuf read_head_;
		boost::asio::streambuf chunked_size_buf_;
		char crcf_[2];
		std::ofstream chunked_file_;
		std::function<void(size_t)> on_length_ = nullptr;
		std::function<void(std::string_view)> on_data_ = nullptr;

		boost::asio::steady_timer timer_;
		std::size_t timeout_seconds_;

		bool has_close_ = false;
	};
}