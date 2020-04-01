#pragma once
#include "simple_client.hpp"

namespace cinatra {
	class client_factory {
	public:
		static client_factory& instance() {
			static client_factory instance;
			return instance;
		}

		template<typename T, typename...Args>
		auto new_client(Args&&... args) {
			return std::make_shared<simple_client<T>>(ios_, std::forward<Args>(args)...);
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

    template<typename SocketType = NonSSL, res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT = 3000, http_method METHOD = POST>
    inline std::string send_msg(std::string url, std::string msg) {
        assert(!url.empty());
        constexpr bool is_ssl = std::is_same_v<SocketType, SSL>;
        auto [domain, api] = get_domain_url(url);

        auto [host, port] = get_host_port(domain, is_ssl);

        auto client = client_factory::instance().new_client<SocketType>(std::string(host), std::string(port));
        return client->template send_msg<CONTENT_TYPE, TIMEOUT, METHOD>(std::string(api), std::move(msg));
    }

    template<typename SocketType = NonSSL, res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT = 3000>
    inline std::string get(std::string url) {
        return send_msg<SocketType, CONTENT_TYPE, TIMEOUT, GET>(url, "");
    }

    template<typename SocketType = NonSSL, res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT = 3000>
    inline std::string post(std::string url, std::string post_content) {
        return send_msg<SocketType, CONTENT_TYPE, TIMEOUT>(url, std::move(post_content));
    }
}