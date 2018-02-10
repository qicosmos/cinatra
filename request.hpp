#pragma once
#include <fstream>
#include "picohttpparser.h"
#include "utils.hpp"
#include "multipart_reader.hpp"
#include "gzip.hpp"
#include "define.h"
 
namespace cinatra {
	enum class data_proc_state : int8_t {
		data_begin,
		data_continue,
		data_end,
		data_all_end,
		data_close,
		data_error
	};

	template <typename socket_type>
	class connection;
	using conn_type = connection<boost::asio::ip::tcp::socket>;

	class request {
	public:
		request(conn_type* con) : con_(con){
			buf_.resize(1024);
		}

		auto get_conn() const{
			return con_;
		}

		int parse_header(std::size_t last_len) {
			using namespace std::string_view_literals;
			num_headers_ = sizeof(headers_) / sizeof(headers_[0]);
			header_len_ = phr_parse_request(buf_.data(), cur_size_, &method_,
				&method_len_, &url_, &url_len_,
				&minor_version_, headers_, &num_headers_, last_len);

			check_gizp();
			auto header_value = get_header_value("content-length");
			if (header_value.empty()) {
				auto transfer_encoding = get_header_value("transfer-encoding");
				if (transfer_encoding == "chunked"sv) {
					is_chunked_ = true;
				}
				
				body_len_ = 0;
			}
			else {
				set_body_len(atoi(header_value.data()));
			}

            //parse url and queries
            std::string_view url = {url_, url_len_};
			size_t npos = url.find('/');
			if (npos == std::string_view::npos)
				return -1;

            size_t pos = url.find('?');
            if(pos!=std::string_view::npos){
                queries_ = parse_query(url.substr(pos+1, url_len_-pos-1));
				url_len_ = pos;
            }

			return header_len_;
		}

		void set_body_len(size_t len) {
			body_len_ = len;
			left_body_len_ = body_len_;
		}

		size_t total_len() {
			return header_len_ + body_len_;
		}

		size_t header_len() const{
			return header_len_;
		}

		size_t body_len() const {
			return  body_len_;
		}

		bool has_recieved_all() {
			return (total_len() == current_size());
		}

		bool has_recieved_all_part() {
			return (body_len_ == cur_size_ - header_len_);
		}

		bool at_capacity() {
			return (header_len_ + body_len_) > MaxSize;
		}

		bool at_capacity(size_t size) {
			return size > MaxSize;
		}

		size_t current_size() const{
			return cur_size_;
		}

		size_t left_size() {
			return buf_.size() - cur_size_;
		}

		bool update_size(size_t size) {
			cur_size_ += size;
			if (cur_size_ > MaxSize) {
				return true;
			}

			return false;
		}

		bool update_and_expand_size(size_t size) {
			if (update_size(size)) { //at capacity
				return true;
			}

			if (cur_size_ >= buf_.size())
				resize_double();

			return false;
		}

		char* buffer() {
			return &buf_[cur_size_];
		}

		std::string_view body() const{
			if (has_gzip_&&!gzip_str_.empty()) {
				return { gzip_str_.data(), gzip_str_.length() };
			}

			return std::string_view(&buf_[header_len_], body_len_);
		}

		const char* current_part() const {
			return &buf_[header_len_];
		}

		const char* buffer(size_t size) const {
			return &buf_[size];
		}

		void reset() {
			cur_size_ = 0;
			is_chunked_ = false;
			state_ = data_proc_state::data_begin;
			part_data_ = {};
		}

		void fit_size() {
			auto total = left_body_len_;// total_len();
			auto size = buf_.size();
			if (size == MaxSize)
				return;
			
			if (total < MaxSize) {
				if (total > size)
					resize(total);
			}
			else {
				resize(MaxSize);
			}
		}

		bool has_body() const {
			return body_len_ != 0 || is_chunked_;
		}

		bool is_http11() {
			return minor_version_ == 1;
		}

		size_t left_body_len() const{
			size_t size = buf_.size();
			return left_body_len_ > size ? size : left_body_len_;
		}

		bool body_finished() {
			return left_body_len_ == 0;
		}

		bool is_chunked() const{
			return is_chunked_;
		}

		bool has_gzip() const {
			return has_gzip_;
		}

		void reduce_left_body_size(size_t size) {
			left_body_len_ -= size;
		}

		size_t left_body_size() {
			auto size = buf_.size();
			return left_body_len_ > size ? size : left_body_len_;
		}

		void set_current_size(size_t size) {
			cur_size_ = size;
			if (size == 0) {
				copy_method_url();
			}
		}

		std::string_view get_header_value(std::string_view key) const {
			for (size_t i = 0; i < num_headers_; i++) {
				if (iequal(headers_[i].name, headers_[i].name_len, key.data()))
					return std::string_view(headers_[i].value, headers_[i].value_len);
			}

			return {};
		}

		const std::multimap<std::string_view, std::string_view>* get_multipart_headers() const {
			return multipart_headers_;
		}

		std::string_view get_multipart_file_name() const {
			auto it = multipart_headers_->begin();
			auto val = it->second;
			auto pos = val.find("filename");
			if (pos == std::string_view::npos) {
				return {};
			}

			auto start = val.find('"', pos) + 1;
			auto end = val.rfind('"');
			if (start == std::string_view::npos || end == std::string_view::npos || end<start) {
				return {};
			}

			auto filename = val.substr(start, end - start);
			return filename;
		}

		void set_multipart_headers(const std::multimap<std::string_view, std::string_view>& headers) {
			multipart_headers_ = &headers;
		}

		std::map<std::string_view, std::string_view> parse_query(std::string_view str) {
			std::map<std::string_view, std::string_view> query;
			std::string_view key;
			std::string_view val;
			size_t pos = 0;
			size_t length = str.length();
			for (size_t i = 0; i < length; i++) {
				char c = str[i];
				if (c == '=') {
					key = { &str[pos], i - pos };
					key = trim(key);
					pos = i + 1;
				}
				else if (c == '&') {
					val = { &str[pos], i - pos };
					val = trim(val);
					pos = i + 1;
					//if (is_form_url_encode(key)) {
					//	auto s = form_urldecode(key);
					//}
					query.emplace(key, val);
				}
			}

			if (pos == 0) {
				return {};
			}

			val = { &str[pos], length - pos };
			val = trim(val);
			query.emplace(key, val);
			return query;
		}

		bool parse_form_urlencoded() {
			form_url_map_.clear();
			if (has_gzip_) {
				bool r = uncompress();
				if (!r)
					return false;
			}

            auto body_str = body();
            form_url_map_ = parse_query(body_str);
            if(form_url_map_.empty())
                return false;

			return true;
		}

		int parse_chunked(size_t bytes_transferred) {
			auto str = std::string_view(&buf_[header_len_], bytes_transferred - header_len_);

			return -1;
		}

		std::string_view get_method() const{
			if (method_len_ != 0)
				return { method_ , method_len_ };

			return { method_str_.data(), method_str_.length() };
		}

		std::string_view get_url() const {
			if (method_len_ != 0)
				return { url_, url_len_ };

			return { url_str_.data(), url_str_.length() };
		}

		std::map<std::string_view, std::string_view> get_form_url_map() const{
			return form_url_map_;
		}

		void set_state(data_proc_state state) {
			state_ = state;
		}

		data_proc_state get_state() const{
			return state_;
		}

		void set_part_data(std::string_view data) {
			if (has_gzip_) {
				bool r = uncompress(data);
				if (!r)
					return;
			}
			
			part_data_ = data;
		}

		std::string_view get_part_data() const{
			if (has_gzip_) {
				return { gzip_str_.data(), gzip_str_.length() };
			}

			return part_data_;
		}

		void set_http_type(http_type type) {
			http_type_ = type;
		}

		http_type get_http_type() const {
			return http_type_;
		}

        const std::map<std::string_view, std::string_view>& queries() const{
            return queries_;
        }

		std::string_view get_query_value(std::string_view key) const{
			auto it = queries_.find(key);
			if (it == queries_.end()) {
				auto itf = form_url_map_.find(key);
				if (itf == form_url_map_.end())
					return {};

				return itf->second;
			}

			return it->second;
		}

		bool uncompress(std::string_view str) {
			if (str.empty())
				return false;

			gzip_str_.clear();
			return gzip_codec::uncompress(str, gzip_str_);
		}

		bool uncompress() {
			gzip_str_.clear();
			return gzip_codec::uncompress(std::string_view(&buf_[header_len_], body_len_), gzip_str_);
		}

	private:
		void resize_double() {
			size_t size = buf_.size();
			resize(2 * size);
		}

		void resize(size_t size) {
			copy_method_url();
			buf_.resize(size);
		}

		void copy_method_url() {
			if (method_len_ == 0)
				return;

			method_str_ = std::string( method_, method_len_ );
			url_str_ = std::string(url_, url_len_);
			method_len_ = 0;
			url_len_ = 0;
		}

		void check_gizp() {
			auto encoding = get_header_value("content-encoding");
			if (encoding.empty()) {
				has_gzip_ = false;
			}
			else {
				auto it = encoding.find("gzip");
				has_gzip_ = (it != std::string_view::npos);
			}
		}

		constexpr const static size_t MaxSize = 3 * 1024 * 1024;
		conn_type* con_ = nullptr;

		std::vector<char> buf_;

		size_t num_headers_ = 0;
		struct phr_header headers_[32];
		const char *method_ = nullptr;
		size_t method_len_ = 0;
		const char *url_ = nullptr;
		size_t url_len_ = 0;
		int minor_version_;
		int header_len_;
		size_t body_len_;

		std::string method_str_;
		std::string url_str_;

		size_t cur_size_ = 0;
		size_t left_body_len_ = 0;

        std::map<std::string_view, std::string_view> queries_;
		std::map<std::string_view, std::string_view> form_url_map_;

		bool has_gzip_ = false;
		std::string gzip_str_;

		bool is_chunked_ = false;

		data_proc_state state_ = data_proc_state::data_begin;
		std::string_view part_data_;
		http_type http_type_ = http_type::unknown;

		const std::multimap<std::string_view, std::string_view>* multipart_headers_;
	};
}