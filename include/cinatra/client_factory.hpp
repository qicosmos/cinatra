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

    inline std::pair<std::string_view, std::string_view> get_host_url(std::string_view path) {
        size_t size = path.size();
        size_t pos = std::string_view::npos;
        for (size_t i = 0; i < size; i++) {
            if (path[i] == '/') {
                if (i == size - 1) {
                    pos = i;
                    break;
                }

                if (i + 1 < size - 1 && path[i + 1] == '/') {
                    i++;
                    continue;
                }
                else {
                    pos = i;
                    break;
                }
            }
        }

        if (pos == std::string_view::npos) {
            return { path, "/" };
        }

        std::string_view host = path.substr(0, pos);
        std::string_view url = path.substr(pos);
        if (url.length() > 1 && url.back() == '/') {
            url = url.substr(0, url.length() - 1);
        }

        return { host, url };
    }

    template<typename SocketType = NonSSL, res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT = 3000, http_method METHOD = POST>
    inline std::string send_msg(std::string url, std::string msg) {
        assert(!url.empty());
        constexpr bool is_ssl = std::is_same_v<SocketType, SSL>;
        auto [host, api] = get_host_url(url);
        auto client = client_factory::instance().new_client<SocketType>(std::string(host), is_ssl ? "https" : "http");
        return client->send_msg<CONTENT_TYPE, TIMEOUT, METHOD>(std::string(api), std::move(msg));
    }

    template<typename SocketType = NonSSL, res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT = 3000>
    inline std::string get(std::string url) {
        return send_msg<SocketType, CONTENT_TYPE, TIMEOUT, GET>(url, "");
    }

    template<typename SocketType = NonSSL, res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT = 3000>
    inline std::string post(std::string url, std::string post_content) {
        return send_msg<SocketType, CONTENT_TYPE, TIMEOUT>(url, std::move(post_content));
    }

#ifdef CINATRA_ENABLE_SSL
	//template<res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT = 3000, http_method METHOD = POST>
	//inline std::string send_msg(std::string ip, std::string api, std::string msg, boost::asio::ssl::context & context) {
	//	assert(!api.empty() && api[0] == '/');
	//	auto client = client_factory::instance().new_client<SSL>(std::move(ip), "https", context);
	//	return client->send_msg<CONTENT_TYPE>(std::move(api), std::move(msg));
	//}

	//template<res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT = 3000, http_method METHOD = POST>
	//inline std::string send_msg(std::string ip, std::string port, std::string api, std::string msg, boost::asio::ssl::context & context) {
	//	assert(!api.empty() && api[0] == '/');
	//	auto client = client_factory::instance().new_client<SSL>(std::move(ip), std::move(port), context);
	//	return client->send_msg<CONTENT_TYPE>(std::move(api), std::move(msg));
	//}
#else
	template<res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT = 3000, http_method METHOD = POST>
	inline std::string send_msg(std::string ip, std::string api, std::string msg) {
		assert(!api.empty() && api[0] == '/');
		auto client = client_factory::instance().new_client(std::move(ip), "http");
		return client->send_msg<CONTENT_TYPE>(std::move(api), std::move(msg));
	}

	template<res_content_type CONTENT_TYPE = res_content_type::json, size_t TIMEOUT = 3000, http_method METHOD = POST>
	inline std::string send_msg(std::string ip, std::string port, std::string api, std::string msg) {
		assert(!api.empty() && api[0] == '/');
		auto client = client_factory::instance().new_client(std::move(ip), std::move(port));
		return client->send_msg<CONTENT_TYPE>(std::move(api), std::move(msg));
	}
#endif
}