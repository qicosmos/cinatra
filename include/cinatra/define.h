#pragma once
namespace cinatra {
	enum class content_type {
		string,
		multipart,
		urlencoded,
		chunked,
		octet_stream,
		websocket,
		unknown,
	};

	enum class res_content_type{
		html,
		json,
		string,
		multipart,
        none
	};

	constexpr inline auto HTML = res_content_type::html;
	constexpr inline auto JSON = res_content_type::json;
	constexpr inline auto TEXT = res_content_type::string;
	constexpr inline auto NONE = res_content_type::none;

	inline const std::string_view STATIC_RESOURCE = "cinatra_static_resource";
	inline const std::string CSESSIONID = "CSESSIONID";

    struct NonSSL {};
    struct SSL {};
}
