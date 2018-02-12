//
// Created by qiyu on 12/19/17.
//

#ifndef CINATRA_UTILS_HPP
#define CINATRA_UTILS_HPP
#include <iostream>
#include <string>
#include <string_view>
#include <cstdlib>
#include <cctype>
#include <type_traits>

namespace cinatra {
	struct ci_less
	{
		// case-independent (ci) compare_less binary function
		struct nocase_compare
		{
			bool operator() (const unsigned char& c1, const unsigned char& c2) const {
				return tolower(c1) < tolower(c2);
			}
		};
		bool operator() (const std::string & s1, const std::string & s2) const {
			return std::lexicographical_compare
			(s1.begin(), s1.end(),   // source range
				s2.begin(), s2.end(),   // dest range
				nocase_compare());  // comparison
		}

		bool operator() (const std::string_view & s1, const std::string_view & s2) const {
			return std::lexicographical_compare
			(s1.begin(), s1.end(),   // source range
				s2.begin(), s2.end(),   // dest range
				nocase_compare());  // comparison
		}
	};

	class noncopyable {
	public:
		noncopyable() = default;
		~noncopyable() = default;

	private:
		noncopyable(const noncopyable&) = delete;
		noncopyable& operator=(const noncopyable&) = delete;
	};

	using namespace std::string_view_literals;

	template <class T>
	struct sv_char_trait : std::char_traits<T> {
		using base_t = std::char_traits<T>;
		using char_type = typename base_t::char_type;

		static constexpr int compare(std::string_view s1, std::string_view s2) noexcept {
			if (s1.length() != s2.length())
				return -1;

			size_t n = s1.length();
			for (size_t i = 0; i < n; ++i) {
				if (!base_t::eq(s1[i], s2[i])) {
					return base_t::eq(s1[i], s2[i]) ? -1 : 1;
				}
			}

			return 0;
		}

		static constexpr size_t find(std::string_view str, const char_type& a) noexcept {
			auto s = str.data();
			for (size_t i = 0; i < str.length(); ++i) {
				if (base_t::eq(s[i], a)) {
					return i;
				}
			}

			return  std::string_view::npos;
		}
	};

	inline std::string_view trim_left(std::string_view v) {
		v.remove_prefix(std::min(v.find_first_not_of(" "), v.size()));
		return v;
	}

	inline std::string_view trim_right(std::string_view v) {
		v.remove_suffix(std::min(v.size() - v.find_last_not_of(" ") - 1, v.size()));
		return v;
	}

	inline std::string_view trim(std::string_view v) {
		v.remove_prefix(std::min(v.find_first_not_of(" "), v.size()));
		v.remove_suffix(std::min(v.size() - v.find_last_not_of(" ") - 1, v.size()));
		return v;
	}

	template<typename T>
	constexpr bool  is_int64_v = std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t>;

	enum class http_method {
		DEL,
		GET,
		HEAD,
		POST,
		PUT,
		CONNECT,
		OPTIONS,
		TRACE
	};
	constexpr inline auto GET = http_method::GET;
	constexpr inline auto POST = http_method::POST;
	constexpr inline auto DEL = http_method::DEL;
	constexpr inline auto HEAD = http_method::HEAD;
	constexpr inline auto PUT = http_method::PUT;
	constexpr inline auto CONNECT = http_method::CONNECT;
	constexpr inline auto TRACE = http_method::TRACE;
	constexpr inline auto OPTIONS = http_method::OPTIONS;

	constexpr auto type_to_name(std::integral_constant<http_method, http_method::DEL>) noexcept { return "DELETE"sv; }
	constexpr auto type_to_name(std::integral_constant<http_method, http_method::GET>) noexcept { return "GET"sv; }
	constexpr auto type_to_name(std::integral_constant<http_method, http_method::HEAD>) noexcept { return "HEAD"sv; }

	constexpr auto type_to_name(std::integral_constant<http_method, http_method::POST>) noexcept { return "POST"sv; }
	constexpr auto type_to_name(std::integral_constant<http_method, http_method::PUT>) noexcept { return "PUT"sv; }

	constexpr auto type_to_name(std::integral_constant<http_method, http_method::CONNECT>) noexcept { return "CONNECT"sv; }
	constexpr auto type_to_name(std::integral_constant<http_method, http_method::OPTIONS>) noexcept { return "OPTIONS"sv; }
	constexpr auto type_to_name(std::integral_constant<http_method, http_method::TRACE>) noexcept { return "TRACE"sv; }

	inline bool iequal(const char *s, size_t l, const char *t) {
		if (strlen(t) != l)
			return false;

		for (size_t i = 0; i < l; i++) {
			if (std::tolower(s[i]) != t[i])
				return false;
		}

		return true;
	}

	inline const std::string form_urldecode(std::string_view str) {
		using namespace std;
		string result;
		string::size_type i;
		for (i = 0; i < str.size(); ++i) {
			if (str[i] == '+') {
				result += ' ';
			}
			else if (str[i] == '%' && str.size() > i + 2) {
				int val = std::strtol(&str[i + 1], 0, 16);
				result += std::to_string(val);
				i += 2;
			}
			else {
				result += str[i];
			}
		}
		return result;
	}

	inline bool is_form_url_encode(std::string_view str) {
		return str.find("%") != std::string_view::npos || str.find("+") != std::string_view::npos;
	}

	inline std::string_view get_extension(std::string_view name) {
		size_t pos = name.rfind('.');
		if (pos == std::string_view::npos) {
			return {};
		}

		return name.substr(pos);
	}

	inline std::string to_hex_string(std::size_t value) {
		std::ostringstream stream;
		stream << std::hex << value;
		return stream.str();
	}
	inline const char *MAP = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz"
		"0123456789+/";

	inline const char *MAP_URL_ENCODED = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz"
		"0123456789-_";
	// from h2o
	inline size_t base64_encode(char *_dst, const void *_src, size_t len, int url_encoded) {
		char *dst = _dst;
		const uint8_t *src = reinterpret_cast<const uint8_t *>(_src);
		const char *map = url_encoded ? MAP_URL_ENCODED : MAP;
		uint32_t quad;

		for (; len >= 3; src += 3, len -= 3) {
			quad = ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2];
			*dst++ = map[quad >> 18];
			*dst++ = map[(quad >> 12) & 63];
			*dst++ = map[(quad >> 6) & 63];
			*dst++ = map[quad & 63];
		}
		if (len != 0) {
			quad = (uint32_t)src[0] << 16;
			*dst++ = map[quad >> 18];
			if (len == 2) {
				quad |= (uint32_t)src[1] << 8;
				*dst++ = map[(quad >> 12) & 63];
				*dst++ = map[(quad >> 6) & 63];
				if (!url_encoded)
					*dst++ = '=';
			}
			else {
				*dst++ = map[(quad >> 12) & 63];
				if (!url_encoded) {
					*dst++ = '=';
					*dst++ = '=';
				}
			}
		}

		*dst = '\0';
		return dst - _dst;
	}

	inline bool is_valid_utf8(unsigned char *s, size_t length)
	{
		for (unsigned char *e = s + length; s != e; )
		{
			if (s + 4 <= e && ((*(uint32_t *)s) & 0x80808080) == 0)
			{
				s += 4;
			}
			else
			{
				while (!(*s & 0x80))
				{
					if (++s == e)
					{
						return true;
					}
				}

				if ((s[0] & 0x60) == 0x40)
				{
					if (s + 1 >= e || (s[1] & 0xc0) != 0x80 || (s[0] & 0xfe) == 0xc0)
					{
						return false;
					}
					s += 2;
				}
				else if ((s[0] & 0xf0) == 0xe0)
				{
					if (s + 2 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 ||
						(s[0] == 0xe0 && (s[1] & 0xe0) == 0x80) || (s[0] == 0xed && (s[1] & 0xe0) == 0xa0))
					{
						return false;
					}
					s += 3;
				}
				else if ((s[0] & 0xf8) == 0xf0)
				{
					if (s + 3 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 || (s[3] & 0xc0) != 0x80 ||
						(s[0] == 0xf0 && (s[1] & 0xf0) == 0x80) || (s[0] == 0xf4 && s[1] > 0x8f) || s[0] > 0xf4)
					{
						return false;
					}
					s += 4;
				}
				else
				{
					return false;
				}
			}
		}
		return true;
	}

	template<typename T>
	inline std::string to_str(T&& value) {
		using U = std::remove_const_t<std::remove_reference_t<T>>;
		if constexpr(std::is_integral_v<U> && !is_int64_v<U>) {
			std::vector<char> temp(20, '\0');
			itoa_fwd(value, temp.data());
			return std::string(temp.data());
		}
		else if constexpr (is_int64_v<U>) {
			std::vector<char> temp(65, '\0');
			xtoa(value, temp.data(), 10, std::is_signed_v<U>);
			return std::string(temp.data());
		}
		else if constexpr (std::is_floating_point_v<U>) {
			std::vector<char> temp(20, '\0');
			sprintf(temp.data(), "%f", value);
			return std::string(temp.data());
		}
		else if constexpr(std::is_same_v<std::string, U> || std::is_same_v<const char*, U>) {
			return value;
		}
		else {
			std::cout << "this type has not supported yet" << std::endl;
		}
	}

#define HAS_MEMBER(member)\
template<typename T, typename... Args>\
struct has_##member\
{\
private:\
    template<typename U> static auto Check(int) -> decltype(std::declval<U>().member(std::declval<Args>()...), std::true_type()); \
	template<typename U> static std::false_type Check(...);\
public:\
	enum{value = std::is_same<decltype(Check<T>(0)), std::true_type>::value};\
};

	HAS_MEMBER(before)
	HAS_MEMBER(after)

	template <typename... Args, typename F, std::size_t... Idx>
	constexpr void for_each_l(std::tuple<Args...>& t, F&& f, std::index_sequence<Idx...>) {
		(std::forward<F>(f)(std::get<Idx>(t)), ...);
	}

	template <typename... Args, typename F, std::size_t... Idx>
	constexpr void for_each_r(std::tuple<Args...>& t, F&& f, std::index_sequence<Idx...>) {
		constexpr auto size = sizeof...(Idx);
		(std::forward<F>(f)(std::get<size - Idx - 1>(t)), ...);
	}

	template<http_method N>
	constexpr void get_str(std::string& s, std::string_view name) {
		s = type_to_name(std::integral_constant<http_method, N>{}).data();
		s += std::string(name.data(), name.length());
	}

	template<http_method... Is>
	constexpr auto get_arr(std::string_view name) {
		std::array<std::string, sizeof...(Is)> arr = {};
		size_t index = 0;
		(get_str<Is>(arr[index++], name), ...);

		return arr;
	}
}

#endif //CINATRA_UTILS_HPP
