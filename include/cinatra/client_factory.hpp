#pragma once
#include "simple_client.hpp"

namespace cinatra {
	class client_factory {
	public:
		static client_factory& instance() {
			static client_factory instance;
			return instance;
		}

		template<typename...Args>
		auto new_client(Args&&... args) {
			return std::make_shared<simple_client>(ios_, std::forward<Args>(args)...);
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
}