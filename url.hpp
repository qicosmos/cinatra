//
// Created by liuxo on 12/19/17.
//

#ifndef CINATRA_URL_HPP
#define CINATRA_URL_HPP

#pragma once
#include <string>
#include <sstream>
#include <string_view>
#include <algorithm>
#include <iomanip>
#include <cstddef>
#include <iostream>

namespace cinatra
{

    // var bools = [];
    // var valid_chr = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-';
    // for(var i = 0; i <= 255; ++ i) {
    // 	var contain = valid_chr.indexOf(String.fromCharCode(i)) == -1;
    // 	bools.push(contain?false:true);
    // }
    // console.log(JSON.stringify(bools))

    inline std::ostringstream& quote_impl(std::ostringstream& os, std::string_view str, std::string_view safe) {
        os << setiosflags(std::ios::right) << std::setfill('0');
        const constexpr static bool valid_chr[128] =
        {
            false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,true,true,false,true,true,true,true,true,true,true,true,true,true,false,false,false,false,false,false,false,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,false,false,false,false,true,false,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,false,false,false,false,false
        };
        auto begin = reinterpret_cast<const std::byte*>(str.data());
        auto end = begin + sizeof(char) * str.size();
        std::for_each(begin, end, [&os, &safe] (auto& chr)
        {
            char chrval = (char) chr;
            unsigned int intval = (unsigned int) chr;
            if ((intval > 128 || !valid_chr[intval]) && safe.find(chrval) == std::string_view::npos)
                os << '%' << std::setw(2) << std::hex << std::uppercase << intval;
            else 
                os << chrval;
        });
        return os;
    }

    inline const std::string quote(std::string_view str)
    {
        std::ostringstream os;
        return quote_impl(os, str, "/").str();
    }

    inline const std::string quote_plus(std::string_view str)
    {
        if (str.find(' ') == std::string_view::npos) return quote(str);

        std::ostringstream os;
        auto strval = quote_impl(os, str, " ").str();
        std::replace(strval.begin(), strval.end(), ' ', '+');
        return strval;
    }

}

#endif //CINATRA_URL_HPP