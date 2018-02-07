#pragma once
namespace cinatra {
	enum class http_type {
		string,
		multipart,
		urlencoded,
		chunked,
		octet_stream,
		websocket,
		unknown,
	};
}
