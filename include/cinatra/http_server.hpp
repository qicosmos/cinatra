#pragma once
#include "use_asio.hpp"
#include <string>
#include <vector>
#include <string_view>

#ifdef _MSC_VER
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif
#include "io_service_pool.hpp"
#include "connection.hpp"
#include "http_router.hpp"
#include "router.hpp"
#include "function_traits.hpp"
#include "url_encode_decode.hpp"
#include "http_cache.hpp"
#include "session_manager.hpp"
#include "cookie.hpp"

namespace cinatra {
	
	//cache
	template<typename T>
	struct enable_cache {
		enable_cache(T t) :value(t) {}
		T value;
	};

	template<class service_pool_policy = io_service_pool>
	class http_server_ : private noncopyable {
	public:
		template<class... Args>
		explicit http_server_(Args&&... args) : io_service_pool_(std::forward<Args>(args)...)
#ifdef CINATRA_ENABLE_SSL
			, ctx_(boost::asio::ssl::context::sslv23)
#endif
		{
			http_cache::get().set_cache_max_age(86400);
			init_conn_callback();
		}

		void enable_http_cache(bool b) {
			http_cache::get().enable_cache(b);
		}

		template<typename F>
		void init_ssl_context(bool ssl_enable_v3, F&& f, std::string certificate_chain_file,
			std::string private_key_file, std::string tmp_dh_file) {
#ifdef CINATRA_ENABLE_SSL
			unsigned long ssl_options = boost::asio::ssl::context::default_workarounds
				| boost::asio::ssl::context::no_sslv2
				| boost::asio::ssl::context::single_dh_use;

			if (!ssl_enable_v3)
				ssl_options |= boost::asio::ssl::context::no_sslv3;

			ctx_.set_options(ssl_options);
			ctx_.set_password_callback(std::forward<F>(f));
			ctx_.use_certificate_chain_file(std::move(certificate_chain_file));
			ctx_.use_private_key_file(std::move(private_key_file), boost::asio::ssl::context::pem);
			ctx_.use_tmp_dh_file(std::move(tmp_dh_file));
#endif
		}

		//address :
		//		"0.0.0.0" : ipv4. use 'https://localhost/' to visit
		//		"::1" : ipv6. use 'https://[::1]/' to visit
		//		"" : ipv4 & ipv6.
		bool listen(std::string_view address, std::string_view port) {
			boost::asio::ip::tcp::resolver::query query(address.data(), port.data());
			return listen(query);
		}

		//support ipv6 & ipv4
		bool listen(std::string_view port) {
			boost::asio::ip::tcp::resolver::query query(port.data());
			return listen(query);
		}

		bool listen(const boost::asio::ip::tcp::resolver::query & query) {
			boost::asio::ip::tcp::resolver resolver(io_service_pool_.get_io_service());
			boost::asio::ip::tcp::resolver::iterator endpoints = resolver.resolve(query);

			bool r = false;

			for (; endpoints != boost::asio::ip::tcp::resolver::iterator(); ++endpoints) {
				boost::asio::ip::tcp::endpoint endpoint = *endpoints;

				auto acceptor = std::make_shared<boost::asio::ip::tcp::acceptor>(io_service_pool_.get_io_service());
				acceptor->open(endpoint.protocol());
				acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

				try {
					acceptor->bind(endpoint);
					acceptor->listen();
					start_accept(acceptor);
					r = true;
				}
				catch (const std::exception& ex) {
					std::cout << ex.what() << "\n";
					//LOG_INFO << e.what();
				}
			}

			return r;
		}

		void stop() {
			io_service_pool_.stop();
		}

		void run() {
			if (!fs::exists(public_root_path_.data())) {
				fs::create_directories(public_root_path_.data());
			}

			if (!fs::exists(static_dir_.data())) {
				fs::create_directories(static_dir_.data());
			}

			io_service_pool_.run();
		}

		intptr_t run_one() {
			return io_service_pool_.run_one();
		}

		intptr_t poll() {
			return io_service_pool_.poll();
		}

		intptr_t poll_one() {
			return io_service_pool_.poll_one();
		}

		void set_static_dir(std::string&& path) {
			static_dir_ = public_root_path_+std::move(path)+"/";
		}

		const std::string& static_dir() const {
			return static_dir_;
		}

		//xM
		void set_max_req_buf_size(std::size_t max_buf_size) {
			max_req_buf_size_ = max_buf_size;
		}

		void set_keep_alive_timeout(long seconds) {
			keep_alive_timeout_ = seconds;
		}

		template<typename T>
		bool need_cache(T&& t) {
			if constexpr(std::is_same_v<T, enable_cache<bool>>) {
				return t.value;
			}
			else {
				return false;
			}
		}

		//set http handlers
		template<http_method... Is, typename Function, typename... AP>
		void set_http_handler(std::string_view name, Function&& f, AP&&... ap) {
			if constexpr(has_type<enable_cache<bool>, std::tuple<std::decay_t<AP>...>>::value) {//for cache
				bool b = false;
				((!b&&(b = need_cache(std::forward<AP>(ap)))),...);
				if (!b) {
					http_cache::get().add_skip(name);
				}else{
					http_cache::get().add_single_cache(name);
				}
				auto tp = filter<enable_cache<bool>>(std::forward<AP>(ap)...);
				auto lm = [this, name, f = std::move(f)](auto... ap) {
					http_router_.register_handler<Is...>(name, std::move(f), std::move(ap)...);
				};
				std::apply(lm, std::move(tp));
			}
			else {
				http_router_.register_handler<Is...>(name, std::forward<Function>(f), std::forward<AP>(ap)...);
			}
		}

        void set_base_path(const std::string& key,const std::string& path)
        {
            base_path_[0] = std::move(key);
            base_path_[1] = std::move(path);
        }

        void set_res_cache_max_age(std::time_t seconds)
        {
            static_res_cache_max_age_ = seconds;
        }

        std::time_t get_res_cache_max_age()
        {
            return static_res_cache_max_age_;
        }

        void set_cache_max_age(std::time_t seconds)
		{
			http_cache::get().set_cache_max_age(seconds);
		}

		std::time_t get_cache_max_age()
		{
			return http_cache::get().get_cache_max_age();
		}

		//don't begin with "./" or "/", not absolutely path
		void set_public_root_directory(const std::string& name)
        {
        	if(!name.empty()){
				public_root_path_ = "./"+name+"/";
			}
			else {
				public_root_path_ = "./";
			}
        }

        std::string get_public_root_directory()
        {
            return public_root_path_;
        }

		void set_download_check(std::function<bool(request& req, response& res)> checker) {
			download_check_ = std::move(checker);
		}

		//should be called before listen
		void set_upload_check(std::function<bool(request& req, response& res)> checker) {
			upload_check_ = std::move(checker);
		}

		void mapping_to_root_path(std::string relate_path) {
			relate_paths_.emplace_back("."+std::move(relate_path));
		}

		void set_not_found_handler(std::function<void(request& req, response& res)> not_found) {
			not_found_ = std::move(not_found);
		}

	private:
		void start_accept(std::shared_ptr<boost::asio::ip::tcp::acceptor> const& acceptor) {
			auto new_conn = std::make_shared<connection<Socket>>(
				io_service_pool_.get_io_service(), max_req_buf_size_, keep_alive_timeout_, http_handler_, static_dir_, 
				upload_check_?&upload_check_ : nullptr
#ifdef CINATRA_ENABLE_SSL
				, ctx_
#endif
				);
			acceptor->async_accept(new_conn->socket(), [this, new_conn, acceptor](const boost::system::error_code& e) {
				if (!e) {
					new_conn->socket().set_option(boost::asio::ip::tcp::no_delay(true));
					new_conn->start();
				}
				else {
					//LOG_INFO << "server::handle_accept: " << e.message();
				}

				start_accept(acceptor);
			});
		}

		void set_static_res_handler()
		{
			set_http_handler<POST,GET>(STATIC_RESOURCE, [this](request& req, response& res){
				if (download_check_) {
					bool r = download_check_(req, res);
					if (!r)
						return;
				}

				auto state = req.get_state();
				switch (state) {
					case cinatra::data_proc_state::data_begin:
					{
						std::string relatice_file_name = req.get_relative_filename();
						auto mime = req.get_mime({ relatice_file_name.data(), relatice_file_name.length()});
						std::wstring source_path = fs::absolute(relatice_file_name).filename().wstring();
						std::wstring root_path = fs::absolute(public_root_path_).filename().wstring();
						if(source_path.find(fs::absolute(root_path).filename().wstring()) ==std::string::npos){
							auto it = std::find_if(relate_paths_.begin(), relate_paths_.end(), [this, &relatice_file_name](auto& str) {
								auto pos = relatice_file_name.find(str);
								if (pos != std::string::npos) {
									relatice_file_name = relatice_file_name.replace(0, str.size(), 
										public_root_path_.substr(0, public_root_path_.size() - 1));
								}
								return pos !=std::string::npos;
							});
							if (it == relate_paths_.end()) {
								res.set_status_and_content(status_type::forbidden);
								return;
							}
						}
						
						auto in = std::make_shared<std::ifstream>(relatice_file_name,std::ios_base::binary);
						if (!in->is_open()) {
							if (not_found_) {
								not_found_(req, res);
								return;
							}
							res.set_status_and_content(status_type::not_found,"");
							return;
						}
                        
						if(is_small_file(in.get(),req)){
							send_small_file(res, in.get(), mime);
							return;
						}

						write_chunked_header(req, in, mime);
					}
						break;
					case cinatra::data_proc_state::data_continue:
					{
						write_chunked_body(req);
					}
						break;
					case cinatra::data_proc_state::data_end:
					{
						auto conn = req.get_conn();
						conn->on_close();
					}
						break;
					case cinatra::data_proc_state::data_error:
					{
						//network error
					}
						break;
				}
			},enable_cache{false});
		}

		bool is_small_file(std::ifstream* in,request& req) const {
			auto file_begin = in->tellg();
			in->seekg(0, std::ios_base::end);
			auto  file_size = in->tellg();
			in->seekg(file_begin);
			req.save_request_static_file_size(file_size);
			return file_size <= 5 * 1024 * 1024;
		}

		void send_small_file(response& res, std::ifstream* in, std::string_view mime) {
			res.add_header("Access-Control-Allow-origin", "*");
			res.add_header("Content-type", std::string(mime.data(), mime.size()) + "; charset=utf8");
			std::stringstream file_buffer;
			file_buffer << in->rdbuf();
			if (static_res_cache_max_age_>0)
			{
				std::string max_age = std::string("max-age=") + std::to_string(static_res_cache_max_age_);
				res.add_header("Cache-Control", max_age.data());
			}
#ifdef CINATRA_ENABLE_GZIP
			res.set_status_and_content(status_type::ok, file_buffer.str(), res_content_type::none, content_encoding::gzip);
#else
			res.set_status_and_content(status_type::ok, file_buffer.str());
#endif
		}

		void write_chunked_header(request& req, std::shared_ptr<std::ifstream> in, std::string_view mime) {
			auto range_header = req.get_header_value("range");
			req.set_range_flag(!range_header.empty());
			req.set_range_start_pos(range_header);

			std::string res_content_header = std::string(mime.data(), mime.size()) + "; charset=utf8";
			res_content_header += std::string("\r\n") + std::string("Access-Control-Allow-origin: *");
			res_content_header += std::string("\r\n") + std::string("Accept-Ranges: bytes");
			if (static_res_cache_max_age_>0)
			{
				std::string max_age = std::string("max-age=") + std::to_string(static_res_cache_max_age_);
				res_content_header += std::string("\r\n") + std::string("Cache-Control: ") + max_age;
			}
			auto conn = req.get_conn();
			conn->set_tag(in);
			if(req.is_range())
			{
				std::int64_t file_pos  = req.get_range_start_pos();
				in->seekg(file_pos);
				auto end_str = std::to_string(req.get_request_static_file_size());
				res_content_header += std::string("\r\n") +std::string("Content-Range: bytes ")+std::to_string(file_pos)+std::string("-")+std::to_string(req.get_request_static_file_size()-1)+std::string("/")+end_str;
			}
			conn->write_chunked_header(std::string_view(res_content_header.data(), res_content_header.size()),req.is_range());
		}

		void write_chunked_body(request& req) {
			auto conn = req.get_conn();
			auto in = std::any_cast<std::shared_ptr<std::ifstream>>(conn->get_tag());
			std::string str;
			const size_t len = 3 * 1024 * 1024;
			str.resize(len);
			in->read(&str[0], len);
			size_t read_len = (size_t)in->gcount();
			if (read_len != len) {
				str.resize(read_len);
			}
			bool eof = (read_len == 0 || read_len != len);
			conn->write_chunked_data(std::move(str), eof);
		}

		void init_conn_callback() {
            set_static_res_handler();
			http_handler_ = [this](request& req, response& res) {
                res.set_base_path(this->base_path_[0],this->base_path_[1]);
                res.set_url(req.get_url());
				try {
					bool success = http_router_.route(req.get_method(), req.get_url(), req, res);
					if (!success) {
						if (not_found_) {
							not_found_(req, res);
							return;
						}
						res.set_status_and_content(status_type::bad_request, "the url is not right");
					}
				}
				catch (const std::exception& ex) {
					res.set_status_and_content(status_type::internal_server_error, ex.what()+std::string(" exception in business function"));
				}
				catch (...) {
					res.set_status_and_content(status_type::internal_server_error, "unknown exception in business function");
				}				
			};
		}

		service_pool_policy io_service_pool_;

		std::size_t max_req_buf_size_ = 3 * 1024 * 1024; //max request buffer size 3M
		long keep_alive_timeout_ = 60; //max request timeout 60s

		http_router http_router_;
		std::string static_dir_ = "./public/static/"; //default
        std::string base_path_[2] = {"base_path","/"};
        std::time_t static_res_cache_max_age_ = 0;
        std::string public_root_path_ = "./";
//		https_config ssl_cfg_;
#ifdef CINATRA_ENABLE_SSL
		boost::asio::ssl::context ctx_;
#endif

		http_handler http_handler_ = nullptr;
		std::function<bool(request& req, response& res)> download_check_;
		std::vector<std::string> relate_paths_;
		std::function<bool(request& req, response& res)> upload_check_ = nullptr;

		std::function<void(request& req, response& res)> not_found_ = nullptr;
	};

	using http_server = http_server_<io_service_pool>;
}
