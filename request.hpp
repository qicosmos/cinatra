#pragma once
#include <fstream>
#include "picohttpparser.h"
#include "utils.hpp"
#include "multipart_reader.hpp"
#ifdef CINATRA_ENABLE_GZIP
#include "gzip.hpp"
#endif
#include "define.h"
#include "upload_file.hpp"
#include "memento.hpp"
#include "session.hpp"
#include "session_manager.hpp"
#include "url_encode_decode.hpp"
#include "mime_types.hpp"

namespace cinatra {
	enum class data_proc_state : int8_t {
		data_begin,
		data_continue,
		data_end,
		data_all_end,
		data_close,
		data_error
	};

	using tcp_socket = boost::asio::ip::tcp::socket;
	template <typename socket_type>
	class connection;
#ifdef CINATRA_ENABLE_SSL
	using Socket = boost::asio::ssl::stream<tcp_socket>;
#else
	using Socket = tcp_socket;
#endif

	using conn_type = connection<Socket>;

	class request {
	public:
		using event_call_back = std::function<void(request&)>;

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

			if (header_len_ <0 )
				return header_len_;

			check_gzip();
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
			raw_url_ = {url_, url_len_};
			size_t npos = raw_url_.find('/');
			if (npos == std::string_view::npos)
				return -1;

            size_t pos = raw_url_.find('?');
            if(pos!=std::string_view::npos){
                queries_ = parse_query(raw_url_.substr(pos+1, url_len_-pos-1));
				url_len_ = pos;
            }

			return header_len_;
		}

		std::string_view raw_url() {
			return raw_url_;
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
#ifdef CINATRA_ENABLE_GZIP
			if (has_gzip_&&!gzip_str_.empty()) {
				return { gzip_str_.data(), gzip_str_.length() };
			}
#endif
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
			files_.clear();
			is_chunked_ = false;
			state_ = data_proc_state::data_begin;
			part_data_ = {};
            utf8_character_params_.clear();
            utf8_character_pathinfo_params_.clear();
            queries_.clear();
            multipart_form_map_.clear();
			is_range_resource_ = false;
			range_start_pos_ = 0;
			static_resource_file_size_ = 0;
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
		
		//refactor later
        	void expand_size(){
            		auto total = total_len();
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

        std::string get_multipart_field_name(const std::string& field_name) const {
            if (multipart_headers_.empty())
                return {};

            auto it = multipart_headers_.begin();
            auto val = it->second;
            //auto pos = val.find("name");
			auto pos = val.find(field_name);
            if (pos == std::string::npos) {
                return {};
            }

            auto start = val.find('"', pos) + 1;
            auto end = val.rfind('"');
            if (start == std::string::npos || end == std::string::npos || end<start) {
                return {};
            }

            auto key_name = val.substr(start, end - start);
            return key_name;
        }

        void save_multipart_key_value(const std::string& key,const std::string& value)
        {
			if(!key.empty())
            multipart_form_map_.emplace(key,value);
        }

		void update_multipart_value(const std::string& key, const char* buf, size_t size) {
			auto it = multipart_form_map_.find(key);
			if (it != multipart_form_map_.end()) {
				multipart_form_map_[key] += std::string(buf, size);
			}
		}

        std::string get_multipart_value_by_key1(const std::string& key)
        {
			if (!key.empty()) {
				return multipart_form_map_[key];
			}

			return {};
        }

		void handle_multipart_key_value(){
			if(multipart_form_map_.empty()){
				return;
			}

			for (auto& pair : multipart_form_map_) {
				form_url_map_.emplace(std::string_view(pair.first.data(), pair.first.size()), 
					std::string_view(pair.second.data(), pair.second.size()));
			}
		}

        bool is_multipart_file() const {
            if (multipart_headers_.empty()){
                return false;
            }

			return multipart_headers_.find("Content-Type") != multipart_headers_.end();
        }

		void set_multipart_headers(const std::multimap<std::string_view, std::string_view>& headers) {
			for (auto pair : headers) {
				multipart_headers_[std::string(pair.first.data(), pair.first.size())] = std::string(pair.second.data(), pair.second.size());
			}
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

			if ((length - pos) > 0) {
				val = { &str[pos], length - pos };
				val = trim(val);
				query.emplace(key, val);
			}
			return query;
		}

		bool parse_form_urlencoded() {
			form_url_map_.clear();
#ifdef CINATRA_ENABLE_GZIP
			if (has_gzip_) {
				bool r = uncompress();
				if (!r)
					return false;
			}
#endif
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

		std::string_view get_res_path() const {
			auto url = get_url();

			return url.substr(1);
		}

		std::string get_filename_from_path() const {
			auto file_name = get_res_path();
			std::string real_file_name = std::string(file_name.data(), file_name.size());
			if (is_form_url_encode(file_name))
			{
				real_file_name = code_utils::get_string_by_urldecode(file_name);
			}

			return real_file_name;
		}

		std::string_view get_mime(std::string_view filename) const{
			auto extension = get_extension(filename.data());
			auto mime = get_mime_type(extension);
			return mime;
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
#ifdef CINATRA_ENABLE_GZIP
			if (has_gzip_) {
				bool r = uncompress(data);
				if (!r)
					return;
			}
#endif
			
			part_data_ = data;
		}

		std::string_view get_part_data() const{
			if (has_gzip_) {
				return { gzip_str_.data(), gzip_str_.length() };
			}

			return part_data_;
		}

		void set_http_type(content_type type) {
			http_type_ = type;
		}

		content_type get_content_type() const {
			return http_type_;
		}

        const std::map<std::string_view, std::string_view>& queries() const{
            return queries_;
        }

		std::string_view get_query_value(size_t n) {
			auto url = get_url();
			size_t tail = (url.back() == '/') ? 1 : 0;
			for (auto item : memento::pathinfo_mem) {
				if (url.find(item) != std::string_view::npos) {
					if (item.length() == url.length())
						return {};

					auto str = url.substr(item.length(), url.length() - item.length() - tail);
					auto params = split(str, "/");
					if (n >= params.size())
						return {};
					if(code_utils::is_url_encode(params[n]))
					{
                        auto map_url = url.length()>1 && url.back()=='/' ? url.substr(0,url.length()-1):url;
                        std::string map_key = std::string(map_url.data(),map_url.size())+ std::to_string(n);

						auto ret = utf8_character_pathinfo_params_.emplace(map_key, code_utils::get_string_by_urldecode(params[n]));
						return std::string_view(ret.first->second.data(), ret.first->second.size());
					}
					return params[n];
				}
			}

			return {};
		}

		std::string_view get_query_value(std::string_view key){
            auto url = get_url();
            url = url.length()>1 && url.back()=='/' ? url.substr(0,url.length()-1):url;
            std::string map_key = std::string(url.data(),url.size())+std::string(key.data(),key.size());
			auto it = queries_.find(key);
			if (it == queries_.end()) {
				auto itf = form_url_map_.find(key);
				if (itf == form_url_map_.end())
					return {};

				if(code_utils::is_url_encode(itf->second))
				{
					auto ret= utf8_character_params_.emplace(map_key, code_utils::get_string_by_urldecode(itf->second));
					return std::string_view(ret.first->second.data(), ret.first->second.size());
				}
				return itf->second;
			}
			if(code_utils::is_url_encode(it->second))
			{
				auto ret = utf8_character_params_.emplace(map_key, code_utils::get_string_by_urldecode(it->second));
				return std::string_view(ret.first->second.data(), ret.first->second.size());
			}
			return it->second;
		}

		bool uncompress(std::string_view str) {
			if (str.empty())
				return false;

			bool r = true;
#ifdef CINATRA_ENABLE_GZIP
			gzip_str_.clear();
			r = gzip_codec::uncompress(str, gzip_str_);
#endif
			return r;
		}

		bool uncompress() {
			bool r = true;
#ifdef CINATRA_ENABLE_GZIP
			gzip_str_.clear();
			r = gzip_codec::uncompress(std::string_view(&buf_[header_len_], body_len_), gzip_str_);
#endif
			return r;
		}

		bool open_upload_file(const std::string& filename) {
			upload_file file;
			bool r = file.open(filename);
			if (!r)
				return false;
			
			files_.push_back(std::move(file));
			return true;
		}

		void write_upload_data(const char* data, size_t size) {
			if (size == 0)
				return;

			assert(!files_.empty());

			files_.back().write(data, size);
		}

		void close_upload_file() {
			if (files_.empty())
				return;

			files_.back().close();
		}

		const std::vector<upload_file>& get_upload_files() const {
			return files_;
		}

		std::map<std::string_view, std::string_view> get_cookies() const
		{
			auto cookies_str = get_header_value("cookie");
			auto cookies = get_cookies_map(cookies_str);
            return cookies;
		}

        std::weak_ptr<session> get_session(const std::string& name) const
		{
			auto cookies = get_cookies();
			auto iter = cookies.find(name);
			if(iter==cookies.end())
			{
				return {};
			}
			return session_manager::get_session(std::string(iter->second.data(), iter->second.length()));
		}

		std::weak_ptr<session> get_session() const
		{
			return get_session(CSESSIONID);
		}

		void set_range_flag(bool flag)
		{
			is_range_resource_ = flag;
		}

		bool is_range() const
		{
			return is_range_resource_;
		}

		void set_range_start_pos(std::string_view range_header)
		{
			if(is_range_resource_)
			{
               auto l_str_pos = range_header.find("=");
               auto r_str_pos = range_header.rfind("-");
               auto pos_str = range_header.substr(l_str_pos+1,r_str_pos-l_str_pos-1);
               range_start_pos_ = std::atoll(pos_str.data());
			}
		}

		std::int64_t get_range_start_pos() const
        {
            if(is_range_resource_){
                return  range_start_pos_;
            }
            return 0;
        }

        void save_request_static_file_size(std::int64_t size)
		{
			static_resource_file_size_ = size;
		}

		std::int64_t get_request_static_file_size() const
		{
			return static_resource_file_size_;
		}

		void on(data_proc_state event_type, event_call_back&& event_call_back)
		{
			event_call_backs_[(size_t)event_type] = std::move(event_call_back);
		}

		void call_event(data_proc_state event_type) {
			event_call_backs_[(size_t)event_type](*this);
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
			multipart_headers_.clear();
		}

		void check_gzip() {
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

		std::string_view raw_url_;
		std::string method_str_;
		std::string url_str_;

		size_t cur_size_ = 0;
		size_t left_body_len_ = 0;

        std::map<std::string_view, std::string_view> queries_;
		std::map<std::string_view, std::string_view> form_url_map_;
        std::map<std::string,std::string> multipart_form_map_;
		bool has_gzip_ = false;
		std::string gzip_str_;

		bool is_chunked_ = false;

		data_proc_state state_ = data_proc_state::data_begin;
		std::string_view part_data_;
		content_type http_type_ = content_type::unknown;

		std::map<std::string, std::string> multipart_headers_;
		std::vector<upload_file> files_;
		std::map<std::string,std::string> utf8_character_params_;
		std::map<std::string,std::string> utf8_character_pathinfo_params_;
		std::int64_t range_start_pos_;
		bool is_range_resource_ = 0;
		std::int64_t static_resource_file_size_ = 0;
		std::array<event_call_back, (size_t)data_proc_state::data_error + 1> event_call_backs_ = {};
	};
}
