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

// Modern Callback
//
// An asynchronous function that uses callbacks to process results will involve
// the following concepts: _Input_t...: Input parameters of asynchronous
//functions; _Signature_t: The function signature of this asynchronous callback;
//should meet the type of 'void (_Exception_t, _Result_t ...)' or 'void
//(_Result_t ...)’ _Callable_t: Callback function or mark, if it is a callback
//function, it needs to comply with the signature type of _Signature_t. This
//callback must be called once, and only once; _Return_t: The return value of
//the asynchronous function; _Result_t...: The result value after the completion
//of the asynchronous function is used as the input parameter of the callback
//function; this parameter can have zero or more; _Exception_t: The exception of
//the callback function, if you don't like the exception, ignore this part, but
//you have to handle the exception properly with the asynchronous code;
//
// In the callback adapter model, The
// '_Input_t.../_Result_t/_Exception_t(Optional)' is an inherent part of the
// functionality provided by asynchronous functions; The '_Callable_t /
// _Return_t' part is not used directly, but is handled separately by the
// adapter. This gives the adapter an opportunity to expand into future mode,
// call chain mode, and support coroutines.

#pragma once
#ifndef MODERN_CALLBACK_HEADER_FILE
#define MODERN_CALLBACK_HEADER_FILE

#include <tuple>

// Prepare return_void_t and adapter_t for asynchronous functions
namespace modern_callback {
// Solve the syntax problem of returning void through an indirect class in order
// to optimize the return value
struct return_void_t {
  void get() {}
};

// Callback adapter template class
//_Callable_t must conform to _Signature_t signature
// This class does not do any effective work except for transferring tokens
// Real work waits for specialized classes to do
template <typename _Callable_t, typename _Signature_t> struct adapter_t {
  using callback_type = _Callable_t;
  using return_type = return_void_t;
  using result_type = void;

  template <typename _Callable2_t>
  static std::tuple<callback_type, return_type> traits(_Callable2_t &&token) {
    return {std::forward<_Callable2_t>(token), {}};
  }
};
} // namespace modern_callback

#define MODERN_CALLBACK_TRAITS(_Token_value, _Signature_t)                     \
  using _Adapter_t__ = modern_callback::adapter_t<                             \
      std::remove_cv_t<std::remove_reference_t<_Callable_t>>, _Signature_t>;   \
  auto _Adapter_value__ =                                                      \
      _Adapter_t__::traits(std::forward<_Callable_t>(_Token_value))
#define MODERN_CALLBACK_CALL() std::move(std::get<0>(_Adapter_value__))
#define MODERN_CALLBACK_RETURN()                                               \
  return std::move(std::get<1>(_Adapter_value__)).get()
#define MODERN_CALLBACK_RESULT(_Signature_t)                                   \
  typename modern_callback::adapter_t<                                         \
      std::remove_cv_t<std::remove_reference_t<_Callable_t>>,                  \
      _Signature_t>::result_type

#if 0
//tostring_async demonstrates that in other threads, the input value of _Input_t is converted to _Result_t of type std :: string.
//Then call _Signable_t of type ‘void (std :: string &&)’ _Callable_t.
//Ignore exception handling, so there is no _Exception_t.
//
template<typename _Input_t, typename _Callable_t>
auto tostring_async(_Input_t&& value, _Callable_t&& token)
{
	//Adapter type
	using _Adapter_t = modern_callback::adapter_t<std::remove_cv_t<std::remove_reference_t<_Callable_t>>, void(std::string)>;
	//Get real callback compatible with _Signature_t type and return value _Return_t through adapter
	auto adapter = _Adapter_t::traits(std::forward<_Callable_t>(token));

	//callback and token may not be the same variable, or even the same type
	std::thread([callback = std::move(std::get<0>(adapter)), value = std::forward<_Input_t>(value)]
		{
			using namespace std::literals;
			std::this_thread::sleep_for(0.1s);
			callback(std::to_string(value));
		}).detach();

	//Return the adapter's _Return_t variable
	return std::move(std::get<1>(adapter)).get();
}
#endif

#endif // MODERN_CALLBACK_HEADER_FILE
