#pragma once
#include "use_asio.hpp"
#include <vector>
#include <cassert>
#include <mutex>
#include <any>
#include "request.hpp"
#include "response.hpp"
#include "websocket.hpp"
#include "define.h"
#include "http_cache.hpp"
#include "uuid.h"

namespace cinatra {
	using http_handler = std::function<void(request&, response&)>;
	using send_ok_handler = std::function<void()>;
	using send_failed_handler = std::function<void(const boost::system::error_code&)>;

	template <typename socket_type>
	class connection :public std::enable_shared_from_this<connection<socket_type>>, private noncopyable {
	public:
		explicit connection(boost::asio::io_service& io_service, std::size_t max_req_size, long keep_alive_timeout,
			http_handler& handler, std::string& static_dir, std::function<bool(request& req, response& res)>* upload_check
#ifdef CINATRA_ENABLE_SSL
			, boost::asio::ssl::context& ctx
#endif
		)
			:
#ifdef CINATRA_ENABLE_SSL
			socket_(io_service, ctx),
#else
			socket_(io_service),
#endif
			MAX_REQ_SIZE_(max_req_size), KEEP_ALIVE_TIMEOUT_(keep_alive_timeout),
			timer_(io_service), http_handler_(handler), req_(this,res_), static_dir_(static_dir), upload_check_(upload_check)
		{
			init_multipart_parser();
		}

		tcp_socket& socket()
		{
#ifdef CINATRA_ENABLE_SSL
			return socket_.next_layer();
#else
			return socket_;
#endif
		}

		std::string local_address() {
			if (has_closed_) {
				return "";
			}

			std::stringstream ss;
			ss << socket_.local_endpoint();
			return ss.str();
		}

		std::string remote_address() {
			if (has_closed_) {
				return "";
			}

			std::stringstream ss;
			ss << socket_.remote_endpoint();
			return ss.str();
		}

		void start() {
			do_read();
		}

		const std::string& static_dir() {
			return static_dir_;
		}

		void on_error(status_type status, std::string&& reason) {
			keep_alive_ = false;
			req_.set_state(data_proc_state::data_error);
			response_back(status, std::move(reason));
		}

		void on_close() {
			keep_alive_ = false;
			req_.set_state(data_proc_state::data_error);
			close();
		}

		void reset_timer() {
			timer_.expires_from_now(std::chrono::seconds(KEEP_ALIVE_TIMEOUT_));
			auto self = this->shared_from_this();

			timer_.async_wait([self](boost::system::error_code const& ec) {
				if (ec) {
					return;
				}

				self->close();
			});
		}

		void set_tag(std::any&& tag) {
			tag_ = std::move(tag);
		}

		auto& get_tag() {
			return tag_;
		}

		template<typename... Fs>
		void send_ws_string(std::string msg, Fs&&... fs) {
			send_ws_msg(std::move(msg), opcode::text, std::forward<Fs>(fs)...);
		}

		template<typename... Fs>
		void send_ws_binary(std::string msg, Fs&&... fs) {
			send_ws_msg(std::move(msg), opcode::binary, std::forward<Fs>(fs)...);
		}

		template<typename... Fs>
		void send_ws_msg(std::string msg, opcode op = opcode::text, Fs&&... fs) {
			constexpr const size_t size = sizeof...(Fs);
			static_assert(size != 0 || size != 2);
			if constexpr(size == 2) {
				set_callback(std::forward<Fs>(fs)...);
			}

			auto header = ws_.format_header(msg.length(), op);
			send_msg(std::move(header), std::move(msg));
		}

		void write_chunked_header(std::string_view mime,bool is_range=false) {
			req_.set_http_type(content_type::chunked);
			reset_timer();
			if(!is_range){
                chunked_header_ = http_chunk_header + "Content-Type: " + std::string(mime.data(), mime.length()) + "\r\n\r\n";
			}else{
                chunked_header_ = http_range_chunk_header + "Content-Type: " + std::string(mime.data(), mime.length()) + "\r\n\r\n";
            }
			boost::asio::async_write(socket_,
				boost::asio::buffer(chunked_header_),
				[self = this->shared_from_this()](const boost::system::error_code& ec, std::size_t bytes_transferred) {
				self->handle_chunked_header(ec);
			});
		}

		void write_chunked_data(std::string&& buf, bool eof) {
			reset_timer();

			std::vector<boost::asio::const_buffer> buffers = res_.to_chunked_buffers(buf.data(), buf.length(), eof);
			if (buffers.empty()) {
				handle_write(boost::system::error_code{});
				return;
			}

			auto self = this->shared_from_this();
			boost::asio::async_write(socket_, buffers, [this, self, buf = std::move(buf), eof](const boost::system::error_code& ec, size_t) {
				if (ec) {
					return;
				}

				if (eof) {
					req_.set_state(data_proc_state::data_end);
				}
				else {
					req_.set_state(data_proc_state::data_continue);
				}

				call_back();
			});
		}

		void response_now() {
			do_write();
		}

		//~connection() {
		//	close();
		//}
	private:
		void do_read() {
			req_.reset();
			res_.reset();
			reset_timer();

#ifdef CINATRA_ENABLE_SSL
			if (!has_shake_) {
				async_handshake();
			}
			else {
				async_read_some();
			}
#else
			async_read_some();
#endif
		}

		void async_handshake() {
#ifdef CINATRA_ENABLE_SSL
			socket_.async_handshake(boost::asio::ssl::stream_base::server,
				[this, self = this->shared_from_this()](const boost::system::error_code& error) {
				if (error) {
					std::cout << error.message() << std::endl;
					return;
				}

				has_shake_ = true;
				async_read_some();
			});
#endif
		}

		void async_read_some() {
			socket_.async_read_some(boost::asio::buffer(req_.buffer(), req_.left_size()),
				[this, self = this->shared_from_this()](const boost::system::error_code& e, std::size_t bytes_transferred) {
				if (e) {
					if (e == boost::asio::error::eof) {
						shutdown_send();
					}
					has_shake_ = false;
					return;
				}

				self->handle_read(e, bytes_transferred);
			});
		}

		void handle_read(const boost::system::error_code& e, std::size_t bytes_transferred) {
			if (e) {
				if (e == boost::asio::error::eof) {
					shutdown_send();
				}

				return;
			}

			//1. read some
			//2. check request length
			auto last_len = req_.current_size();
			bool at_capacity = req_.update_and_expand_size(bytes_transferred);
			if (at_capacity) { //the request is too long
				response_back(status_type::bad_request, "The request is too long, limitation is 3M");
				return;
			}

			//3. parse request
			int ret = req_.parse_header(last_len);

			//4. check parse result
			if (ret == parse_status::has_error) { //4.1 parse request error, response bad request
				response_back(status_type::bad_request);
				return;
			}

			check_keep_alive();
			if (ret == parse_status::not_complete) { //4.2 not completed, continue read
				//do_read();
				do_read_head();
			}
			else {
				if (req_.get_method() == "GET"&&http_cache::get().need_cache(req_.get_url())&&!http_cache::get().not_cache(req_.get_url())) {
					auto raw_url = req_.raw_url();
					if (!http_cache::get().empty()) {
						auto resp_vec = http_cache::get().get(std::string(raw_url.data(), raw_url.length()));
						//write back cache
						if (!resp_vec.empty()) {
							std::vector<boost::asio::const_buffer> buffers;
							for(auto &iter:resp_vec)
							{
								buffers.emplace_back(boost::asio::buffer(iter.data(),iter.size()));
							}
							boost::asio::async_write(socket_, buffers,
								[self = this->shared_from_this(), resp_vec = std::move(resp_vec)](const boost::system::error_code& ec, std::size_t bytes_transferred) {
								self->handle_write(ec);
							});
							return;
						}
					}
				}

				//4.3 complete request
				//5. check if has body
				set_response_attr();
				if (req_.has_body()) { //5.1 has body
					auto type = get_content_type();
					req_.set_http_type(type);
					switch (type) {
					case cinatra::content_type::string:
					case cinatra::content_type::unknown:
						handle_string_body(bytes_transferred);
						break;
					case cinatra::content_type::multipart:
						handle_multipart();
						break;
					case cinatra::content_type::octet_stream:
						handle_octet_stream(bytes_transferred);
						break;
					case cinatra::content_type::urlencoded:
						handle_form_urlencoded(bytes_transferred);
						break;
					case cinatra::content_type::chunked:
						handle_chunked(bytes_transferred);
						break;
					}
				}
				else { //5.2 just head, no body; because websocket just 'GET' head, no body, so need deal with websocket
					handle_header_request(); //do business, deal with the complete request
				}
			}
		}

		void do_read_head() {
			reset_timer();

			socket_.async_read_some(boost::asio::buffer(req_.buffer(), req_.left_size()),
				[self = this->shared_from_this()](const boost::system::error_code& e, std::size_t bytes_transferred) {
				self->handle_read(e, bytes_transferred);
			});
		}

		void do_read_body() {
			reset_timer();

			auto self = this->shared_from_this();
			boost::asio::async_read(socket_, boost::asio::buffer(req_.buffer(), req_.left_body_len()),
				[this, self](const boost::system::error_code& ec, size_t bytes_transferred) {
				if (ec) {
					//LOG_WARN << ec.message();
					close();
					return;
				}

				req_.update_size(bytes_transferred);
				req_.reduce_left_body_size(bytes_transferred);

				if (req_.body_finished()) {
					handle_body();
				}
				else {
					do_read_body();
				}
			});
		}

		void do_write() {
			reset_timer();
			//auto content_length = res_.get_header_value("content-length");
			//assert(!content_length.empty());
			std::vector<boost::asio::const_buffer> buffers = res_.to_buffers();
			if (buffers.empty()) {
				handle_write(boost::system::error_code{});
				return;
			}

			//cache
			if (req_.get_method() == "GET"&&http_cache::get().need_cache(req_.get_url()) && !http_cache::get().not_cache(req_.get_url())) {
				auto raw_url = req_.raw_url();
				http_cache::get().add(std::string(raw_url.data(), raw_url.length()), res_.raw_content());
			}

			boost::asio::async_write(socket_, buffers,
				[self = this->shared_from_this()](const boost::system::error_code& ec, std::size_t bytes_transferred) {
				self->handle_write(ec);
			});
		}

		content_type get_content_type() {
			if (req_.is_chunked())
				return content_type::chunked;

			auto content_type = req_.get_header_value("content-type");
			if (!content_type.empty()) {
				if (content_type.find("application/x-www-form-urlencoded") != std::string_view::npos) {
					return content_type::urlencoded;
				}
				else if (content_type.find("multipart/form-data") != std::string_view::npos) {
					auto size = content_type.find("=");
					auto bd = content_type.substr(size + 1, content_type.length() - size);
					if (bd[0] == '"'&& bd[bd.length()-1] == '"') {
						bd = bd.substr(1, bd.length() - 2);
					}
					std::string boundary(bd.data(), bd.length());
					multipart_parser_.set_boundary("\r\n--" + std::move(boundary));
					return content_type::multipart;
				}
				else if (content_type.find("application/octet-stream") != std::string_view::npos) {
					return content_type::octet_stream;
				}
				else {
					return content_type::string;
				}
			}

			return content_type::unknown;
		}

		void close() {
			boost::system::error_code ec;
			socket().close(ec);
			has_shake_ = false;
			has_closed_ = true;
		}

		void set_response_attr() {
			auto host = req_.get_header_value("host");
			if (!host.empty()) {
				size_t pos = host.find(':');
				if (pos != std::string_view::npos) {
					res_.set_domain(host.substr(0, pos));
				}
			}
			
			res_.set_path(req_.get_url());
		}

		/****************** begin handle http body data *****************/
		void handle_string_body(std::size_t bytes_transferred) {
			//defalt add limitation for string_body and else. you can remove the limitation for very big string.
			if (req_.at_capacity()) {
				response_back(status_type::bad_request, "The request is too long, limitation is 3M");
				return;
			}

			if (req_.has_recieved_all()) {
				handle_body();
			}
			else {
				req_.expand_size();
				assert(req_.current_size() >= req_.header_len());
				size_t part_size = req_.current_size() - req_.header_len();
				if (part_size == -1) {
					response_back(status_type::internal_server_error);
					return;
				}
				req_.reduce_left_body_size(part_size);
				do_read_body();
			}
		}

		//-------------octet-stream----------------//
		void handle_octet_stream(size_t bytes_transferred) {
			//call_back();
			try {
				std::string name = static_dir_ + uuids::uuid_system_generator{}().to_short_str();
				req_.open_upload_file(name);
			}
			catch (const std::exception& ex) {
				response_back(status_type::internal_server_error, ex.what());
				return;
			}

			req_.set_state(data_proc_state::data_continue);//data
			size_t part_size = bytes_transferred - req_.header_len();
			if (part_size != 0) {
				req_.reduce_left_body_size(part_size);
				req_.set_part_data({ req_.current_part(), part_size });
				req_.write_upload_data(req_.current_part(), part_size);
				//call_back();
			}

			if (req_.has_recieved_all()) {
				//on finish
				req_.set_state(data_proc_state::data_end);
				call_back();
				do_write();
			}
			else {
				req_.fit_size();
				req_.set_current_size(0);
				do_read_octet_stream_body();
			}
		}

		void do_read_octet_stream_body() {
			auto self = this->shared_from_this();
			boost::asio::async_read(socket_, boost::asio::buffer(req_.buffer(), req_.left_body_len()),
				[this, self](const boost::system::error_code& ec, size_t bytes_transferred) {
				if (ec) {
					req_.set_state(data_proc_state::data_error);
					call_back();
					close();
					return;
				}

				req_.set_part_data({ req_.buffer(), bytes_transferred });
				req_.write_upload_data(req_.buffer(), bytes_transferred);
				//call_back();

				req_.reduce_left_body_size(bytes_transferred);

				if (req_.body_finished()) {
					req_.set_state(data_proc_state::data_end);
					call_back();
					do_write();
				}
				else {
					do_read_octet_stream_body();
				}
			});
		}

		//-------------octet-stream----------------//

		//-------------form urlencoded----------------//
		//TODO: here later will refactor the duplicate code
		void handle_form_urlencoded(size_t bytes_transferred) {
			if (req_.at_capacity()) {
				response_back(status_type::bad_request, "The request is too long, limitation is 3M");
				return;
			}

			if (req_.has_recieved_all()) {
				handle_url_urlencoded_body();
			}
			else {
				req_.expand_size();
				size_t part_size = bytes_transferred - req_.header_len();
				req_.reduce_left_body_size(part_size);
				//req_.fit_size();
				do_read_form_urlencoded();
			}
		}

		void handle_url_urlencoded_body() {
			bool success = req_.parse_form_urlencoded();

			if (!success) {
				response_back(status_type::bad_request, "form urlencoded error");
				return;
			}

			call_back();
			if (!res_.need_delay())
				do_write();
		}

		void do_read_form_urlencoded() {
			reset_timer();

			auto self = this->shared_from_this();
			boost::asio::async_read(socket_, boost::asio::buffer(req_.buffer(), req_.left_body_len()),
				[this, self](const boost::system::error_code& ec, size_t bytes_transferred) {
				if (ec) {
					//LOG_WARN << ec.message();
					close();
					return;
				}

				req_.update_size(bytes_transferred);
				req_.reduce_left_body_size(bytes_transferred);

				if (req_.body_finished()) {
					handle_url_urlencoded_body();
				}
				else {
					do_read_form_urlencoded();
				}
			});
		}
		//-------------form urlencoded----------------//

		void call_back() {
			assert(http_handler_);
			http_handler_(req_, res_);
		}

		void call_back_data() {
			req_.set_state(data_proc_state::data_continue);
			call_back();
			req_.set_part_data({});
		}
		//-------------multipart----------------------//
		void init_multipart_parser() {
				multipart_parser_.on_part_begin = [this](const multipart_headers & headers) {
					req_.set_multipart_headers(headers);
					auto filename = req_.get_multipart_field_name("filename");
					is_multi_part_file_ = req_.is_multipart_file();
					if (filename.empty()&& is_multi_part_file_) {
						req_.set_state(data_proc_state::data_error);
						res_.set_status_and_content(status_type::bad_request, "mutipart error");
						return;
					}						
					if(is_multi_part_file_)
					{
						auto ext = get_extension(filename);
						try {
							std::string name = static_dir_ + uuids::uuid_system_generator{}().to_short_str()
								+ std::string(ext.data(), ext.length());
							req_.open_upload_file(name);
						}
						catch (const std::exception& ex) {
							req_.set_state(data_proc_state::data_error);
							res_.set_status_and_content(status_type::internal_server_error, ex.what());
							return;
						}						
					}else{
						auto key = req_.get_multipart_field_name("name");
						req_.save_multipart_key_value(std::string(key.data(),key.size()),"");
					}
				};
				multipart_parser_.on_part_data = [this](const char* buf, size_t size) {
					if (req_.get_state() == data_proc_state::data_error) {
						return;
					}
					if(is_multi_part_file_){
						req_.write_upload_data(buf, size);
					}else{
						auto key = req_.get_multipart_field_name("name");
						req_.update_multipart_value(std::move(key), buf, size);
					}
				};
				multipart_parser_.on_part_end = [this] {
					if (req_.get_state() == data_proc_state::data_error)
						return;
					if(is_multi_part_file_)
					{
						req_.close_upload_file();
					}
				};
				multipart_parser_.on_end = [this] {
					if (req_.get_state() == data_proc_state::data_error)
						return;
                    req_.handle_multipart_key_value();
					//call_back(); 
				};		
		}

		bool parse_multipart(size_t size, std::size_t length) {
			if (length == 0)
				return false;

			req_.set_part_data(std::string_view(req_.buffer(size), length));
			std::string_view multipart_body = req_.get_part_data();
			size_t bufsize = multipart_body.length();

			size_t fed = 0;
			do {
				size_t ret = multipart_parser_.feed(multipart_body.data() + fed, multipart_body.length() - fed);
				fed += ret;
			} while (fed < bufsize && !multipart_parser_.stopped());

			if (multipart_parser_.has_error()) {
				//LOG_WARN << multipart_parser_.get_error_message();
				req_.set_state(data_proc_state::data_error);
				return true;
			}

			req_.reduce_left_body_size(length);
			return false;
		}

		void handle_multipart() {
			if (upload_check_) {
				bool r = (*upload_check_)(req_, res_);
				if (!r) {
					close();
					return;
				}					
			}

			bool has_error = parse_multipart(req_.header_len(), req_.current_size() - req_.header_len());

			if (has_error) {
				response_back(status_type::bad_request, "mutipart error");
				return;
			}

			if (req_.has_recieved_all_part()) {
				call_back();
				do_write();
			}
			else {
				req_.set_current_size(0);
				do_read_multipart();
			}
		}

		void do_read_multipart() {
			reset_timer();

			req_.fit_size();
			auto self = this->shared_from_this();
			boost::asio::async_read(socket_, boost::asio::buffer(req_.buffer(), req_.left_body_len()),
				[self, this](boost::system::error_code ec, std::size_t length) {
				if (ec) {
					req_.set_state(data_proc_state::data_error);
					call_back();
					response_back(status_type::bad_request, "mutipart error");
					return;
				}

				bool has_error = parse_multipart(0, length);

				if (has_error) { //parse error
					keep_alive_ = false;
					response_back(status_type::bad_request, "mutipart error");
					return;
				}

				reset_timer();
				if (req_.body_finished()) {
					call_back();
					do_write();
					return;
				}

				req_.set_current_size(0);
				do_read_part_data();
			});
		}

		void do_read_part_data() {
			auto self = this->shared_from_this();
			boost::asio::async_read(socket_, boost::asio::buffer(req_.buffer(), req_.left_body_size()),
				[self, this](boost::system::error_code ec, std::size_t length) {
				if (ec) {
					req_.set_state(data_proc_state::data_error);
					call_back();
					return;
				}

				bool has_error = parse_multipart(0, length);

				if (has_error) {
					response_back(status_type::bad_request, "mutipart error");
					return;
				}

				reset_timer();
				if (!req_.body_finished()) {
					do_read_part_data();
				}
				else {
					//response_back(status_type::ok, "multipart finished");
					call_back();
					do_write();
				}
			});
		}
		//-------------multipart----------------------//

		void handle_header_request() {
			if (is_upgrade_) { //websocket
				req_.set_http_type(content_type::websocket);
				//timer_.cancel();
				ws_.upgrade_to_websocket(req_, res_);
				response_handshake();
				return;
			}

			bool r = handle_gzip();
			if (!r) {
				response_back(status_type::bad_request, "gzip uncompress error");
				return;
			}

			call_back();

			if (req_.get_content_type() == content_type::chunked)
				return;

			if (req_.get_state() == data_proc_state::data_error) {
				return;
			}

			if (!res_.need_delay())
				do_write();
		}

		//-------------web socket----------------//
		void response_handshake() {
			std::vector<boost::asio::const_buffer> buffers = res_.to_buffers();
			if (buffers.empty()) {
				close();
				return;
			}

			auto self = this->shared_from_this();
			boost::asio::async_write(socket_, buffers, [this, self](const boost::system::error_code& ec, std::size_t length) {
				if (ec) {
					close();
					return;
				}

				req_.set_state(data_proc_state::data_begin);
				call_back();
				req_.call_event(req_.get_state());

				req_.set_current_size(0);
				do_read_websocket_head(SHORT_HEADER);
			});
		}

		void do_read_websocket_head(size_t length) {
			auto self = this->shared_from_this();
			boost::asio::async_read(socket_, boost::asio::buffer(req_.buffer(), length),
				[this, self](const boost::system::error_code& ec, size_t bytes_transferred) {
				if (ec) {
					timer_.cancel();
					req_.call_event(data_proc_state::data_error);

					close();
					return;
				}

				size_t length = bytes_transferred + req_.current_size();
				req_.set_current_size(0);
				int ret = ws_.parse_header(req_.buffer(), length);

				if (ret == parse_status::complete) {
					//read payload
					auto payload_length = ws_.payload_length();
					req_.set_body_len(payload_length);
					if (req_.at_capacity(payload_length)) {
						req_.call_event(data_proc_state::data_error);
						close();
						return;
					}

					req_.set_current_size(0);
					req_.fit_size();
					do_read_websocket_data(req_.left_body_len());
				}
				else if (ret == parse_status::not_complete) {
					req_.set_current_size(bytes_transferred);
					do_read_websocket_head(ws_.left_header_len());
				}
				else {
					req_.call_event(data_proc_state::data_error);
					close();
				}
			});
		}

		void do_read_websocket_data(size_t length) {
			auto self = this->shared_from_this();
			boost::asio::async_read(socket_, boost::asio::buffer(req_.buffer(), length),
				[this, self](const boost::system::error_code& ec, size_t bytes_transferred) {
				if (ec) {
					req_.call_event(data_proc_state::data_error);
					close();
					return;
				}

				if (req_.body_finished()) {
					req_.set_current_size(0);
					bytes_transferred = ws_.payload_length();
				}

				std::string payload;
				ws_frame_type ret = ws_.parse_payload(req_.buffer(), bytes_transferred, payload);
				if (ret == ws_frame_type::WS_INCOMPLETE_FRAME) {
					req_.update_size(bytes_transferred);
					req_.reduce_left_body_size(bytes_transferred);
					do_read_websocket_data(req_.left_body_len());
					return;
				}

				if (!handle_ws_frame(ret, std::move(payload), bytes_transferred))
					return;

				req_.set_current_size(0);
				do_read_websocket_head(SHORT_HEADER);
			});
		}

		bool handle_ws_frame(ws_frame_type ret, std::string&& payload, size_t bytes_transferred) {
			switch (ret)
			{
			case cinatra::ws_frame_type::WS_ERROR_FRAME:
				req_.call_event(data_proc_state::data_error);
				close();
				return false;
			case cinatra::ws_frame_type::WS_OPENING_FRAME:
				break;
			case cinatra::ws_frame_type::WS_TEXT_FRAME:
			case cinatra::ws_frame_type::WS_BINARY_FRAME:
			{
				reset_timer();
				req_.set_part_data({ payload.data(), payload.length() });
				req_.call_event(data_proc_state::data_continue);
			}
			//on message
			break;
			case cinatra::ws_frame_type::WS_CLOSE_FRAME:
			{
				close_frame close_frame = ws_.parse_close_payload(payload.data(), payload.length());
				const int MAX_CLOSE_PAYLOAD = 123;
				size_t len = std::min<size_t>(MAX_CLOSE_PAYLOAD, payload.length());
				req_.set_part_data({ close_frame.message, len });
				req_.call_event(data_proc_state::data_close);

				std::string close_msg = ws_.format_close_payload(opcode::close, close_frame.message, len);
				auto header = ws_.format_header(close_msg.length(), opcode::close);
				send_msg(std::move(header), std::move(close_msg));
			}
			break;
			case cinatra::ws_frame_type::WS_PING_FRAME:
			{
				auto header = ws_.format_header(payload.length(), opcode::pong);
				send_msg(std::move(header), std::move(payload));
			}
			break;
			case cinatra::ws_frame_type::WS_PONG_FRAME:
				ws_ping();
				break;
			default:
				break;
			}

			return true;
		}

		void ws_ping() {
			timer_.expires_from_now(std::chrono::seconds(60));
			timer_.async_wait([self = this->shared_from_this()](boost::system::error_code const& ec) {
				if (ec) {
					self->close();
					return;
				}

				self->send_ws_msg("ping", opcode::ping);
			});
		}
		//-------------web socket----------------//

		//-------------chunked(read chunked not support yet, write chunked is ok)----------------------//
		void handle_chunked(size_t bytes_transferred) {
			int ret = req_.parse_chunked(bytes_transferred);
			if (ret == parse_status::has_error) {
				response_back(status_type::internal_server_error, "not support yet");
				return;
			}
		}

		void handle_chunked_header(const boost::system::error_code& ec) {
			if (ec) {
				return;
			}

			req_.set_state(data_proc_state::data_continue);
			call_back();//app set the data
		}
		//-------------chunked(read chunked not support yet, write chunked is ok)----------------------//

		void handle_body() {
			if (req_.at_capacity()) {
				response_back(status_type::bad_request, "The body is too long, limitation is 3M");
				return;
			}

			bool r = handle_gzip();
			if (!r) {
				response_back(status_type::bad_request, "gzip uncompress error");
				return;
			}

			if (req_.get_content_type() == content_type::multipart) {
				bool has_error = parse_multipart(req_.header_len(), req_.current_size() - req_.header_len());
				if (has_error) {
					response_back(status_type::bad_request, "mutipart error");
					return;
				}
				do_write();
				return;
			}

			call_back();

			if (!res_.need_delay())
				do_write();
		}

		bool handle_gzip() {
			if (req_.has_gzip()) {
				return req_.uncompress();
			}

			return true;
		}

		void response_back(status_type status, std::string&& content) {
			res_.set_status_and_content(status, std::move(content));
			do_write(); //response to client
		}

		void response_back(status_type status) {
			res_.set_status_and_content(status);
			do_write(); //response to client
		}

		enum parse_status {
			complete = 0,
			has_error = -1,
			not_complete = -2,
		};

		void check_keep_alive() {
			auto req_conn_hdr = req_.get_header_value("connection");
			if (req_.is_http11()) {
				// HTTP1.1
				//ͷ����û�а���connection�ֶ�
				//����ͷ���а�����connection�ֶε���ֵ��Ϊclose
				//��������ǳ�����
				keep_alive_ = req_conn_hdr.empty() || !iequal(req_conn_hdr.data(), req_conn_hdr.size(), "close");
			}
			else {
				//HTTP1.0������(0.9 or ?)
				//ͷ������connection,����connection�ֶ�ֵΪkeep-alive
				//����������ǳ�����
				keep_alive_ = !req_conn_hdr.empty() && iequal(req_conn_hdr.data(), req_conn_hdr.size(), "keep-alive");
			}

			if (keep_alive_) {
				is_upgrade_ = ws_.is_upgrade(req_);
				if (!is_upgrade_)
					res_.add_header("Connection", "keep-alive");
			}
			else {
				res_.add_header("Connection", "close");
			}
		}

		void handle_write(const boost::system::error_code& ec) {
			if (ec) {
				return;
			}

			if (keep_alive_) {
				do_read();
			}
			else {
				timer_.cancel(); //avoid close two times
				shutdown();
				close();
			}
		}

		void shutdown_send() {
			boost::system::error_code ignored_ec;
#ifdef CINATRA_ENABLE_SSL
			socket_.shutdown(ignored_ec);
#else
			socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ignored_ec);
#endif
		}

		void shutdown() {
			boost::system::error_code ignored_ec;
#ifdef CINATRA_ENABLE_SSL
			socket_.shutdown(ignored_ec);
#else
			socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
#endif
		}

		//-----------------send message----------------//
		void send_msg(std::string&& data) {
			std::lock_guard<std::mutex> lock(buffers_mtx_);
			buffers_[active_buffer_ ^ 1].push_back(std::move(data)); // move input data to the inactive buffer
			if (!writing())
				do_write_msg();
		}

		void send_msg(std::string&& header, std::string&& data) {
			std::lock_guard<std::mutex> lock(buffers_mtx_);
			buffers_[active_buffer_ ^ 1].push_back(std::move(header));
			buffers_[active_buffer_ ^ 1].push_back(std::move(data)); // move input data to the inactive buffer
			if (!writing())
				do_write_msg();
		}

		void do_write_msg() {
			active_buffer_ ^= 1; // switch buffers
			for (const auto& data : buffers_[active_buffer_]) {
				buffer_seq_.push_back(boost::asio::buffer(data));
			}

			boost::asio::async_write(socket_, buffer_seq_, [this, self = this->shared_from_this()](const boost::system::error_code& ec, size_t bytes_transferred) {
				std::lock_guard<std::mutex> lock(buffers_mtx_);
				buffers_[active_buffer_].clear();
				buffer_seq_.clear();

				if (!ec) {
					if (send_ok_cb_)
						send_ok_cb_();
					if (!buffers_[active_buffer_ ^ 1].empty()) // have more work
						do_write_msg();
				}
				else {
					if (send_failed_cb_)
						send_failed_cb_(ec);
					req_.set_state(data_proc_state::data_error);
					call_back();
					close();
				}
			});
		}

		bool writing() const { return !buffer_seq_.empty(); }

		template<typename F1, typename F2>
		void set_callback(F1&& f1, F2&& f2) {
			send_ok_cb_ = std::move(f1);
			send_failed_cb_ = std::move(f2);
		}

		//-----------------send message----------------//
		socket_type socket_;
		boost::asio::steady_timer timer_;
		response res_;
		request req_;
		websocket ws_;
		bool is_upgrade_ = false;
		bool keep_alive_ = false;
		const std::size_t MAX_REQ_SIZE_;
		const long KEEP_ALIVE_TIMEOUT_;
		const std::string& static_dir_;
		bool has_shake_ = false;
		bool has_closed_ = false;

		//for writing message
		std::mutex buffers_mtx_;
		std::vector<std::string> buffers_[2]; // a double buffer
		std::vector<boost::asio::const_buffer> buffer_seq_;
		int active_buffer_ = 0;
		std::function<void()> send_ok_cb_ = nullptr;
		std::function<void(const boost::system::error_code&)> send_failed_cb_ = nullptr;

		std::string chunked_header_;
		multipart_reader multipart_parser_;
		bool is_multi_part_file_;
		//callback handler to application layer
		const http_handler& http_handler_;
		std::function<bool(request& req, response& res)>* upload_check_ = nullptr;
		std::any tag_;
	};

	inline constexpr data_proc_state ws_open = data_proc_state::data_begin;
	inline constexpr data_proc_state ws_message = data_proc_state::data_continue;
	inline constexpr data_proc_state ws_close = data_proc_state::data_close;
	inline constexpr data_proc_state ws_error = data_proc_state::data_error;
}