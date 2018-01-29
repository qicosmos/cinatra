#pragma once
#include <boost/asio.hpp>
#include <boost/noncopyable.hpp>

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
	class http_server : private boost::noncopyable {
	public:
		explicit http_server(std::size_t io_service_pool_size) : io_service_pool_(io_service_pool_size){
			init_conn_callback();
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
			} catch (const std::exception& e){
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
			auto new_conn = std::make_shared<connection<boost::asio::ip::tcp::socket>>(
				io_service_pool_.get_io_service(), max_req_buf_size_, keep_alive_timeout_, http_handler_, static_dir_);
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

		io_service_pool io_service_pool_;

		std::size_t max_req_buf_size_ = 3 * 1024 * 1024; //max request buffer size 3M
		long keep_alive_timeout_ = 60; //max request timeout 60s

		http_router http_router_;
		std::string static_dir_ = "/tmp/"; //default

		http_handler http_handler_ = nullptr;
	};
}