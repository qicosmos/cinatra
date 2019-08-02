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

	template<res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT = 3000, http_method METHOD = POST>
	inline std::string send_msg(std::string ip, std::string api, std::string msg) {
		auto client = client_factory::instance().new_client(std::move(ip), "http");
		return client->send_msg<CONTENT_TYPE>(std::move(api), std::move(msg));
	}

	template<res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT = 3000, http_method METHOD = POST>
	inline std::string send_msg(std::string ip, std::string port, std::string api, std::string msg) {
		auto client = client_factory::instance().new_client(std::move(ip), std::move(port));
		return client->send_msg<CONTENT_TYPE>(std::move(api), std::move(msg));
	}
}