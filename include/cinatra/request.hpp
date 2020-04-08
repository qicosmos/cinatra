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
#include "session.hpp"
#include "session_manager.hpp"
#include "url_encode_decode.hpp"
#include "mime_types.hpp"
#include "response.hpp"
namespace cinatra {
	enum class data_proc_state : int8_t {
		data_begin,
		data_continue,
		data_end,
		data_all_end,
		data_close,
		data_error
	};

    class base_connection;
	template <typename T>
	class connection;

    using conn_type = std::weak_ptr<base_connection>;
    class request;
    using check_header_cb = std::function<bool(request&)>;

	class request {
	public:
		using event_call_back = std::function<void(request&)>;
        
		request(response& res) : res_(res){
			buf_.resize(1024);
		}

        void set_conn(conn_type conn) {
            conn_ = std::move(conn);
        }

        template<typename T>
        connection<T>* get_conn() {
            auto base_conn = conn_.lock();
            if (base_conn == nullptr)
                return nullptr;

            connection<T>* ptr = (connection<T>*)(base_conn.get());
            return ptr;
		}

        conn_type get_weak_base_conn() {
            return conn_;
        }

		int parse_header(std::size_t last_len, size_t start=0) {
			using namespace std::string_view_literals;
			if(!copy_headers_.empty())
				copy_headers_.clear();
			num_headers_ = sizeof(headers_) / sizeof(headers_[0]);
			header_len_ = phr_parse_request(buf_.data(), cur_size_, &method_,
				&method_len_, &url_, &url_len_,
				&minor_version_, headers_, &num_headers_, last_len);

            if (cur_size_ > max_header_len_) {
                return -1;
            }

			if (header_len_ <0 )
				return header_len_;

            if (check_headers_) {
                bool r = check_headers_(*this);
                if (!r) {
                    return -1;
                }
            }

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
				set_body_len(atoll(header_value.data()));
			}

			auto cookie = get_header_value("cookie");
			if (!cookie.empty()) {
				cookie_str_ = std::string(cookie.data(), cookie.length());
			}

            //parse url and queries
			raw_url_ = {url_, url_len_};
			size_t npos = raw_url_.find('/');
			if (npos == std::string_view::npos)
				return -1;

            size_t pos = raw_url_.find('?');
            if(pos!=std::string_view::npos){
                queries_ = parse_query(std::string_view{raw_url_}.substr(pos+1, url_len_-pos-1));
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
			return (total_len() <= current_size());
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

		const char* data() {
			return buf_.data();
		}

		const size_t last_len() const {
			return last_len_;
		}

		std::string_view req_buf() {
			return std::string_view(buf_.data() + last_len_, total_len());
		}

		std::string_view head() {
			return std::string_view(buf_.data() + last_len_, header_len_);
		}

		std::string_view body() {
			return std::string_view(buf_.data() + last_len_ + header_len_, body_len_);
		}

		void set_left_body_size(size_t size) {
			left_body_len_ = size;
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
			for (auto& file : files_) {
				file.close();
			}
			files_.clear();
			is_chunked_ = false;
			state_ = data_proc_state::data_begin;
			part_data_ = {};
            utf8_character_params_.clear();
            utf8_character_pathinfo_params_.clear();
            queries_.clear();
			cookie_str_.clear();
			form_url_map_.clear();
            multipart_form_map_.clear();
			is_range_resource_ = false;
			range_start_pos_ = 0;
			static_resource_file_size_ = 0;
			copy_headers_.clear();
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

        int minor_version() {
            return minor_version_;
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
				copy_method_url_headers();
			}
		}

		std::string_view get_header_value(std::string_view key) const {
			if (copy_headers_.empty()) {
				for (size_t i = 0; i < num_headers_; i++) {
					if (iequal(headers_[i].name, headers_[i].name_len, key.data(), key.length()))
						return std::string_view(headers_[i].value, headers_[i].value_len);
				}

				return {};
			}

			auto it = std::find_if(copy_headers_.begin(), copy_headers_.end(), [key] (auto& pair){
				if (iequal(pair.first.data(), pair.first.size(), key.data())) {
					return true;
				}

				return false;
			});

			if (it != copy_headers_.end()) {
				return (*it).second;
			}

			return {};
		}

		std::pair<phr_header*, size_t> get_headers() {
			if(copy_headers_.empty())
				return { headers_ , num_headers_ };

			num_headers_ = copy_headers_.size();
			for (size_t i = 0; i < num_headers_; i++) {
				headers_[i].name = copy_headers_[i].first.data();
				headers_[i].name_len = copy_headers_[i].first.size();
				headers_[i].value = copy_headers_[i].second.data();
				headers_[i].value_len = copy_headers_[i].second.size();
			}
			return { headers_ , num_headers_ };
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

		void update_multipart_value(std::string key, const char* buf, size_t size) {
			if (!key.empty()) {
				last_multpart_key_ = key;
			}
			else {
				key = last_multpart_key_;
			}

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

			bool has_content_type = (multipart_headers_.find("Content-Type") != multipart_headers_.end());
			auto it = multipart_headers_.find("Content-Disposition");
			bool has_content_disposition = (it != multipart_headers_.end());
			if (has_content_disposition) {
				if (it->second.find("filename") != std::string::npos) {
					return true;
				}

				return false;
			}

			return has_content_type|| has_content_disposition;
        }

		void set_multipart_headers(const multipart_headers& headers) {
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
			else if((length - pos) == 0) {
				query.emplace(key, "");
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

		std::string get_relative_filename() const {
			auto file_name = get_url();
			if (is_form_url_encode(file_name)){
				return code_utils::get_string_by_urldecode(file_name);
			}
			
			return std::string(file_name);
		}

		std::string get_filename_from_path() const {
			auto file_name = get_res_path();
			if (is_form_url_encode(file_name)) {
				return code_utils::get_string_by_urldecode(file_name);
			}

			return std::string(file_name.data(), file_name.size());
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
            auto get_val = [&n](auto& map) {
                auto it = map.begin();
                std::advance(it, n);
                return it->second;
            };

            if (n >= queries_.size() ) {
                if(n >= form_url_map_.size())
                    return {};

                return get_val(form_url_map_);
            }
            else {
                return get_val(queries_);
            }
		}

		template<typename T>
		T get_query_value(std::string_view key) {
			static_assert(std::is_arithmetic_v<T>);
			auto val = get_query_value(key);
			if (val.empty()) {
				throw std::logic_error("empty value");
			}

			if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t> ||
				std::is_same_v<T, bool> || std::is_same_v<T, char> || std::is_same_v<T, short>) {
				int r = std::atoi(val.data());
				if (val[0] != '0' && r == 0) {
					throw std::invalid_argument(std::string(val) +": is not an integer");
				}
				return r;
			}
			else if constexpr (std::is_same_v<T, int64_t>|| std::is_same_v<T, uint64_t>) {
				auto r = std::atoll(val.data());
				if (val[0] != '0' && r == 0) {
					throw std::invalid_argument(std::string(val) + ": is not an integer");
				}
				return r;
			}
			else if constexpr (std::is_floating_point_v<T>) {
				char* end;
				auto f = strtof(val.data(), &end);
				if (val.back() != *(end-1)) {
					throw std::invalid_argument(std::string(val) + ": is not a float");
				}
				return f;
			}
			else {
				throw std::invalid_argument("not support the value type");
			}
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

		upload_file* get_file() {
			if(!files_.empty())
				return &files_.back();

			return nullptr;
		}

		std::map<std::string_view, std::string_view> get_cookies() const
		{
			//auto cookies_str = get_header_value("cookie");
			auto cookies = get_cookies_map(cookie_str_);
            return cookies;
		}

        std::weak_ptr<session> get_session(const std::string& name)
		{
			auto cookies = get_cookies();
			auto iter = cookies.find(name);
			std::weak_ptr<session> ref;
			if(iter!=cookies.end())
			{
				ref = session_manager::get().get_session(std::string(iter->second.data(), iter->second.length()));
			}
			res_.set_session(ref);
			return ref;
		}

		std::weak_ptr<session> get_session()
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
			if(event_call_backs_[(size_t)event_type])
			event_call_backs_[(size_t)event_type](*this);
		}

		template<typename... T>
		void set_aspect_data(T&&... data) {
			(aspect_data_.push_back(std::forward<T>(data)), ...);
		}

		void set_aspect_data(std::vector<std::string>&& data) {
			aspect_data_ = std::move(data);
		}

		std::vector<std::string> get_aspect_data() {
			return std::move(aspect_data_);
		}

		void set_last_len(size_t len) {
			last_len_ = len;
		}

        void set_validate(size_t max_header_len, check_header_cb check_headers) {
            max_header_len_ = max_header_len;
            check_headers_ = std::move(check_headers);
        }

	private:
		void resize_double() {
			size_t size = buf_.size();
			resize(2 * size);
		}

		void resize(size_t size) {
			copy_method_url_headers();
			buf_.resize(size);
		}

		void copy_method_url_headers() {
			if (method_len_ == 0)
				return;

			method_str_ = std::string( method_, method_len_ );
			url_str_ = std::string(url_, url_len_);
			method_len_ = 0;
			url_len_ = 0;

			auto filename = get_multipart_field_name("filename");
			multipart_headers_.clear();
			if (!filename.empty()) {
				copy_headers_.emplace_back("filename", std::move(filename));
			}			

			if (header_len_ < 0)
				return;

			for (size_t i = 0; i < num_headers_; i++) {
				copy_headers_.emplace_back(std::string(headers_[i].name, headers_[i].name_len), 
					std::string(headers_[i].value, headers_[i].value_len));
			}
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
        conn_type conn_;
        response& res_;
		std::vector<char> buf_;

		size_t num_headers_ = 0;
		struct phr_header headers_[32];
		const char *method_ = nullptr;
		size_t method_len_ = 0;
		const char *url_ = nullptr;
		size_t url_len_ = 0;
		int minor_version_ = 0;
		int header_len_ = 0;
		size_t body_len_ = 0;

		std::string raw_url_;
		std::string method_str_;
		std::string url_str_;
		std::string cookie_str_;
		std::vector<std::pair<std::string, std::string>> copy_headers_;

		size_t cur_size_ = 0;
		size_t left_body_len_ = 0;

		size_t last_len_ = 0; //for pipeline, last request buffer position

        std::map<std::string_view, std::string_view> queries_;
		std::map<std::string_view, std::string_view> form_url_map_;
        std::map<std::string,std::string> multipart_form_map_;
		bool has_gzip_ = false;
		std::string gzip_str_;

		bool is_chunked_ = false;

        //validate
        size_t max_header_len_ = 1024 * 1024;
        check_header_cb check_headers_;

		data_proc_state state_ = data_proc_state::data_begin;
		std::string_view part_data_;
		content_type http_type_ = content_type::unknown;

		std::map<std::string, std::string> multipart_headers_;
		std::string last_multpart_key_;
		std::vector<upload_file> files_;
		std::map<std::string,std::string> utf8_character_params_;
		std::map<std::string,std::string> utf8_character_pathinfo_params_;
		std::int64_t range_start_pos_ = 0;
		bool is_range_resource_ = 0;
		std::int64_t static_resource_file_size_ = 0;
		std::vector<std::string> aspect_data_;
		std::array<event_call_back, (size_t)data_proc_state::data_error + 1> event_call_backs_ = {};
	};
}
