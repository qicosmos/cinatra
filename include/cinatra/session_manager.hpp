//
// Created by xmh on 18-5-7.
//

#ifndef CINATRA_SESSION_UTILS_HPP
#define CINATRA_SESSION_UTILS_HPP
#include "session.hpp"
#include "request.hpp"
namespace cinatra {
	class session_manager {
	public:
		session_manager() = delete;
		static std::shared_ptr<session> create_session(const std::string& name, std::size_t expire, 
			const std::string& path = "/", const std::string& domain = ""){
			uuids::uuid_random_generator uid{};
			std::string uuid_str = uid().to_short_str();
			auto s = std::make_shared<session>(name, uuid_str, expire, path, domain);

			{
				std::unique_lock<std::mutex> lock(mtx_);
				map_.emplace(std::move(uuid_str), s);
			}

			return s;
		}

		static std::shared_ptr<session> create_session(std::string_view host, const std::string& name,
			std::time_t expire = -1, const std::string &path = "/") {
			auto pos = host.find(":");
			if (pos != std::string_view::npos){
				host = host.substr(0, pos);
			}

			return create_session(name, expire, path, std::string(host.data(), host.length()));
		}

		static std::weak_ptr<session> get_session(const std::string& id) {
			std::unique_lock<std::mutex> lock(mtx_);
			auto it = map_.find(id);
			return (it != map_.end()) ? it->second : nullptr;
		}

		static void del_session(const std::string& id) {
			std::unique_lock<std::mutex> lock(mtx_);
			auto it = map_.find(id);
			if (it != map_.end())
				map_.erase(it);
		}

		static void check_expire() {
			if (map_.empty())
				return;

			auto now = std::time(nullptr);
			std::unique_lock<std::mutex> lock(mtx_);
			for (auto it = map_.begin(); it != map_.end();) {
				if (now - it->second->time_stamp() >= max_age_) {
					it = map_.erase(it);
				}
				else {
					++it;
				}
			}
		}

		static void set_max_inactive_interval(int seconds) {
			max_age_ = seconds;
		}

	private:
		inline static std::map<std::string, std::shared_ptr<session>> map_;
		inline static std::mutex mtx_;
		inline static int max_age_ = 0;
	};
}
#endif //CINATRA_SESSION_UTILS_HPP
