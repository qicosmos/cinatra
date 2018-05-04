//
// Created by xmh on 18-4-3.
//

#ifndef WEBSTR_STRING_HPP
#define WEBSTR_STRING_HPP
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include "url_encode_decode.hpp"
class  String:public std::string
{
    friend std::ostream& operator<<(std::ostream& out,const String& str);
public:
    enum  class reg_mode:std::size_t
    {
        signle,
        global
    };
    enum class write_file_mode:std::size_t
    {
        full,
        add
    };
public:
    String():std::string()
    {

    }
    String(const char* str):std::string(str)
    {

    }
    String(const std::string& str):std::string(str)
    {

    }
    String(std::string_view str_view):std::string(str_view.data(),str_view.size())
    {

    }
    ~String()
    {

    }
public:
    String(const char* str,std::string::size_type n):std::string(str,n)
    {

    }
    bool operator ==(const String& str) const
    {
        return this->to_stdstring()==str.to_stdstring();
    }
    String operator +(const String& str) const
    {
        return this->to_stdstring()+str.to_stdstring();
    }
//    String& operator=(const String& str)
//    {
//        this->std::string::operator=(str.c_str());
//        return *this;
//    }
public:
    std::string to_stdstring() const
    {
        return *this;
    }
    std::vector<String> split(String splitstr) const
    {
        std::vector<String> Vect;
        const char* head =this->data();
        const char* ptr = head;
        char special = splitstr[0];
        const int compareSize = splitstr.size();
        while(*ptr!='\0')
        {
            if(*ptr==special)
            {
                String possibleSpecialStr(ptr,compareSize);
                if(possibleSpecialStr==splitstr)
                {
                    Vect.push_back(std::move(String(head,ptr-head)));
                    head = ptr+compareSize;
                    ptr+=compareSize;
                }
            }
            ptr++;
        }
        Vect.push_back(std::move(String(head,ptr-head)));
        return std::move(Vect);
    }
    String& join(std::vector<String>& Vect,String splitstr)
    {
        for(auto iter=Vect.begin();iter!=Vect.end();iter++)
        {
            if(iter!=Vect.end()-1)
            {
                std::cout<<*iter<<std::endl;
                (*this)+=*iter+splitstr;
                continue;
            }
            (*this)+=*iter;
        }
        return *this;
    }
    String& replace(String key,String rp,reg_mode mode =reg_mode::signle)
    {
        const char* head = this->data();
        const char* data = this->data();
        const std::size_t key_size = key.size();
        while(*data!='\0')
        {
            if(*data==key[0])
            {
                if(String(data,key_size)==key)
                {
                    String before(head,data-head);
                    String after(data+key_size,this->size());
                    //std::cout<<(before+rp+after)<<std::endl;
                    *this = before+rp+after;
                    head = this->data();
                    data = this->data()+before.size()+rp.size();
                    if(mode==reg_mode::signle)
                    {
                        break;
                    }else if(mode==reg_mode::global){
                        continue;
                    }else{
                        break;
                    }
                }
            }
            data++;
        }
        return *this;
    }
    bool contain(const String& str) const
    {
        auto pos = this->find(str.c_str());
        return pos!=std::string::npos?true:false;
    }
    String url_decode()
    {
        auto str_view = std::string_view(this->data(),this->size());
        if(code_utils::is_url_encode(str_view))
        {
           return  code_utils::get_string_by_urldecode(str_view);
        }
        return *this;
    }
public:
    int to_int() const
    {
        return std::atoi(this->c_str());
    }

    double to_double() const
    {
        return std::atof(this->c_str());
    }
    template<typename T>
    String& format_number(T number)
    {
        *this = std::to_string(number);
        return *this;
    }
    String to_lower() const
    {
        String temp;
        for(auto iter:*this)
        {
            temp+=std::tolower(iter);
        }
        return temp;
    }
    String to_upper() const
    {
        String temp;
        for(auto iter:*this)
        {
            temp+=std::toupper(iter);
        }
        return temp;
    }
    String sort()
    {
        String tmp_str = *this;
        std::size_t char_size = this->size();
        for(int i=0;i<char_size-1;i++)
        {
            for(int k=0;k<char_size-1-i;k++)
            {
                if(tmp_str[k]>tmp_str[k+1])
                {
                    char temp = tmp_str[k];
                    tmp_str[k] = tmp_str[k+1];
                    tmp_str[k+1] = temp;
                }
            }
        }
        return tmp_str;
    }
public:
    bool write_file(const String& fileName,write_file_mode mode = write_file_mode::full) const
    {
        auto write_mode = mode==write_file_mode::full?std::ios_base::out:std::ios_base::app;
        std::ofstream fileHandler(fileName.c_str(),std::ios_base::binary | write_mode);
        fileHandler<<this->c_str();
        return true;
    }

    String& read_file(const String& fileName)
    {
        std::ifstream fileHandler(fileName.c_str(),std::ios_base::binary);
        std::stringstream buff;
        buff<<fileHandler.rdbuf();
        *this = buff.str();
        return  *this;
    }
};
std::ostream& operator<<(std::ostream& out,const String& str)
{
    out<<str.c_str();
    return out;
}
#endif //WEBSTR_STRING_HPP