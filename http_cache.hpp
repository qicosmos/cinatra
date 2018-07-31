#pragma once
#include <string>
#include <string_view>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "use_asio.hpp"
namespace cinatra {
	constexpr const size_t MAX_CACHE_SIZE = 100000;

	class http_cache {
	public:
		static void add(const std::string& key, const std::vector<std::string>& content) {
			std::unique_lock<std::mutex> lock(mtx_);
			
			if (std::distance(cur_it_, cache_.end()) > MAX_CACHE_SIZE) {
				cur_it_ = cache_.begin();
			}
			cur_it_ = cache_.emplace(key, content).first;
			cache_time_[key] = std::time(nullptr) + max_cache_age_;
		}

		static std::vector<std::string> get(const std::string& key) {
			std::unique_lock<std::mutex> lock(mtx_);
			auto time_it = cache_time_.find(key);
			auto it = cache_.find(key);
			auto now_time = std::time(nullptr);
			if(time_it != cache_time_.end() && time_it->second >= now_time){
				return it == cache_.end() ? std::vector<std::string>{} : it->second;
			}else{
				if(time_it != cache_time_.end() && it != cache_.end()){
					cur_it_ = cache_.erase(it);
				}
				return std::vector<std::string>{};
			}
		}

		static bool empty() {
			return cache_.empty();
		}

		static void update(const std::string& key) {
			std::unique_lock<std::mutex> lock(mtx_);
			auto it = cache_.find(key);
			if (it != cache_.end())
				cache_.erase(it);
		}

		static void add_skip(std::string_view key) {
			skip_cache_.emplace(key);
		}

		static void enable_cache(bool b) {
			need_cache_ = b;
		}

		static bool need_cache() {
			return need_cache_;
		}

		static bool not_cache(std::string_view key) {
			return skip_cache_.find(key) != skip_cache_.end();
		}

		static void set_cache_max_age(std::time_t seconds)
		{
			max_cache_age_ = seconds;
		}

		static std::time_t get_cache_max_age()
		{
			return max_cache_age_;
		}

	private:
		static std::mutex mtx_;
		static bool need_cache_;
		static std::unordered_map<std::string, std::vector<std::string>> cache_;
		static std::unordered_map<std::string, std::vector<std::string>>::iterator cur_it_;
		static std::unordered_set<std::string_view> skip_cache_;
		static std::time_t max_cache_age_;
		static std::unordered_map<std::string, std::time_t > cache_time_;
	};

	std::unordered_map<std::string, std::vector<std::string>> http_cache::cache_;
	std::unordered_map<std::string, std::vector<std::string>>::iterator http_cache::cur_it_= http_cache::cache_.begin();
	bool http_cache::need_cache_ = false;
	std::mutex http_cache::mtx_;
	std::unordered_set<std::string_view> http_cache::skip_cache_;
	std::time_t http_cache::max_cache_age_ = 0;
	std::unordered_map<std::string, std::time_t > http_cache::cache_time_;
}