#pragma once
#include <type_traits>
#include <functional>
#include <tuple>

//member function
#define TIMAX_FUNCTION_TRAITS(...)\
template <typename ReturnType, typename ClassType, typename... Args>\
struct function_traits_impl<ReturnType(ClassType::*)(Args...) __VA_ARGS__> : function_traits_impl<ReturnType(Args...)>{};\

namespace timax
{
	/*
	* 1. function type                                                 ==>     Ret(Args...)
	* 2. function pointer                                              ==>     Ret(*)(Args...)
	* 3. function reference                                            ==>     Ret(&)(Args...)
	* 4. pointer to non-static member function ==> Ret(T::*)(Args...)
	* 5. function object and functor                           ==> &T::operator()
	* 6. function with generic operator call           ==> template <typeanme ... Args> &T::operator()
	*/
	template <typename T>
	struct function_traits_impl;

	template<typename T>
	struct function_traits : function_traits_impl<
		std::remove_cv_t<std::remove_reference_t<T>>>
	{};

	template<typename Ret, typename... Args>
	struct function_traits_impl<Ret(Args...)>
	{
	public:
		enum { arity = sizeof...(Args) };
		typedef Ret function_type(Args...);
		typedef Ret result_type;
		using stl_function_type = std::function<function_type>;
		typedef Ret(*pointer)(Args...);

		template<size_t I>
		struct args
		{
			static_assert(I < arity, "index is out of range, index must less than sizeof Args");
			using type = typename std::tuple_element<I, std::tuple<Args...>>::type;
		};

		typedef std::tuple<std::remove_cv_t<std::remove_reference_t<Args>>...> tuple_type;
		using args_type_t = std::tuple<Args...>;
	};

	template<size_t I, typename Function>
	using arg_type = typename function_traits<Function>::template args<I>::type;

	// function pointer
	template<typename Ret, typename... Args>
	struct function_traits_impl<Ret(*)(Args...)> : function_traits<Ret(Args...)> {};

	// std::function
	template <typename Ret, typename... Args>
	struct function_traits_impl<std::function<Ret(Args...)>> : function_traits_impl<Ret(Args...)> {};

	// pointer of non-static member function
	TIMAX_FUNCTION_TRAITS()
	TIMAX_FUNCTION_TRAITS(const)
	TIMAX_FUNCTION_TRAITS(volatile)
	TIMAX_FUNCTION_TRAITS(const volatile)

	// functor
	template<typename Callable>
	struct function_traits_impl : function_traits_impl<decltype(&Callable::operator())> {};

	template <typename Function>
	typename function_traits<Function>::stl_function_type to_function(const Function& lambda)
	{
		return static_cast<typename function_traits<Function>::stl_function_type>(lambda);
	}

	template <typename Function>
	typename function_traits<Function>::stl_function_type to_function(Function&& lambda)
	{
		return static_cast<typename function_traits<Function>::stl_function_type>(std::forward<Function>(lambda));
	}

	template <typename Function>
	typename function_traits<Function>::pointer to_function_pointer(const Function& lambda)
	{
		return static_cast<typename function_traits<Function>::pointer>(lambda);
	}

	template<typename Function>
	struct get_class_from_member_function
	{
		using type = void;
	};

	template<typename Ret,typename Class,typename...Args>
	struct get_class_from_member_function<Ret(Class::*)(Args...)>
	{
		using type = Class;
	};

	template<typename Ret,typename Class,typename...Args>
	struct get_class_from_member_function<Ret(Class::*)(Args...) const>
	{
		using type = Class;
	};

	template<typename T,typename...Args>
	struct contains_give_type:std::false_type
	{
        static auto filter(Args&&...tp)
		{
			return std::tuple<Args...>(std::move(tp)...);
		}
	};

	template<typename T,typename...Args>
	struct contains_give_type<T,T,Args...>:std::true_type
	{
         static auto filter(T t,Args...tp)
		 {
			 return std::tuple<Args...>(std::move(tp)...);
		 }

		 static T get_point(T t,Args...tp)
		 {
		 	return t;
		 }
	};

	template<typename T>
	struct is_member_function:std::false_type
	{

	};

	template<typename Ret,typename Class,typename...Args>
	struct is_member_function<Ret(Class::*)(Args...)>:std::true_type
	{

	};
}
