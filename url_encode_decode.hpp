//
// Created by xmh on 18-7-2.
//

#ifndef _URL_ENCODE_DECODE_HPP
#define _URL_ENCODE_DECODE_HPP

#include <string_view>
#include <string>
#include<locale>
#include <codecvt>
#include <string>

namespace code_utils {
	inline static std::string url_encode(const std::string &value) noexcept {
		static auto hex_chars = "0123456789ABCDEF";

		std::string result;
		result.reserve(value.size()); // Minimum size of result

		for (auto &chr : value) {
			if (!((chr >= '0' && chr <= '9') || (chr >= 'A' && chr <= 'Z') || (chr >= 'a' && chr <= 'z') || chr == '-' || chr == '.' || chr == '_' || chr == '~'))
				result += std::string("%") + hex_chars[static_cast<unsigned char>(chr) >> 4] + hex_chars[static_cast<unsigned char>(chr) & 15];
			else
				result += chr;
		}

		return result;
	}

	inline static std::string url_decode(const std::string &value) noexcept {
		std::string result;
		result.reserve(value.size() / 3 + (value.size() % 3)); // Minimum size of result

		for (std::size_t i = 0; i < value.size(); ++i) {
			auto &chr = value[i];
			if (chr == '%' && i + 2 < value.size()) {
				auto hex = value.substr(i + 1, 2);
				auto decoded_chr = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
				result += decoded_chr;
				i += 2;
			}
			else if (chr == '+')
				result += ' ';
			else
				result += chr;
		}

		return result;
	}

	inline static std::string get_string_by_urldecode(const std::string_view& content)
	{
		return url_decode(std::string(content.data(), content.size()));
	}

	inline static  bool is_url_encode(std::string_view str) {
		return str.find("%") != std::string_view::npos || str.find("+") != std::string_view::npos;
	}


	template<class Facet>
	struct deletable_facet : Facet
	{
		template<class ...Args>
		deletable_facet(Args&& ...args)
			: Facet(std::forward<Args>(args)...) {}
		~deletable_facet() {}
	};

	inline static std::wstring gbk_to_utf16(const std::string &gbk)
	{
#ifdef _MSC_VER
		const char* GBK_LOCALE_NAME = ".936";
#else
		const char* GBK_LOCALE_NAME = "zh_CN.GBK";
#endif

		typedef deletable_facet<std::codecvt_byname<wchar_t, char, std::mbstate_t>> gbfacet_t;

		std::wstring_convert<gbfacet_t> conv(new gbfacet_t(GBK_LOCALE_NAME));
		std::wstring utf16 = conv.from_bytes(gbk);
		return std::move(utf16);
	}

	inline static std::string utf16_to_gbk(const std::wstring &utf16)
	{
#ifdef _MSC_VER
		const char* GBK_LOCALE_NAME = ".936";
#else
		const char* GBK_LOCALE_NAME = "zh_CN.GBK";
#endif

		typedef deletable_facet<std::codecvt_byname<wchar_t, char, std::mbstate_t>> gbfacet_t;
		std::wstring_convert<gbfacet_t> conv(new gbfacet_t(GBK_LOCALE_NAME));

		std::string gbk = conv.to_bytes(utf16);
		return std::move(gbk);
	}

	inline static std::wstring utf8_to_utf16(const std::string &utf8)
	{
		std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
		std::wstring utf16 = conv.from_bytes(utf8);
		return std::move(utf16);
	}

	inline std::string utf16_to_utf8(const std::wstring &utf16)
	{
		std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
		std::string utf8 = conv.to_bytes(utf16);
		return std::move(utf8);
	}


	inline static std::string utf8_to_gbk(const std::string &utf8)
	{
		return utf16_to_gbk(utf8_to_utf16(utf8));
	}

	inline static std::string gbk_to_utf8(const std::string &gbk)
	{
		return utf16_to_utf8(gbk_to_utf16(gbk));
	}
}
#endif //_URL_ENCODE_DECODE_HPP