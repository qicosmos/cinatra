#pragma once
#include "async_client.hpp"

namespace cinatra {
	class client_factory {
	public:
		static client_factory& instance() {
			static client_factory instance;
			return instance;
		}

		template<typename...Args>
		auto new_client(Args&&... args) {
			return std::make_shared<async_client>(ios_, std::forward<Args>(args)...);
		}

		void run() {
			ios_.run();
		}

		void stop() {
			ios_.stop();
		}

	private:
		client_factory() : work_(ios_) {
			thd_ = std::make_shared<std::thread>([this] {ios_.run(); });
		}

		~client_factory() { 
			ios_.stop(); 
			thd_->join();
		}

		client_factory(const client_factory&) = delete;
		client_factory& operator=(const client_factory&) = delete;
		client_factory(client_factory&&) = delete;
		client_factory& operator=(client_factory&&) = delete;

		boost::asio::io_service ios_;
		boost::asio::io_service::work work_;
		std::shared_ptr<std::thread> thd_;
	};

    //inline auto get(std::string uri, size_t timeout_seconds = 5) {
    //    auto client = cinatra::client_factory::instance().new_client();
    //    return client->get(std::move(uri), timeout_seconds);
    //}

    //inline auto post(std::string uri, std::string body, size_t timeout_seconds = 5) {
    //    auto client = cinatra::client_factory::instance().new_client();
    //    return client->post(std::move(uri), std::move(body), timeout_seconds);
    //}

    //inline error_code async_get(std::string uri, callback_t cb) {
    //    auto client = cinatra::client_factory::instance().new_client();
    //    return client->async_get(std::move(uri), std::move(cb));
    //}

    //inline error_code async_post(std::string uri, std::string body, callback_t cb) {
    //    auto client = cinatra::client_factory::instance().new_client();
    //    return client->async_post(std::move(uri), std::move(body), std::move(cb));
    //}
}