#pragma once
#include <vector>
#include <map>
#include <string>
#include <string_view>
#include "request.hpp"
#include "response.hpp"
#include "utils.hpp"
#include "function_traits.hpp"

namespace cinatra {
	class http_router {
	public:
		template<http_method... Is, typename Function, typename... Ap>
		void register_handler(std::string_view name, Function&& f, Ap&&... ap) {
			if constexpr(sizeof...(Is) > 0) {
				auto arr = get_arr<Is...>(name);

				for (auto& s : arr) {
					register_nonmember_func(s, std::forward<Function>(f), std::forward<Ap>(ap)...);
				}
			}
			else {
				register_nonmember_func(std::string(name.data(), name.length()), std::forward<Function>(f), std::forward<Ap>(ap)...);
			}
		}

		template <http_method... Is, class T, class Type, typename T1, typename... Ap>
		void register_handler(std::string_view name, Type T::* f, T1 t, Ap&&... ap) {
			register_handler_impl<Is...>(name, f, t, std::forward<Ap>(ap)...);
		}

		void remove_handler(std::string name) {
			this->map_invokers_.erase(name);
		}

		//elimate exception, resut type bool: true, success, false, failed
		bool route(std::string_view method, std::string_view url, const request& req, response& res) {
			std::string key(method.data(), method.length());
			if (url.rfind('.') == std::string_view::npos) {
				key += std::string(url.data(), url.length());
			}
			else {
				key += std::string(STAIC_RES.data(), STAIC_RES.length());
			}

			auto it = map_invokers_.find(key);
			if (it == map_invokers_.end()) {
				return false;
			}

			it->second(req, res);
			return true;
		}

	private:
		template <http_method... Is, class T, class Type, typename T1, typename... Ap>
		void register_handler_impl(std::string_view name, Type T::* f, T1 t, Ap&&... ap) {
			if constexpr(sizeof...(Is) > 0) {
				auto arr = get_arr<Is...>(name);

				for (auto& s : arr) {
					register_member_func(s, f, t, std::forward<Ap>(ap)...);
				}
			}
			else {
				register_member_func(std::string(name.data(), name.length()), f, t, std::forward<Ap>(ap)...);
			}
		}

		template<typename Function, typename... AP>
		void register_nonmember_func(const std::string& name, Function f, AP&&... ap) {
			this->map_invokers_[name] = std::bind(&http_router::invoke<Function, AP...>, this,
				std::placeholders::_1, std::placeholders::_2, std::move(f), std::move(ap)...);
		}

		template<typename Function, typename... AP>
		void invoke(const request& req, response& res, Function f, AP... ap) {
			using result_type = std::result_of_t<Function(const request&, response&)>;
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
		void register_member_func(const std::string& name, Function f, Self self, AP&&... ap) {
			this->map_invokers_[name] = std::bind(&http_router::invoke_mem<Function, Self, AP...>, this,
				std::placeholders::_1, std::placeholders::_2, f, self, std::move(ap)...);
		}

		template<typename Function, typename Self, typename... AP>
		void invoke_mem(const request& req, response& res, Function f, Self self, AP... ap) {
			using result_type = typename timax::function_traits<Function>::result_type;
			std::tuple<AP...> tp(std::move(ap)...);
			bool r = do_ap_before(req, res, tp);

			if (!r)
				return;

			if constexpr(std::is_void_v<result_type>) {
				//business
				(*self.*f)(req, res);
				//after
				do_void_after(req, res, tp);
			}
			else {
				//business
				result_type result = (*self.*f)(req, res);
				//after
				do_after(std::move(result), req, res, tp);
			}
		}

		template<typename Tuple>
		bool do_ap_before(const request& req, response& res, Tuple& tp) {
			bool r = true;
			for_each_l(tp, [&r, &req, &res](auto& item) {
				if (!r)
					return;

				if constexpr (has_before<decltype(item), const request&, response&>::value)
					r = item.before(req, res);
			}, std::make_index_sequence<std::tuple_size_v<Tuple>>{});

			return r;
		}

		template<typename Tuple>
		void do_void_after(const request& req, response& res, Tuple& tp) {
			bool r = true;
			for_each_r(tp, [&r, &req, &res](auto& item) {
				if (!r)
					return;

				if constexpr (has_after<decltype(item), const request&, response&>::value)
					r = item.after(req, res);
			}, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
		}

		template<typename T, typename Tuple>
		void do_after(T&& result, const request& req, response& res, Tuple& tp) {
			bool r = true;
			for_each_r(tp, [&r, result = std::move(result), &req, &res](auto& item){
				if (!r)
					return;

				if constexpr (has_after<decltype(item), T, const request&, response&>::value)
					r = item.after(std::move(result), req, res);
			}, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
		}

		typedef std::function<void(const request&, response&)> invoker_function;
		std::map<std::string, invoker_function> map_invokers_;
	};
}
