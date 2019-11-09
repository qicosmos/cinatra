#pragma once
#include <vector>
#include <map>
#include <unordered_map>
#include <string>
#include <string_view>
#include "request.hpp"
#include "response.hpp"
#include "utils.hpp"
#include "function_traits.hpp"
#include "mime_types.hpp"
#include "memento.hpp"
#include "session.hpp"
namespace cinatra {
	class http_router {
	public:
		template<http_method... Is, typename Function, typename... Ap>
		std::enable_if_t<!std::is_member_function_pointer_v<Function>> register_handler(std::string_view name, Function&& f, const Ap&... ap) {
			if (name == "/*"sv) {
				assert("register error");
				return;
			}

			if (name.back() == '*') {
				memento::pathinfo_mem.push_back(std::string_view(name.data(), name.length()-1));
			}

			if constexpr(sizeof...(Is) > 0) {
				auto arr = get_arr<Is...>(name);

				for (auto& s : arr) {
					register_nonmember_func(name, s, std::forward<Function>(f), ap...);
				}
			}
			else {
				register_nonmember_func(name, std::string(name.data(), name.length()), std::forward<Function>(f), ap...);
			}
		}

		template <http_method... Is, class T, class Type, typename T1, typename... Ap>
		std::enable_if_t<std::is_same_v<T*, T1>> register_handler(std::string_view name, Type (T::* f)(request&, response&), T1 t, const Ap&... ap) {
			register_handler_impl<Is...>(name, f, t, ap...);
		}

		template <http_method... Is, class T, class Type, typename... Ap>
		void register_handler(std::string_view name, Type(T::* f)(request&, response&), const Ap&... ap) {
			register_handler_impl<Is...>(name, f, (T*)nullptr, ap...);
		}

		void remove_handler(std::string name) {
			this->map_invokers_.erase(name);
		}

		//elimate exception, resut type bool: true, success, false, failed
		bool route(std::string_view method, std::string_view url, request& req, response& res) {
			std::string key(method.data(), method.length());
			bool is_static_res_flag = false;
			if (url.rfind('.') == std::string_view::npos) {
			        url = url.length()>1 && url.back()=='/' ? url.substr(0,url.length()-1):url;
				auto pos = url.rfind("index"sv);
				if (pos != std::string_view::npos)
					key += url.substr(0, pos == 1 ? 1 : pos - 1);
				else
					key += std::string(url.data(), url.length());
			}
			else {
				key += std::string(STATIC_RESOURCE.data(), STATIC_RESOURCE.length());
				is_static_res_flag = true;
			}

			auto it = map_invokers_.find(key);
			if (it == map_invokers_.end()) {
				return get_wildcard_function(key, req, res);
			}
			if(is_static_res_flag==false)
				session_manager::get().check_expire();
			it->second(req, res);
			return true;
		}

	private:
		bool get_wildcard_function(const std::string& key, request& req, response& res) {
			for (auto& pair : wildcard_invokers_) {
				if (key.find(pair.first) != std::string::npos) {
					pair.second(req, res);
					return true;
				}
			}
			return false;
		}

		template <http_method... Is, class T, class Type, typename T1, typename... Ap>
		void register_handler_impl(std::string_view name, Type T::* f, T1 t, const Ap&... ap) {
			if constexpr(sizeof...(Is) > 0) {
				auto arr = get_arr<Is...>(name);

				for (auto& s : arr) {
					register_member_func(name, s, f, t, ap...);
				}
			}
			else {
				register_member_func(name, std::string(name.data(), name.length()), f, t, ap...);
			}
		}

		template<typename Function, typename... AP>
		void register_nonmember_func(std::string_view raw_name, const std::string& name, Function f, const AP&... ap) {
			if (raw_name.back()=='*') {
				this->wildcard_invokers_[name.substr(0, name.size() - 2)] = std::bind(&http_router::invoke<Function, AP...>, this,
					std::placeholders::_1, std::placeholders::_2, std::move(f), ap...);
			}
			else {
				this->map_invokers_[name] = std::bind(&http_router::invoke<Function, AP...>, this,
					std::placeholders::_1, std::placeholders::_2, std::move(f), ap...);
			}
		}

		template<typename Function, typename... AP>
		void invoke(request& req, response& res, Function f, AP... ap) {
			using result_type = std::result_of_t<Function(request&, response&)>;
			std::tuple<AP...> tp(std::move(ap)...);
			bool r = do_ap_before(req, res, tp);

			if (!r)
				return;

			if constexpr(std::is_void_v<result_type>) {
				//business
				f(req, res);
				//after
				do_void_after(req, res, tp);
			}
			else {
				//business
				result_type result = f(req, res);
				//after
				do_after(std::move(result), req, res, tp);
			}
		}

		template<typename Function, typename Self, typename... AP>
		void register_member_func(std::string_view raw_name, const std::string& name, Function f, Self self, const AP&... ap) {
			if (raw_name.back() == '*') {
				this->wildcard_invokers_[name.substr(0, name.size() - 2)] = std::bind(&http_router::invoke_mem<Function, Self, AP...>, this,
					std::placeholders::_1, std::placeholders::_2, f, self, ap...);
			}
			else {
				this->map_invokers_[name] = std::bind(&http_router::invoke_mem<Function, Self, AP...>, this,
					std::placeholders::_1, std::placeholders::_2, f, self, ap...);
			}
		}

		template<typename Function, typename Self, typename... AP>
		void invoke_mem(request& req, response& res, Function f, Self self, AP... ap) {
			using result_type = typename timax::function_traits<Function>::result_type;
			std::tuple<AP...> tp(std::move(ap)...);
			bool r = do_ap_before(req, res, tp);

			if (!r)
				return;
			using nonpointer_type = std::remove_pointer_t<Self>;
			if constexpr(std::is_void_v<result_type>) {
				//business
				if(self)
					(*self.*f)(req, res);
				else
					(nonpointer_type{}.*f)(req, res);
				//after
				do_void_after(req, res, tp);
			}
			else {
				//business
				result_type result;
				if (self)
					result = (*self.*f)(req, res);
				else
					result = (nonpointer_type{}.*f)(req, res);
				//after
				do_after(std::move(result), req, res, tp);
			}
		}

		template<typename Tuple>
		bool do_ap_before(request& req, response& res, Tuple& tp) {
			bool r = true;
			for_each_l(tp, [&r, &req, &res](auto& item) {
				if (!r)
					return;

				constexpr bool has_befor_mtd = has_before<decltype(item), request&, response&>::value;
				if constexpr (has_befor_mtd)
					r = item.before(req, res);
			}, std::make_index_sequence<std::tuple_size_v<Tuple>>{});

			return r;
		}

		template<typename Tuple>
		void do_void_after(request& req, response& res, Tuple& tp) {
			bool r = true;
			for_each_r(tp, [&r, &req, &res](auto& item) {
				if (!r)
					return;

				constexpr bool has_after_mtd = has_after<decltype(item), request&, response&>::value;
				if constexpr (has_after_mtd)
					r = item.after(req, res);
			}, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
		}

		template<typename T, typename Tuple>
		void do_after(T&& result, request& req, response& res, Tuple& tp) {
			bool r = true;
			for_each_r(tp, [&r, result = std::move(result), &req, &res](auto& item){
				if (!r)
					return;

				if constexpr (has_after<decltype(item), T, request&, response&>::value)
					r = item.after(std::move(result), req, res);
			}, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
		}

		typedef std::function<void(request&, response&)> invoker_function;
		std::map<std::string, invoker_function> map_invokers_;
		std::unordered_map<std::string, invoker_function> wildcard_invokers_; //for url/*
	};
}
