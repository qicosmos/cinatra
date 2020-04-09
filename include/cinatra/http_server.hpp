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

	template<typename ScoketType, class service_pool_policy = io_service_pool>
	class http_server_ : private noncopyable {
	public:
        using type = ScoketType;
		template<class... Args>
		explicit http_server_(Args&&... args) : io_service_pool_(std::forward<Args>(args)...)
		{
			http_cache::get().set_cache_max_age(86400);
			init_conn_callback();
		}

		void enable_http_cache(bool b) {
			http_cache::get().enable_cache(b);
		}

        void set_ssl_conf(ssl_configure conf) {
            ssl_conf_ = std::move(conf);
        }

        bool port_in_use(unsigned short port) {
            using namespace boost::asio;
            using ip::tcp;

            io_service svc;
            tcp::acceptor a(svc);

            boost::system::error_code ec;
            a.open(tcp::v4(), ec) || a.bind({ tcp::v4(), port }, ec);

            return ec == error::address_in_use;
        }

		//address :
		//		"0.0.0.0" : ipv4. use 'https://localhost/' to visit
		//		"::1" : ipv6. use 'https://[::1]/' to visit
		//		"" : ipv4 & ipv6.
		bool listen(std::string_view address, std::string_view port) {
            if (port_in_use(atoi(port.data()))) {
                return false;
            }

			boost::asio::ip::tcp::resolver::query query(address.data(), port.data());
			return listen(query);
		}

		//support ipv6 & ipv4
		bool listen(std::string_view port) {
            if (port_in_use(atoi(port.data()))) {
                return false;
            }

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
            init_dir(static_dir_);
            init_dir(upload_dir_);

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

		void set_static_dir(std::string path) {
            set_file_dir(std::move(path), static_dir_);
		}

        void set_upload_dir(std::string path) {
            set_file_dir(std::move(path), upload_dir_);
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

		void set_multipart_begin(std::function<void(request&, std::string&)> begin) {
			multipart_begin_ = std::move(begin);
		}

        void set_validate(size_t max_header_len, check_header_cb check_headers) {
            max_header_len_ = max_header_len;
            check_headers_ = std::move(check_headers);
        }

		void enable_timeout(bool enable){
			enable_timeout_ = enable;
		}

        void enable_response_time(bool enable) {
            need_response_time_ = enable;
        }

        void set_transfer_type(transfer_type type) {
            transfer_type_ = type;
        }

	private:
		void start_accept(std::shared_ptr<boost::asio::ip::tcp::acceptor> const& acceptor) {
			auto new_conn = std::make_shared<connection<ScoketType>>(
				io_service_pool_.get_io_service(), ssl_conf_, max_req_buf_size_, keep_alive_timeout_, http_handler_, upload_dir_,
				upload_check_?&upload_check_ : nullptr
			);

			acceptor->async_accept(new_conn->tcp_socket(), [this, new_conn, acceptor](const boost::system::error_code& e) {
				if (!e) {
					new_conn->tcp_socket().set_option(boost::asio::ip::tcp::no_delay(true));
                    if (multipart_begin_) {
                        new_conn->set_multipart_begin(multipart_begin_);
                    }
					
                    new_conn->enable_response_time(need_response_time_);
					new_conn->enable_timeout(enable_timeout_);

                    if (check_headers_) {
                        new_conn->set_validate(max_header_len_, check_headers_);
                    }
                    
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
					if (!r) {
						res.set_status_and_content(status_type::bad_request);
						return;
					}						
				}

				auto state = req.get_state();
				switch (state) {
					case cinatra::data_proc_state::data_begin:
					{
						std::string relative_file_name = req.get_relative_filename();
						std::string fullpath = static_dir_ + relative_file_name;

						auto mime = req.get_mime(relative_file_name);
						auto in = std::make_shared<std::ifstream>(fullpath, std::ios_base::binary);
						if (!in->is_open()) {
							if (not_found_) {
								not_found_(req, res);
								return;
							}
							res.set_status_and_content(status_type::not_found,"");
							return;
						}

                        req.get_conn<ScoketType>()->set_tag(in);
                        
						if(is_small_file(in.get(),req)){
							send_small_file(res, in.get(), mime);
							return;
						}

                        if(transfer_type_== transfer_type::CHUNKED)
						    write_chunked_header(req, in, mime);
                        else
                            write_ranges_header(req, mime, fs::path(relative_file_name).filename().string(), std::to_string(fs::file_size(fullpath)));
					}
						break;
					case cinatra::data_proc_state::data_continue:
					{
                        if (transfer_type_ == transfer_type::CHUNKED)
                            write_chunked_body(req);
                        else
                            write_ranges_data(req);
					}
						break;
					case cinatra::data_proc_state::data_end:
					{
						auto conn = req.get_conn<ScoketType>();
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
			
			if(req.is_range())
			{
				std::int64_t file_pos  = req.get_range_start_pos();
				in->seekg(file_pos);
				auto end_str = std::to_string(req.get_request_static_file_size());
				res_content_header += std::string("\r\n") +std::string("Content-Range: bytes ")+std::to_string(file_pos)+std::string("-")+std::to_string(req.get_request_static_file_size()-1)+std::string("/")+end_str;
			}
            req.get_conn<ScoketType>()->write_chunked_header(std::string_view(res_content_header),req.is_range());
		}

		void write_chunked_body(request& req) {
            const size_t len = 3 * 1024 * 1024;
            auto str = get_send_data(req, len);
            auto read_len = str.size();
            bool eof = (read_len == 0 || read_len != len);
            req.get_conn<ScoketType>()->write_chunked_data(std::move(str), eof);
		}

        void write_ranges_header(request& req, std::string_view mime, std::string filename, std::string file_size) {
            std::string header_str = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-origin: *\r\nAccept-Ranges: bytes\r\n";
            header_str.append("Content-Disposition: attachment;filename=");
            header_str.append(std::move(filename)).append("\r\n");
            header_str.append("Connection: keep-alive\r\n");
            header_str.append("Content-Type: ").append(mime).append("\r\n");
            header_str.append("Content-Length: ");
            header_str.append(file_size).append("\r\n\r\n");
            req.get_conn<ScoketType>()->write_ranges_header(std::move(header_str));
        }

        void write_ranges_data(request& req) {
            const size_t len = 3 * 1024 * 1024;
            auto str = get_send_data(req, len);
            auto read_len = str.size();
            bool eof = (read_len == 0 || read_len != len);
            req.get_conn<ScoketType>()->write_ranges_data(std::move(str), eof);
        }

        std::string get_send_data(request& req, const size_t len) {
            auto conn = req.get_conn<ScoketType>();
            auto in = std::any_cast<std::shared_ptr<std::ifstream>>(conn->get_tag());
            std::string str;
            str.resize(len);
            in->read(&str[0], len);
            size_t read_len = (size_t)in->gcount();
            if (read_len != len) {
                str.resize(read_len);
            }

            return str;
        }

		void init_conn_callback() {
            set_static_res_handler();
			http_handler_ = [this](request& req, response& res) {
				res.set_headers(req.get_headers());
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

        void set_file_dir(std::string&& path, std::string& dir) {
            /*
            default: current path + "www"/"upload"
            "": current path
            "./temp", "temp" : current path + temp
            "/temp" : linux path; "C:/temp" : windows path
            */
            if (path.empty()) {
                dir = fs::current_path().string();
                return;
            }

            if (path[0] == '/' || (path.length() >= 2 && path[1] == ':')) {
                dir = std::move(path);
            }
            else {
                dir = fs::absolute(path).string();
            }
        }

        void init_dir(const std::string& dir) {
            std::error_code ec;
            bool r = fs::exists(dir, ec);
            if (ec) {
                std::cout << ec.message();
            }

            if (!r) {
                fs::create_directories(dir, ec);
                if (ec) {
                    std::cout << ec.message();
                }
            }
        }

		service_pool_policy io_service_pool_;

		std::size_t max_req_buf_size_ = 3 * 1024 * 1024; //max request buffer size 3M
		long keep_alive_timeout_ = 60; //max request timeout 60s

		http_router http_router_;
		std::string static_dir_ = fs::absolute("www").string(); //default
        std::string upload_dir_ = fs::absolute("www").string(); //default
        std::time_t static_res_cache_max_age_ = 0;

		bool enable_timeout_ = true;
		http_handler http_handler_ = nullptr;
		std::function<bool(request& req, response& res)> download_check_;
		std::vector<std::string> relate_paths_;
		std::function<bool(request& req, response& res)> upload_check_ = nullptr;

		std::function<void(request& req, response& res)> not_found_ = nullptr;
		std::function<void(request&, std::string&)> multipart_begin_ = nullptr;

        size_t max_header_len_;
        check_header_cb check_headers_;

        transfer_type transfer_type_ = transfer_type::CHUNKED;
        ssl_configure ssl_conf_;
        bool need_response_time_ = false;
	};

    template<typename T>
	using http_server_proxy = http_server_<T, io_service_pool>;

    using http_server = http_server_proxy<NonSSL>;
    using http_ssl_server = http_server_proxy<SSL>;
}
