//
// Created by qiyu on 12/19/17.
//

#ifndef CINATRA_RESPONSE_HPP
#define CINATRA_RESPONSE_HPP
#include "use_asio.hpp"
#include <string>
#include <vector>
#include <string_view>
#include <chrono>
#include "response_cv.hpp"
#include "itoa.hpp"
#include "utils.hpp"
#include "mime_types.hpp"
#include "session_manager.hpp"
#include "nlohmann_json.hpp"
#include "render.h"
#include "http_cache.hpp"
namespace cinatra {
	class response {
	public:
        response() {
        }

		std::string& response_str(){
            return rep_str_;
        }

        void enable_response_time(bool enable) {
            need_response_time_ = enable;
            if (need_response_time_) {
                char mbstr[50];
                std::time_t tm = std::chrono::system_clock::to_time_t(last_time_);
                std::strftime(mbstr, sizeof(mbstr), "%a, %d %b %Y %T GMT", std::localtime(&tm));
                last_date_str_ = mbstr;
            }
        }

        template<status_type status, res_content_type content_type, size_t N>
        constexpr auto set_status_and_content(const char(&content)[N], content_encoding encoding = content_encoding::none) {
            constexpr auto status_str = to_rep_string(status);
            constexpr auto type_str = to_content_type_str(content_type);
            constexpr auto len_str = num_to_string<N-1>::value;

            rep_str_.append(status_str).append(len_str.data(), len_str.size()).append(type_str).append(rep_server);

            if(need_response_time_)
                append_date_time();
            else
                rep_str_.append("\r\n");

            rep_str_.append(content);
        }

        void append_date_time() {
            using namespace std::chrono_literals;

            auto t = std::chrono::system_clock::now();
            if (t - last_time_ > 1s) {
                char mbstr[50];
                std::time_t tm = std::chrono::system_clock::to_time_t(t);
                std::strftime(mbstr, sizeof(mbstr), "%a, %d %b %Y %T GMT", std::localtime(&tm));
                last_date_str_ = mbstr;
                rep_str_.append("Date: ").append(mbstr).append("\r\n\r\n");
                last_time_ = t;
            }
            else {
                rep_str_.append("Date: ").append(last_date_str_).append("\r\n\r\n");
            }
        }

		void build_response_str() {
			rep_str_.append(to_rep_string(status_));

//			if (keep_alive) {
//				rep_str_.append("Connection: keep-alive\r\n");
//			}
//			else {
//				rep_str_.append("Connection: close\r\n");
//			}

			if (!headers_.empty()) {
				for (auto& header : headers_) {
					rep_str_.append(header.first).append(":").append(header.second).append("\r\n");
				}
				headers_.clear();
			}

			char temp[20] = {};
			itoa_fwd((int)content_.size(), temp);
			rep_str_.append("Content-Length: ").append(temp).append("\r\n");
            if(res_type_!=res_content_type::none){
                rep_str_.append(get_content_type(res_type_));
            }
            rep_str_.append("Server: cinatra\r\n");
			if (session_ != nullptr && session_->is_need_update()) {
				auto cookie_str = session_->get_cookie().to_string();
				rep_str_.append("Set-Cookie: ").append(cookie_str).append("\r\n");
				session_->set_need_update(false);
			}

            if (need_response_time_)
                append_date_time();
            else
                rep_str_.append("\r\n");

			rep_str_.append(std::move(content_));
		}

		std::vector<boost::asio::const_buffer> to_buffers() {
			std::vector<boost::asio::const_buffer> buffers;
			add_header("Host", "cinatra");
			if(session_ != nullptr && session_->is_need_update())
			{
				auto cookie_str = session_->get_cookie().to_string();
				add_header("Set-Cookie",cookie_str.c_str());
                session_->set_need_update(false);
			}
			buffers.reserve(headers_.size() * 4 + 5);
			buffers.emplace_back(to_buffer(status_));
			for (auto const& h : headers_) {
				buffers.emplace_back(boost::asio::buffer(h.first));
				buffers.emplace_back(boost::asio::buffer(name_value_separator));
				buffers.emplace_back(boost::asio::buffer(h.second));
				buffers.emplace_back(boost::asio::buffer(crlf));
			}

			buffers.push_back(boost::asio::buffer(crlf));

			if (body_type_ == content_type::string) {
				buffers.emplace_back(boost::asio::buffer(content_.data(), content_.size()));
			}

			if (http_cache::get().need_cache(raw_url_)) {
				cache_data.clear();
				for (auto& buf : buffers) {
					cache_data.push_back(std::string(boost::asio::buffer_cast<const char*>(buf),boost::asio::buffer_size(buf)));
				}
			}

			return buffers;
		}

		void add_header(std::string&& key, std::string&& value) {
			headers_.emplace_back(std::move(key), std::move(value));
		}

		void set_status(status_type status) {
			status_ = status;
		}

		status_type get_status() const {
			return status_;
		}

		void set_delay(bool delay) {
			delay_ = delay;
		}

		void set_status_and_content(status_type status) {
			status_ = status;
			set_content(to_string(status).data());
            build_response_str();
		}

		void set_status_and_content(status_type status, std::string&& content, res_content_type res_type = res_content_type::none, content_encoding encoding = content_encoding::none) {
			status_ = status;
            res_type_ = res_type;

#ifdef CINATRA_ENABLE_GZIP
			if (encoding == content_encoding::gzip) {
				std::string encode_str;
				bool r = gzip_codec::compress(std::string_view(content.data(), content.length()), encode_str, true);
				if (!r) {
					set_status_and_content(status_type::internal_server_error, "gzip compress error");
				}
				else {
					add_header("Content-Encoding", "gzip");
					set_content(std::move(encode_str));
				}
			}
			else 
#endif
				set_content(std::move(content));
            build_response_str();
		}

		std::string_view get_content_type(res_content_type type){
            switch (type) {
                case cinatra::res_content_type::html:
                    return rep_html;
                case cinatra::res_content_type::json:
                    return rep_json;
                case cinatra::res_content_type::string:
                    return rep_string;
                case cinatra::res_content_type::multipart:
                    return rep_multipart;
                case cinatra::res_content_type::none:
                default:
                    return "";
            }
        }

		bool need_delay() const {
			return delay_;
		}

		void reset() {
            if(headers_.empty())
			    rep_str_.clear();
            res_type_ = res_content_type::none;
			status_ = status_type::init;
			proc_continue_ = true;
			delay_ = false;
			headers_.clear();
			content_.clear();
            session_ = nullptr;

            if(cache_data.empty())
                cache_data.clear();
		}

		void set_continue(bool con) {
			proc_continue_ = con;
		}

		bool need_continue() const {
			return proc_continue_;
		}

		void set_content(std::string&& content) {
			body_type_ = content_type::string;
			content_ = std::move(content);
		}

		void set_chunked() {
			//"Transfer-Encoding: chunked\r\n"
			add_header("Transfer-Encoding", "chunked");
		}

		std::vector<boost::asio::const_buffer> to_chunked_buffers(const char* chunk_data, size_t length, bool eof) {
			std::vector<boost::asio::const_buffer> buffers;

			if (length > 0) {
				// convert bytes transferred count to a hex string.
				chunk_size_ = to_hex_string(length);

				// Construct chunk based on rfc2616 section 3.6.1
				buffers.push_back(boost::asio::buffer(chunk_size_));
				buffers.push_back(boost::asio::buffer(crlf));
				buffers.push_back(boost::asio::buffer(chunk_data, length));
				buffers.push_back(boost::asio::buffer(crlf));
			}

			//append last-chunk
			if (eof) {
				buffers.push_back(boost::asio::buffer(last_chunk));
				buffers.push_back(boost::asio::buffer(crlf));
			}

			return buffers;
		}

        std::shared_ptr<cinatra::session> start_session(const std::string& name, std::time_t expire = -1,std::string_view domain = "", const std::string &path = "/")
		{
			session_ = session_manager::get().create_session(domain, name, expire, path);
			return session_;
		}

		std::shared_ptr<cinatra::session> start_session()
		{
			if (domain_.empty()) {
				auto host = get_header_value("host");
				if (!host.empty()) {
					size_t pos = host.find(':');
					if (pos != std::string_view::npos) {
						set_domain(host.substr(0, pos));
					}
				}
			}

			session_ = session_manager::get().create_session(domain_, CSESSIONID);
			return session_;
		}

		void set_domain(std::string_view domain) {
			domain_ = domain;
		}

		std::string_view get_domain() {
			return domain_;
		}

		void set_path(std::string_view path) {
			path_ = path;
		}

		std::string_view get_path() {
			return path_;
		}

		void set_url(std::string_view url)
		{
			raw_url_ = url;
		}

		std::string_view get_url(std::string_view url)
		{
			return raw_url_;
		}

		void set_headers(std::pair<phr_header*, size_t> headers) {
			req_headers_ = headers;
		}

        void render_json(const nlohmann::json& json_data)
        {
#ifdef  CINATRA_ENABLE_GZIP
            set_status_and_content(status_type::ok,json_data.dump(),res_content_type::json,content_encoding::gzip);
#else
            set_status_and_content(status_type::ok,json_data.dump(),res_content_type::json,content_encoding::none);
#endif
        }

        void render_string(std::string&& content)
		{
#ifdef  CINATRA_ENABLE_GZIP
			set_status_and_content(status_type::ok,std::move(content),res_content_type::string,content_encoding::gzip);
#else
			set_status_and_content(status_type::ok,std::move(content),res_content_type::string,content_encoding::none);
#endif
		}

		std::vector<std::string> raw_content() {
			return cache_data;
		}

		void redirect(const std::string& url,bool is_forever = false)
		{
			add_header("Location",url.c_str());
			is_forever==false?set_status_and_content(status_type::moved_temporarily):set_status_and_content(status_type::moved_permanently);
		}

		void redirect_post(const std::string& url) {
			add_header("Location", url.c_str());
			set_status_and_content(status_type::temporary_redirect);
		}

		void set_session(std::weak_ptr<cinatra::session> sessionref)
		{
			if(sessionref.lock()){
				session_ = sessionref.lock();
			}
		}

	private:
		std::string_view get_header_value(std::string_view key) const {
			phr_header* headers = req_headers_.first;
			size_t num_headers = req_headers_.second;
			for (size_t i = 0; i < num_headers; i++) {
				if (iequal(headers[i].name, headers[i].name_len, key.data(), key.length()))
					return std::string_view(headers[i].value, headers[i].value_len);
			}

			return {};
		}

		std::string_view raw_url_;
		std::vector<std::pair<std::string, std::string>> headers_;
		std::vector<std::string> cache_data;
		std::string content_;
		content_type body_type_ = content_type::unknown;
		status_type status_ = status_type::init;
		bool proc_continue_ = true;
		std::string chunk_size_;

		bool delay_ = false;

		std::pair<phr_header*, size_t> req_headers_;
		std::string_view domain_;
		std::string_view path_;
		std::shared_ptr<cinatra::session> session_ = nullptr;
		std::string rep_str_;
		std::chrono::system_clock::time_point last_time_ = std::chrono::system_clock::now();
		std::string last_date_str_;
        res_content_type res_type_;
        bool need_response_time_ = false;
	};
}
#endif //CINATRA_RESPONSE_HPP
