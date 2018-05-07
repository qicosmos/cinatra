#pragma once
#include "use_asio.hpp"
#include <string>
#include <vector>
#include <string_view>
#include "io_service_pool.hpp"
#include "connection.hpp"
#include "http_router.hpp"
#include "router.hpp"
#include "nanolog.hpp"
#include "function_traits.hpp"
#include "url_encode_decode.hpp"
namespace cinatra {

	template<class service_pool_policy = io_service_pool>
	class http_server_ : private noncopyable {
	public:
		template<class... Args>
		explicit http_server_(Args&&... args) : io_service_pool_(std::forward<Args>(args)...)
#ifdef CINATRA_ENABLE_SSL
			, ctx_(boost::asio::ssl::context::sslv23)
#endif
		{
			init_conn_callback();
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
				catch (const std::exception& e) {
					LOG_INFO << e.what();
				}
			}

			return r;
		}

		void stop() {
			io_service_pool_.stop();
		}

		void run() {
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
			static_dir_ = std::move(path);
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

		//set http handlers
		template<http_method... Is, typename Function, typename... AP>
		void set_http_handler(std::string_view name, Function&& f, AP&&... ap) {
			http_router_.register_handler<Is...>(name, std::forward<Function>(f), std::forward<AP>(ap)...);
		}

		template<http_method... Is, typename Function, typename... AP>
		void set_static_res_handler(Function&& f, AP&&... ap) {
			http_router_.register_handler<Is...>(STAIC_RES, std::forward<Function>(f), std::forward<AP>(ap)...);
		}

	private:
		void start_accept(std::shared_ptr<boost::asio::ip::tcp::acceptor> const& acceptor) {
			auto new_conn = std::make_shared<connection<Socket>>(
				io_service_pool_.get_io_service(), max_req_buf_size_, keep_alive_timeout_, http_handler_, static_dir_
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
					LOG_INFO << "server::handle_accept: " << e.message();
				}

				start_accept(acceptor);
			});
		}

        void set_static_res_handler()
        {
            http_router_.register_handler<POST,GET>(STAIC_RES, [](const request& req,response& res){
                auto file_name =req.get_res_path();
                std::string real_file_name= std::string(file_name.data(),file_name.size());
                if(is_form_url_encode(file_name))
                {
                    real_file_name = code_utils::get_string_by_urldecode(file_name);
                }
                auto extension = get_extension(real_file_name.data());
                auto mime = get_mime_type(extension);
                std::string res_content_type = std::string(mime.data(),mime.size())+"; charset=utf8";
                res.add_header("Content-type",std::move(res_content_type));
                res.add_header("Access-Control-Allow-origin","*");
                std::ifstream file("./"+real_file_name,std::ios_base::binary);
				if(!file.is_open()){
					res.set_status_and_content(status_type::not_found,"");
					return;
				}
				std::stringstream file_buffer;
                file_buffer<<file.rdbuf();
#ifdef CINATRA_ENABLE_GZIP
                res.set_status_and_content(status_type::ok, file_buffer.str(), res_content_type::none, content_encoding::gzip);
#else
                res.set_status_and_content(status_type::ok, file_buffer.str());
#endif

            });
        }

		void init_conn_callback() {
            set_static_res_handler();
			http_handler_ = [this](const request& req, response& res) {
				bool success = http_router_.route(req.get_method(), req.get_url(), req, res);
				if (!success) {
					res.set_status_and_content(status_type::bad_request, "the url is not right");
				}
			};
		}

		service_pool_policy io_service_pool_;

		std::size_t max_req_buf_size_ = 3 * 1024 * 1024; //max request buffer size 3M
		long keep_alive_timeout_ = 60; //max request timeout 60s

		http_router http_router_;
		std::string static_dir_ = "./static/"; //default
//		https_config ssl_cfg_;
#ifdef CINATRA_ENABLE_SSL
		boost::asio::ssl::context ctx_;
#endif

		http_handler http_handler_ = nullptr;
	};

	using http_server = http_server_<io_service_pool>;
}