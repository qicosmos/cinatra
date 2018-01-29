//
// Created by qiyu on 12/19/17.
//

#ifndef CINATRA_RESPONSE_HPP
#define CINATRA_RESPONSE_HPP
#include <boost/asio.hpp>
#include <string>
#include <vector>
#include <string_view>
#include "response_cv.hpp"
#include "itoa.hpp"
#include "utils.hpp"

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

			if (body_type_ == http_type::string)
				buffers.emplace_back(boost::asio::buffer(content_));

			return buffers;
		}

		void add_header(std::string&& key, std::string&& value) {
			headers_[std::move(key)] = std::move(value);
		}

		std::string_view get_header_value(const std::string& key) const {
			auto it = headers_.find(key);
			if (it == headers_.end()) {
				return {};
			}

			return std::string_view(it->second.data(), it->second.length());
		}

		void set_status(status_type status) {
			status_ = status;
		}

		status_type get_status() const {
			return status_;
		}

		void set_status_and_content(status_type status) {
			status_ = status;
			set_content(to_string(status).data());
		}

		void set_status_and_content(status_type status, std::string&& content) {
			status_ = status;
			set_content(std::move(content));
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

			body_type_ = http_type::string;
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
		struct ci_less
		{
			// case-independent (ci) compare_less binary function
			struct nocase_compare
			{
				bool operator() (const unsigned char& c1, const unsigned char& c2) const {
					return tolower(c1) < tolower(c2);
				}
			};
			bool operator() (const std::string & s1, const std::string & s2) const {
				return std::lexicographical_compare
				(s1.begin(), s1.end(),   // source range
					s2.begin(), s2.end(),   // dest range
					nocase_compare());  // comparison
			}
		};

		std::map<std::string, std::string, ci_less> headers_;

		std::string content_;
		http_type body_type_ = http_type::unknown;
		status_type status_ = status_type::init;
		bool proc_continue_ = true;
		std::string chunk_size_;
	};
}
#endif //CINATRA_RESPONSE_HPP
