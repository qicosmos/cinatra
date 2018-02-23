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
#include "mime_types.hpp"

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

		bool listen(std::string_view address, std::string_view port) {
			auto acceptor = std::make_shared<boost::asio::ip::tcp::acceptor>(io_service_pool_.get_io_service());
			boost::asio::ip::tcp::resolver resolver(acceptor->get_io_service());
			boost::asio::ip::tcp::resolver::query query(address.data(), port.data());
			boost::asio::ip::tcp::endpoint endpoint = *resolver.resolve(query);
			acceptor->open(endpoint.protocol());
			acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

			bool r = false;
			try {
				acceptor->bind(endpoint);
				acceptor->listen();
				start_accept(acceptor);
				r = true;
			}
			catch (const std::exception& e) {
				LOG_INFO << e.what();
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

		void init_conn_callback() {
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
		std::string static_dir_ = "/tmp/"; //default
//		https_config ssl_cfg_;
#ifdef CINATRA_ENABLE_SSL
		boost::asio::ssl::context ctx_;
#endif

		http_handler http_handler_ = nullptr;
	};

	using http_server = http_server_<io_service_pool>;
}