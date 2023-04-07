#include "utils.h"

bool is_valid_url(const std::string &url)
{
    const std::regex pattern(R"(^(https?:\/\/)?([\da-z\.-]+)\.([a-z\.]{2,6})([\/\w \.-]*)*\/?$)");
    if (std::regex_match(url, pattern))
    {
        return true;
    }
    else
    {
        return false;
    }
}