/*
 *Copyright 2020 lanzhengpeng
 *
 *Licensed under the Apache License, Version 2.0 (the "License");
 *you may not use this file except in compliance with the License.
 *You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *Unless required by applicable law or agreed to in writing, software
 *distributed under the License is distributed on an "AS IS" BASIS,
 *WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *See the License for the specific language governing permissions and
 *limitations under the License.
 */

//现代回调(Modern Callback)
//
//一个使用回调处理结果的异步函数，会涉及以下概念：
//_Input_t...：异步函数的输入参数；
//_Signature_t: 此异步回调的函数签名；应当满足‘void(_Exception_t, _Result_t...)’或者‘void(_Result_t...)’类型；
//_Callable_t：回调函数或标记，如果是回调函数，则需要符合_Signature_t的签名类型。这个回调，必须调用一次，且只能调用一次；
//_Return_t：异步函数的返回值；
//_Result_t...：异步函数完成后的结果值，作为回调函数的入参部分；这个参数可以有零至多个；
//_Exception_t：回调函数的异常， 如果不喜欢异常的则忽略这个部分，但就得异步代码将异常处置妥当；
//
//在回调适配器模型里，_Input_t.../_Result_t/_Exception_t(可选)是异步函数提供的功能所固有的部分；_Callable_t/_Return_t
//部分并不直接使用，而是通过适配器去另外处理。这样给予适配器一次扩展到future模式，调用链模式的机会，以及支持协程的机会。

#pragma once
#ifndef MODERN_CALLBACK_HEADER_FILE
#define MODERN_CALLBACK_HEADER_FILE

#include <tuple>

//准备return_void_t和adapter_t给异步函数使用
namespace modern_callback
{
	//通过一个间接的类来解决返回void的语法问题，以便于优化返回值
	struct return_void_t
	{
		void get() {}
	};

	//回调适配器的模板类
	//_Callable_t 要符合 _Signature_t 签名
	//这个类除了转移token外，不做任何有效的工作
	//有效工作等待特列化的类去做
	template<typename _Callable_t, typename _Signature_t>
	struct adapter_t
	{
		using callback_type = _Callable_t;
		using return_type = return_void_t;

		static std::tuple<callback_type, return_type> traits(_Callable_t&& token)
		{
			return { std::forward<_Callable_t>(token), {} };
		}
	};
}

//或者宏版本写法
#define MODERN_CALLBACK_TRAITS(_Token_value, _Signature_t) \
	using _Adapter_t__ = modern_callback::adapter_t<std::remove_cv_t<std::remove_reference_t<_Callable_t>>, _Signature_t>; \
	auto _Adapter_value__ = _Adapter_t__::traits(std::forward<_Callable_t>(_Token_value))
#define MODERN_CALLBACK_CALL() std::move(std::get<0>(_Adapter_value__))
#define MODERN_CALLBACK_RETURN() return std::move(std::get<1>(_Adapter_value__)).get()

#if 0
//tostring_async 演示了在其他线程里，将_Input_t的输入值，转化为std::string类型的_Result_t。
//然后调用_Signature_t为 ‘void(std::string &&)’ 类型的 _Callable_t。
//忽视异常处理，故没有_Exception_t。
//
template<typename _Input_t, typename _Callable_t>
auto tostring_async(_Input_t&& value, _Callable_t&& token)
{
	//适配器类型
	using _Adapter_t = modern_callback::adapter_t<std::remove_cv_t<std::remove_reference_t<_Callable_t>>, void(std::string)>;
	//通过适配器获得兼容_Signature_t类型的真正的回调，以及返回值_Return_t
	auto adapter = _Adapter_t::traits(std::forward<_Callable_t>(token));

	//callback与token未必是同一个变量，甚至未必是同一个类型
	std::thread([callback = std::move(std::get<0>(adapter)), value = std::forward<_Input_t>(value)]
		{
			using namespace std::literals;
			std::this_thread::sleep_for(0.1s);
			callback(std::to_string(value));
		}).detach();

	//返回适配器的_Return_t变量
	return std::move(std::get<1>(adapter)).get();
}
#endif

#endif //MODERN_CALLBACK_HEADER_FILE
