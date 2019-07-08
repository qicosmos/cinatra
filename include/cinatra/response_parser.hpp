#pragma once
#include <array>
#include <string_view>
#include "picohttpparser.h"

namespace cinatra {
	class response_parser {
	public:
		int parse(int last_len) {
			int minor_version;

			struct phr_header headers[100];
			size_t num_headers = sizeof(headers) / sizeof(headers[0]);
			const char* msg;
			size_t msg_len;
			header_len_ = phr_parse_response(buf_.data(), cur_size_, &minor_version, &status_, &msg, &msg_len, headers, &num_headers, last_len);
			msg_ = { msg, msg_len };
			auto header_value = get_header_value(headers, num_headers, "content-length");
			if (header_value.empty()) {
				body_len_ = 0;
			}
			else {
				body_len_ = atoi(header_value.data());
			}

			return header_len_;
		}

		bool at_capacity() {
			return (header_len_ + body_len_) > MaxSize;
		}

		bool has_body() const {
			return body_len_ != 0;
		}

		std::string_view message() {
			return msg_;
		}

		std::string_view body() {
			return std::string_view(buf_.data() + header_len_, body_len_);
		}

		char* buffer() {
			return &buf_[cur_size_];
		}

		size_t left_size() {
			return buf_.size() - cur_size_;
		}

		bool update_size(size_t size) {
			cur_size_ += size;
			if (cur_size_ > MaxSize)
				return true;

			return false;
		}

		size_t current_size() const {
			return cur_size_;
		}

		void reset() {
			cur_size_ = 0;
			header_len_ = 0;
			body_len_ = 0;
			status_ = 0;
		}

		size_t total_len() {
			return header_len_ + body_len_;
		}

		bool has_recieved_all() {
			return (total_len() == current_size());
		}

		int status() const {
			return status_;
		}

	private:
		std::string_view get_header_value(phr_header* headers, size_t num_headers, std::string_view key) {
			for (size_t i = 0; i < num_headers; i++) {
				if (iequal(headers[i].name, headers[i].name_len, key.data()))
					return std::string_view(headers[i].value, headers[i].value_len);
			}

			return {};
		}

		bool iequal(const char* s, size_t l, const char* t) {
			if (strlen(t) != l)
				return false;

			for (size_t i = 0; i < l; i++) {
				if (std::tolower(s[i]) != t[i])
					return false;
			}

			return true;
		}

		size_t cur_size_ = 0;
		int header_len_ = 0;
		int body_len_ = 0;
		std::string_view msg_;
		size_t msg_len_;
		int status_ = 0;

		constexpr const static size_t MaxSize = 8192;
		std::array<char, MaxSize> buf_;
	};
}