//
// Created by xmh on 18-7-5.
//

#ifndef HTTP_CLIENT_DEMO_MULTIPART_FORM_HPP
#define HTTP_CLIENT_DEMO_MULTIPART_FORM_HPP
#include <map>
#include <random>
#include "md5.hpp"
#include <fstream>
#include <sstream>
#include "mime_types.hpp"
#include <string>
class multipart_file
{
    friend  class multipart_form;
public:
    multipart_file(const std::string& file_name):_file_path(file_name)
    {
        auto rpos = _file_path.rfind("/");
        if(rpos!=std::string::npos)
        {
            _file_name = _file_path.substr(rpos+1);
        }
        auto extension_name_pos = _file_name.rfind(".");
        if(extension_name_pos!=std::string::npos){
            extension_name = _file_name.substr(extension_name_pos+1);
        }
    }
public:
    std::string read_file() const
    {
        std::ifstream file;
        std::stringstream file_buff;
        file.open(_file_path.c_str(),std::ios::binary);
        if(file.is_open()){
            file_buff << file.rdbuf();
        }else{
            throw "file is not open";
        }
        return file_buff.str();
    }

private:
    std::string _file_path;
    std::string _file_name;
    std::string extension_name;
};
class multipart_form
{
public:
    multipart_form()
    {
        separator_str+=MD5(std::to_string(std::random_device{}())).toStr();
        content_type_str.append(separator_str);
        body_separator_str = std::string("--")+separator_str;
    }
public:
    void append(const std::string& name,const std::string& data)
    {
        form_data.insert(std::make_pair(name,data));
    }
    void append(const std::string& name,const multipart_file& multi_file)
    {
        form_data.insert(std::make_pair(name,multi_file.read_file()));
        file_form.insert(std::make_pair(name,multi_file));
    }
    std::string to_body() const
    {
        std::string content = "";
        for(auto iter = form_data.begin();iter!=form_data.end();++iter)
        {
            content.append(body_separator_str).append("\r\n");
            auto file_iter = file_form.find(iter->first);
           if(file_iter==file_form.end()){
              content.append("Content-Disposition: form-data; ").append("name=").append("\"").append(iter->first).append("\"").append("\r\n");
              content.append("\r\n");
               content.append(iter->second).append("\r\n");
           }else{
              auto file_name =  file_iter->second._file_name;
             auto extension_name = file_iter->second.extension_name;
               content.append("Content-Disposition: form-data; ").append("name=").append("\"").append(iter->first).append("\"").append(" filename=").append("\"").append(file_name).append("\"").append("\r\n");
               auto mime_type = cinatra::get_mime_type(std::basic_string_view(extension_name.data(),extension_name.size()));
               content.append("Content-Type: ").append(mime_type).append("\r\n");
               content.append("\r\n");
               content.append(std::move(iter->second)).append("\r\n");
           }
        }
        content.append(body_separator_str).append("--");
        return content;
    }
    std::string content_type() const
    {
        return content_type_str;
    }
private:
    std::map<std::string,std::string> form_data;
    std::map<std::string,multipart_file> file_form;
    std::string separator_str = "----WebKitFormBoundary";
    std::string content_type_str = "multipart/form-data; boundary=";
    std::string body_content;
    std::string body_separator_str = "";
};
#endif //HTTP_CLIENT_DEMO_MULTIPART_FORM_HPP
