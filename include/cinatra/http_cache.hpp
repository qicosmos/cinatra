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
		static http_cache& get(){
			static http_cache instance;
			return instance;
		}

		void add(const std::string& key, const std::vector<std::string>& content) {
			std::unique_lock<std::mutex> lock(mtx_);
			
			if (std::distance(cur_it_, cache_.end()) > MAX_CACHE_SIZE) {
				cur_it_ = cache_.begin();
			}

			cur_it_ = cache_.emplace(key, content).first;
			cache_time_[key] = std::time(nullptr) + max_cache_age_;
		}

		std::vector<std::string> get(const std::string& key) {
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

		bool empty() {
			return cache_.empty();
		}

		void update(const std::string& key) {
			std::unique_lock<std::mutex> lock(mtx_);
			auto it = cache_.find(key);
			if (it != cache_.end())
				cache_.erase(it);
		}

		void add_skip(std::string_view key) {
			skip_cache_.emplace(key);
		}

		void add_single_cache(std::string_view key)
		{
			need_single_cache_.emplace(key);
		}

		void enable_cache(bool b) {
			need_cache_ = b;
		}

		bool need_cache(std::string_view key) {
			if(need_cache_){
				return need_cache_;
			}else{
                return need_single_cache_.find(key)!= need_single_cache_.end();
			}
		}

		bool not_cache(std::string_view key) {
			return skip_cache_.find(key) != skip_cache_.end();
		}

		void set_cache_max_age(std::time_t seconds)
		{
			max_cache_age_ = seconds;
		}

		std::time_t get_cache_max_age()
		{
			return max_cache_age_;
		}

	private:
		http_cache() {};
		http_cache(const http_cache&) = delete;
		http_cache(http_cache&&) = delete;

		std::mutex mtx_;
		bool need_cache_ = false;
		std::unordered_map<std::string, std::vector<std::string>> cache_;
		std::unordered_map<std::string, std::vector<std::string>>::iterator cur_it_ = http_cache::cache_.begin();
		std::unordered_set<std::string_view> skip_cache_;
		std::unordered_set<std::string_view> need_single_cache_;
		std::time_t max_cache_age_ = 0;
		std::unordered_map<std::string, std::time_t > cache_time_;
	};
}