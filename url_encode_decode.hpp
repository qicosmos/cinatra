//
// Created by xmh on 18-3-16.
//

#ifndef CPPWEBSERVER_URL_ENCODE_DECODE_HPP
#define CPPWEBSERVER_URL_ENCODE_DECODE_HPP
#include <string_view>
#include <string>
#include <cstring>
#include <locale>
#include <codecvt>
//#include <boost/locale.hpp>
/*
!*()_-.
a-zA-Z
    for(int i=1; i<128; ++i)
    {
        if((i >= 'a' && i <= 'z') ||
            (i >= 'A' && i <= 'Z') ||
            i == '!' || i == '*' ||
            i == '(' || i == ')' ||
            i == '_' || i == '-' ||
            i == '.')
            std::cout << "1,";
        else
            std::cout << "0,";
    }
*/
namespace  code_utils
{
    static const unsigned char c_urlflags[128] =
            {
                    0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,
                    0,0,0,0,0,0,0,0,0,0, 0,0,0,1,0,0,0,0,0,0,
                    1,1,1,0,0,1,1,0,1,1, 1,1,1,1,1,1,1,1,0,0,
                    0,0,0,0,0,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,
                    1,1,1,1,1,1,1,1,1,1, 1,0,0,0,0,1,0,1,1,1,
                    1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,
                    1,1,1,0,0,0,0,0,
            };
    static unsigned char c_urlhex[16] = { '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f' };

    template<class char_type>
    static inline char_type * encode_hex(char_type * s, wchar_t u)
    {
        *s++ = '%';
        *s++ = c_urlhex[(u >> 4) & 0x0f];
        *s++ = c_urlhex[u & 0xf];
        return s;
    }

    static inline wchar_t decode_hex(const wchar_t* &s)
    {
        if (iswxdigit(s[0]) && iswxdigit(s[1]))
        {
            wchar_t s1 = towlower(s[0]);
            wchar_t s2 = towlower(s[1]);
            if (iswdigit(s1)) s1 -= L'0';
            else s1 -= 'a' - 10;
            if (iswdigit(s2)) s2 -= L'0';
            else s2 -= 'a' - 10;

            s += 2;
            return (s1 << 4) | s2;
        }

        return 0;
    }

    static inline wchar_t decode_hex(const unsigned char* &s)
    {
        if (isxdigit(s[0]) && isxdigit(s[1]))
        {
            wchar_t s1 = (wchar_t)tolower(s[0]);
            wchar_t s2 = (wchar_t)tolower(s[1]);
            if (isdigit(s1)) s1 -= L'0';
            else s1 -= 'a' - 10;
            if (isdigit(s2)) s2 -= L'0';
            else s2 -= 'a' - 10;

            s += 2;
            return (s1 << 4) | s2;
        }

        return 0;
    }
    static inline wchar_t decode_hex(const char* &s)
    {
        return decode_hex(reinterpret_cast<const unsigned char *&>(s));
    }

    static inline size_t xcslen_(const unsigned char* s)
    {
        return strlen((char *)s);
    }
    static inline size_t xcslen_(const char* s)
    {
        return strlen((char *)s);
    }
    static inline size_t xcslen_(const wchar_t* s)
    {
        return wcslen(s);
    }

    template<class char_type>
    static inline size_t http_url_decode(const char_type * pszUrl, intptr_t nLength, wchar_t * sUrl)
    {
        if (!pszUrl || !*pszUrl)
        {
            if (sUrl) *sUrl = 0;
            return 0;
        }
        else
        {
            if (nLength < 0) nLength = xcslen_(pszUrl);

            if (sUrl)
            {
                const char_type * const pszEnd = pszUrl + nLength;
                wchar_t * pszTarget = sUrl;

                for (const char_type * s = pszUrl; *s && s < pszEnd;)
                {
                    wchar_t u = *s;
                    if (u == L'%')
                    {
                        ++s;
                        wchar_t s0 = 0, s1 = 0, s2 = 0;
                        s0 = decode_hex(s);
                        if (((s0 & 0xC0) == 0xC0) && (*s == L'%'))
                        {
                            ++s;
                            s1 = decode_hex(s);
                            if ((s0 & 0x20) && (s1 & 0x80) && *s == L'%')
                            {
                                ++s;
                                s2 = decode_hex(s);
                                s0 = ((s0 & 0x0F) << 12) | ((s1 & 0x3F) << 6) | (s2 & 0x3F);
                            }
                            else
                            {
                                s0 = ((s0 & 0x1F) << 6) | (s1 & 0x3F);
                            }
                        }

                        if (s0)
                        {
                            *pszTarget++ = s0;
                        }
                        else
                        {
                            *pszTarget = L'%';
                            *pszTarget++ = *s;
                        }
                    }
                    else if (u == L'\\')
                    {
                        ++s;
                        u = *s;
                        if (u && (u == L'U' || u == L'u'))
                        {
                            ++s;
                            wchar_t s0 = decode_hex(s);
                            wchar_t s1 = decode_hex(s);
                            if (s0 && s1)
                            {
                                *pszTarget++ = s0 | (s1 << 8);
                            }
                            else if (s0)
                            {
                                *pszTarget = s0;
                            }
                            else
                            {
                                *pszTarget++ = L'\\';
                                *pszTarget++ = u;
                            }
                        }
                        else
                        {
                            *pszTarget++ = L'\\';
                            *pszTarget++ = *s++;
                        }
                    }
                    else if (u == L'+')
                    {
                        *pszTarget++ = L' ';
                        s++;
                    }
                    else
                    {
                        *pszTarget++ = *s++;
                    }
                }

                return pszTarget - sUrl;
            }
            else
            {
                return nLength;
            }
        }
    }

    template<class char_type>
    static inline size_t http_url_encode(const wchar_t * pszUrl, char_type * sUrl)
    {
        if (!pszUrl || !*pszUrl)
        {
            if (sUrl) *sUrl = 0;
            return 0;
        }
        else
        {
            if (sUrl)
            {
                char_type * pszTarget = sUrl;

                for (const wchar_t * s = pszUrl; *s; ++s)
                {
                    wchar_t u = *s;
                    if (u > 0x07FF)
                    {
                        pszTarget = encode_hex(pszTarget, ((u & 0xF000) >> 12) | 0xE0);
                        pszTarget = encode_hex(pszTarget, ((u & 0x0FC0) >> 6) | 0x80);
                        pszTarget = encode_hex(pszTarget, (u & 0x003F) | 0x80);
                    }
                    else if (u > 0x007F)
                    {
                        pszTarget = encode_hex(pszTarget, ((u & 0x07C0) >> 6) | 0xC0);
                        pszTarget = encode_hex(pszTarget, (u & 0x003F) | 0x80);
                    }
                    else if (c_urlflags[u])
                    {
                        *pszTarget++ = (char_type)u;
                    }
                    else
                    {
                        pszTarget = encode_hex(pszTarget, u);
                    }
                }

                return pszTarget - sUrl;
            }
            else
            {
                size_t nLength = 0;
                for (const wchar_t * s = pszUrl; *s; ++s)
                {
                    wchar_t u = *s;
                    if (u > 0x07FF) nLength += 9;
                    else if (u > 0x007F) nLength += 6;
                    else if (c_urlflags[u]) nLength += 1;
                    else nLength += 3;
                }

                return nLength;
            }
        }
    }

    inline std::wstring url_decode(std::string_view text)
    {
        std::wstring str;

        if (!text.empty())
        {
            size_t length = http_url_decode(text.data(), text.size(), nullptr);
            str.resize(length);
            http_url_decode(text.data(), text.size(), str.data());
        }

        return str;
    }

    template<class string_type>
    inline string_type url_encode(std::wstring_view text)
    {
        string_type str;

        if (!text.empty())
        {
            size_t length = http_url_encode<decltype(str[0])>(text.data(), text.size(), nullptr);
            str.resize(length);
            http_url_encode(text.data(), text.size(), str.data());
        }

        return str;
    }

    inline std::string u8wstring_to_string(const std::wstring& wstr)
    {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.to_bytes(wstr);
    }

    inline std::wstring u8string_to_wstring(const std::string& str)
    {
        std::wstring_convert<std::codecvt_utf8<wchar_t> > conv;
        return conv.from_bytes(str);
    }

    inline std::string get_string_by_urldecode(const std::string_view& content)
    {
        return u8wstring_to_string(url_decode(content));
    }

    inline bool is_url_encode(std::string_view str) {
        return str.find("%") != std::string_view::npos || str.find("+") != std::string_view::npos;
    }

//    inline std::string WStringToString(std::wstring wstr,std::string_view code="utf-8")
//    {
//        std::locale::global(std::locale(""));
//        return boost::locale::conv::from_utf(wstr, std::string(code.data(),code.size()));
//    }
//
//    inline std::string getStringByUrlDecode(std::string_view content,std::string_view code="utf-8")
//    {
//        return WStringToString(url_decode(content),code);
//    }
}
#endif //CPPWEBSERVER_URL_ENCODE_DECODE_HPP
