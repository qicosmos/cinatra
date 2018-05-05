//
// Created by qiyu on 12/19/17.
//

#ifndef CINATRA_RESPONSE_HPP
#define CINATRA_RESPONSE_HPP
#include "use_asio.hpp"
#include <string>
#include <vector>
#include <string_view>
#include "response_cv.hpp"
#include "itoa.hpp"
#include "utils.hpp"
#include "mime_types.hpp"
namespace cinatra {
	class response {
	public:
		std::vector<boost::asio::const_buffer> get_response_buffer(std::string&& body) {
			set_content(std::move(body));

			auto buffers = to_buffers();
			return buffers;
		}

		std::vector<boost::asio::const_buffer> to_buffers() {
			std::vector<boost::asio::const_buffer> buffers;
			add_header("Host", "cinatra");
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

			return buffers;
		}

		void add_header(std::string&& key, std::string&& value) {
			headers_.emplace_back(std::move(key), std::move(value));
		}

		//std::string_view get_header_value(const std::string& key) const {
		//	auto it = headers_.find(key);
		//	if (it == headers_.end()) {
		//		return {};
		//	}

		//	return std::string_view(it->second.data(), it->second.length());
		//}

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
		}

		void set_status_and_content(status_type status, std::string&& content,cinatra::res_content_type res_type = cinatra::res_content_type::none, content_encoding encoding = content_encoding::none) {
			status_ = status;
			if(res_type!=cinatra::res_content_type::none){
				auto iter = cinatra::res_mime_map.find(res_type);
				if(iter!=cinatra::res_mime_map.end()){
					add_header("Content-type",std::string(iter->second.data(),iter->second.size()));
				}
			}
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
		}

		bool need_delay() const {
			return delay_;
		}

		void reset() {
			status_ = status_type::init;
			proc_continue_ = true;
			headers_.clear();
			content_.clear();
		}

		void set_continue(bool con) {
			proc_continue_ = con;
		}

		bool need_continue() const {
			return proc_continue_;
		}

		void set_content(std::string&& content) {
			char temp[20] = {};
			itoa_fwd((int)content.size(), temp);
			add_header("Content-Length", temp);

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

	private:
		
		//std::map<std::string, std::string, ci_less> headers_;
		std::vector<std::pair<std::string, std::string>> headers_;
		std::string content_;
		content_type body_type_ = content_type::unknown;
		status_type status_ = status_type::init;
		bool proc_continue_ = true;
		std::string chunk_size_;

		bool delay_ = false;
	};
}
#endif //CINATRA_RESPONSE_HPP
