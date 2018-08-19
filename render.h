#pragma once

#include <iostream>
#include <exception>
#include <sstream>
#include <string>
#include <utility>
#include <map>
#include <vector>
#include <cassert>
#include <iterator>
#include <functional>
#include <memory>
#include <iomanip>
#include "nlohmann_json.hpp"

namespace render {

	template<class Input, class Dictionary, class Output>
	static void parse(Input&& input, Dictionary&& dic, Output&& out);

class object {
    struct holder {
        virtual ~holder() { }
        virtual bool cond() const = 0;
        virtual void map(const std::function<void (object)>& f) const = 0;
        virtual std::string str() const = 0;
        virtual object get(std::string name) = 0;
    };

    template<class T>
    struct holder_impl : holder {
        T obj;
        holder_impl(T obj) : obj(std::move(obj)) { }

        template<class U, int = sizeof(std::declval<U>() ? true : false)>
        bool cond_(int) const {
            return static_cast<bool>(obj);
        }
        template<class>
        bool cond_(...) const {
            throw "This value does not evaluate.";
        }
        virtual bool cond() const override {
            return cond_<T>(0);
        }

        template<class U, int = sizeof(std::declval<U>().begin(), std::declval<U>().end())>
        void map_(int, const std::function<void (object)>& f) const {
            for (auto&& v: obj) {
                f(v);
            }
        }
        template<class>
        void map_(long, const std::function<void (object)>&, ...) const {
            throw "This value does not have begin() or end().";
        }
        virtual void map(const std::function<void (object)>& f) const override {
            map_<T>(0, f);
        };

		template<class U, int = sizeof(std::declval<std::stringstream&>() << std::declval<U>())>
		std::string str_(int) const {
			return str_help(obj);
		}

		template<typename Object>
		std::string str_help(Object& t) const {
			std::stringstream ss;
			ss << t;
			return ss.str();
		}

		std::string str_help(double t) const {
			std::stringstream ss;
			ss << std::setprecision(64) << t;
			return ss.str();
		}
        template<class U>
        std::string str_(...) const {
            throw "This value does not have operator<<().";
        }
        virtual std::string str() const override {
            return str_<T>(0);
        }

        template<class U, int = sizeof(std::declval<U>()[std::declval<std::string>()])>
        object get_(int, std::string name) {
            return obj[std::move(name)];
        }
        template<class U>
        object get_(long, std::string, ...) {
            throw "This value does not have operator[]().";
        }
        virtual object get(std::string name) override {
            return get_<T>(0, std::move(name));
        }
    };

    std::shared_ptr<holder> holder_;

public:
    object() = default;
    object(const object&) = default;
    object(object&&) = default;
    object& operator=(const object&) = default;
    object& operator=(object&&) = default;

    template<class T>
    object(T v) : holder_(new holder_impl<T>(std::move(v))) { }

    template<class T>
    void operator=(T v) { holder_.reset(new holder_impl<T>(std::move(v))); }

    explicit operator bool() const { return holder_->cond(); }
    void map(const std::function<void (object)>& f) const { holder_->map(f); }
    std::string str() const { return holder_->str(); }
    object operator[](std::string name) { return holder_->get(std::move(name)); }
};

using json = nlohmann::json;

typedef std::map<std::string, object> temple;

class parse_error : public std::exception {
    std::string message_;
    int line_number_;
    std::string line1_;
    std::string line2_;
    std::string what_;
    std::string long_error_;

public:
    parse_error(std::string message, int line_number, std::string line1, std::string line2)
        : message_(std::move(message))
        , line_number_(line_number)
        , line1_(line1)
        , line2_(line2) {
        {
            std::stringstream ss;
            ss << "line " << line_number_ << ": " << message_ << std::endl;
            what_ = ss.str();
        }
        {
            std::stringstream ss;
            ss << "ERROR: " << message_ << "\n";
            ss << "LINE: " << line_number_ << "\n";
            ss << line1_ << line2_ << std::endl;
            ss << std::string(line1_.length(), ' ') << '^' << "  <-- current cursor is here\n";
            long_error_ = ss.str();
        }
    }
    virtual const char* what() const noexcept { return what_.c_str(); }
    int line_number() const noexcept { return line_number_; }
    const std::string& line1() const noexcept { return line1_; }
    const std::string& line2() const noexcept { return line2_; }
    const std::string& long_error() const noexcept { return long_error_; }
};

namespace internal {

typedef std::map<std::string, std::vector<object>> tmpl_context;

template<class Iterator>
class parser {
public:
    Iterator current_;
    Iterator next_;
    Iterator last_;
    std::string line_;
    int line_number_;

public:
    typedef std::pair<Iterator, Iterator> range_t;

    parser(Iterator first, Iterator last) : next_(first), last_(last), line_number_(1) {
        assert(first != last);
        current_ = next_++;
    }

    void read() {
        if (current_ == last_)
            throw "End of string suddenly at read()";
        if (*current_ == '\n') {
            line_.clear();
            ++line_number_;
        } else {
            line_.push_back(*current_);
        }
        current_ = next_;
        if (next_ != last_)
            ++next_;
    }
    parse_error read_error(std::string message) {
        std::string line;
        while (current_ != last_ && *current_ != '\n')
            line.push_back(*current_++);
        return parse_error(std::move(message), line_number_, line_, line);
    }

    explicit operator bool() const {
        return current_ != last_;
    }
    char peek() const {
        if (current_ == last_)
            throw "Do not access end of string at peek()";
        return *current_;
    }
    bool has_next() const {
        return next_ != last_;
    }
    char next() const {
        if (next_ == last_)
            throw "Next value is already end of string";
        return *next_;
    }

    typedef std::tuple<Iterator, const std::string*, int> context_t;
    context_t save() const {
        return std::make_tuple(current_, &line_, line_number_);
    }
    void load(context_t context) {
        current_ = next_ = std::get<0>(context);
        if (next_ != last_)
            ++next_;
        line_ = *std::get<1>(context);
        line_number_ = std::get<2>(context);
    }

    template<class F>
    range_t read_while(F f) {
        if (current_ == last_)
            throw "End of string suddenly at read_while()";
        auto first = current_;
        while (f(peek()))
            read();
        return std::make_pair(first, current_);
    }
    template<class F>
    range_t read_while_or_eof(F f) {
        auto first = current_;
        while (current_ != last_ && f(*current_))
            read();
        return std::make_pair(first, current_);
    }

    void skip_whitespace() {
        read_while([](char c) { return c <= 32; });
    }
    void skip_whitespace_or_eof() {
        read_while_or_eof([](char c) { return c <= 32; });
    }
    range_t read_ident() {
        skip_whitespace();
        return read_while([](char c) { return c > 32 && c != '{' && c != '}'; });
    }
    std::string read_ident_str() {
        auto r = read_ident();
        return std::string(r.first, r.second);
    }
    range_t read_variable() {
        auto r = read_while([](char c) { return c > 32 && c != '.' && c != '{' && c != '}'; });
        if (r.first == r.second)
            throw "Did not find variable at read_variable().";
        return r;
    }
    std::string read_variable_str() {
        auto r = read_variable();
        return std::string(r.first, r.second);
    }
	range_t read_include_variable() {
		skip_whitespace();
		auto r = read_while([](char c) { return  c>32&&c != '}'; });
		if (r.first == r.second)
			throw "Did not find variable at read_variable().";
		return r;
	}
	std::string read_include_str() {
		auto r = read_include_variable();
		return std::string(r.first, r.second);
	}
	std::string read_inline_str() {		
		return read_include_str();
	}

    void eat(char c) {
        if (peek() != c)
            throw std::string("Unexpected character ") + peek() + ". Expected character is " + c;
        read();
    }
    void eat(const char* p) {
        while (*p)
            eat(*p++);
    }
    void eat_with_whitespace(const char* p) {
        skip_whitespace();
        eat(p);
    }
    static bool equal(std::pair<Iterator, Iterator> p, const char* str) {
        while (p.first != p.second && *str)
            if (*p.first++ != *str++)
                return false;
        return p.first == p.second && not *str;
    }
};

template<class F, std::size_t N>
static void output_string(F& out, const char (&s)[N]) {
    out.put(s, s + N - 1);
}

template<class Iterator, class Dictionary>
static object get_variable(parser<Iterator>& p, const Dictionary& dic, tmpl_context& ctx, bool skip) {
    p.skip_whitespace();
    if (skip) {
        p.read_variable();
        while (p.peek() == '.') {
            p.read();
            p.read_variable();
        }
        return object();
    } else {
        std::string var = p.read_variable_str();
        auto it = ctx.find(var);
        object obj;
        if (it != ctx.end() && not it->second.empty()) {
            obj = it->second.back();
        } else {
            auto it2 = dic.find(var);
            if (it2 != dic.end()) {
                obj = it2->second;
            } else {
                throw std::string("Variable \"") + var + "\" is not found";
            }
        }
        while (p.peek() == '.') {
            p.read();
            std::string var = p.read_variable_str();
            obj = obj[var];
        }
        return obj;
    }
}

template<class IOS>
struct output_type {
    IOS ios;
    output_type(IOS ios) : ios(std::forward<IOS>(ios)) { }

    template<class Iterator>
    void put(Iterator first, Iterator last) {
        std::copy(first, last, std::ostreambuf_iterator<char>(ios));
    }
    void flush() {
        ios << std::flush;
    }

	void operator<<(std::stringstream& s) {
		ios << s.rdbuf();
	}

    output_type(output_type&&) = default;
    output_type& operator=(output_type&&) = default;

    output_type(const output_type&) = delete;
    output_type& operator=(const output_type&) = delete;
};

template<class IOS>
static output_type<IOS> from_ios(IOS&& ios) {
	return output_type<IOS>(std::forward<IOS>(ios));
}

template<class Iterator, class Dictionary, class F>
static void block(parser<Iterator>& p, const Dictionary& dic, tmpl_context& ctx, bool skip, F& out) {
	while (p) {
		auto r = p.read_while_or_eof([](char c) { return c != '}' && c != '$'; });
		if (not skip)
			out.put(r.first, r.second);
		if (!p)
			break;

		char c = p.peek();
		if (c == '}') {
			if (p.has_next() && p.next() == '}') {
				// end of block
				break;
			}
			else {
				p.read();
				if (not skip)
					output_string(out, "}");
			}
		}
		else if (c == '$') {
			p.read();
			c = p.peek();
			if (c == '$') {
				// $$
				p.read();
				if (not skip)
					output_string(out, "$");
			}
			else if (c == '#') {
				// $# comments
				p.read_while_or_eof([](char peek) {
					return peek != '\n';
				});
			}
			else if (c == '{') {
				p.read();
				c = p.peek();
				if (c == '{') {
					// ${{
					p.read();
					if (not skip)
						output_string(out, "{{");
				}
				else {
					// ${variable}
					object obj = get_variable(p, dic, ctx, skip);
					p.eat_with_whitespace("}");

					if (not skip) {
						std::string str = obj.str();
						out.put(str.begin(), str.end());
					}
				}
			}
			else if (c == '}') {
				p.read();
				c = p.peek();
				if (c == '}') {
					// $}}
					p.read();
					if (not skip)
						output_string(out, "}}");
				}
				else {
					throw std::string("Unexpected character '") + c + "'. It must be '}' after \"$}\"";
				}
			}
			else {
				auto command = p.read_ident();
				if (p.equal(command, "for")) {
					// $for x in xs {{ <block> }}
					auto var1 = p.read_ident_str();
					auto in = p.read_ident();
					if (not p.equal(in, "in"))
						throw "Unexpected string \"" + std::string(in.first, in.second) + "\". It must be \"in\"";
					object obj = get_variable(p, dic, ctx, skip);
					p.eat_with_whitespace("{{");

					if (skip) {
						block(p, dic, ctx, true, out);
					}
					else {
						auto context = p.save();
						auto& vec = ctx[var1];
						obj.map([&](object v) {
							vec.push_back(std::move(v));
							block(p, dic, ctx, skip, out);
							vec.pop_back();
							p.load(context);
						});
						block(p, dic, ctx, true, out);
					}
					p.eat("}}");
				}
				else if (p.equal(command, "if")) {
					// $if x {{ <block> }}
					// $elseif y {{ <block> }}
					// $elseif z {{ <block> }}
					// $else {{ <block> }}
					object obj = get_variable(p, dic, ctx, skip);
					p.eat_with_whitespace("{{");
					bool run; // if `skip` is true, `run` is an unspecified value.
					if (skip) {
						block(p, dic, ctx, true, out);
					}
					else {
						run = static_cast<bool>(obj);
						block(p, dic, ctx, not run, out);
					}
					p.eat("}}");
					while (true) {
						auto context = p.save();
						p.skip_whitespace_or_eof();
						if (!p) {
							p.load(context);
							break;
						}
						c = p.peek();
						if (c == '$') {
							p.read();
							auto command = p.read_ident();
							if (p.equal(command, "elseif")) {
								object obj = get_variable(p, dic, ctx, skip || run);
								p.eat_with_whitespace("{{");
								if (skip || run) {
									block(p, dic, ctx, true, out);
								}
								else {
									bool run_ = static_cast<bool>(obj);
									block(p, dic, ctx, not run_, out);
									if (run_)
										run = true;
								}
								p.eat("}}");
							}
							else if (p.equal(command, "else")) {
								p.eat_with_whitespace("{{");
								block(p, dic, ctx, skip || run, out);
								p.eat("}}");
								break;
							}
							else {
								p.load(context);
								break;
							}
						}
						else {
							p.load(context);
							break;
						}
					}
				}
				else if (p.equal(command, "inline")) {
					// $inline {{ <block> }}
					p.eat_with_whitespace("{{");

					if (skip) {
						block(p, dic, ctx, true, out);
					}
					else {
						std::string include_file = p.read_inline_str();
						std::stringstream buff;
						std::ifstream file(include_file);
						if (!file.is_open()) {
							throw std::runtime_error("html template file can not open");
						}
						buff << file.rdbuf();
						out << buff;
					}
					p.skip_whitespace();
					p.eat("}}");
				}
				else if (p.equal(command, "include")) {
					// $include {{ <block> }}
					p.eat_with_whitespace("{{");

					if (skip) {
						block(p, dic, ctx, true, out);
					}
					else {
						std::string include_file = p.read_include_str();
						std::stringstream buff;
						std::ifstream file(include_file);
						if (!file.is_open()) {
							throw std::runtime_error("html template file can not open");
						}
						buff << file.rdbuf();
						std::stringstream result;
						render::parse(buff.str(), dic, internal::from_ios(result));
						out << result;
					}
					p.skip_whitespace();
					p.eat("}}");
				}
				else {
					throw "Unexpected command " + std::string(command.first, command.second) + ". It must be \"for\" or \"if\"";
				}
			}
		}
		else {
			assert(false && "must not go through.");
			throw "Must not go through.";
		}
	}
}

struct cstring : std::iterator<std::forward_iterator_tag, char> {
    cstring() : p(nullptr) { }
    cstring(const char* p) : p(p) { }
    cstring(const cstring& c) : p(c.p) { }
    cstring begin() { return cstring(p); }
    cstring end() { return cstring(); }
    cstring& operator++() {
        assert(*p != '\0');
        ++p;
        return *this;
    }
    cstring operator++(int) {
        assert(*p != '\0');
        cstring t = *this;
        ++p;
        return t;
    }
    const char& operator*() {
        return *p;
    }
    const char& operator*() const {
        return *p;
    }
    friend bool operator==(const cstring& a, const cstring& b) {
        return
            not a.p && not b.p ? true :
            not a.p &&     b.p ? *b.p == '\0' :
                a.p && not b.p ? *a.p == '\0' :
                                 a.p == b.p;
    }
    friend bool operator!=(const cstring& a, const cstring& b) {
        return !(a == b);
    }
private:
    const char* p;
};

}

template<class Input, class Dictionary, class Output>
static void parse(Input&& input, Dictionary&& dic, Output&& out) {
    auto first = std::begin(input);
    auto last = std::end(input);
    if (first == last) return;

    auto p = internal::parser<decltype(first)>(first, last);
    internal::tmpl_context ctx;
    try {
        internal::block(p, dic, ctx, false, out);
    } catch (std::string message) {
        throw p.read_error(std::move(message));
    } catch (...) {
        throw p.read_error("unexpected error");
    }
    out.flush();
}
template<class Input, class Dictionary>
static void parse(Input&& input, Dictionary&& dic) {
    parse(std::forward<Input>(input), std::forward<Dictionary>(dic), internal::from_ios(std::cout));
}
template<class Dictionary, class Output>
static void parse(const char* input, Dictionary&& dic, Output&& out) {
    parse(internal::cstring(input), std::forward<Dictionary>(dic), std::forward<Output>(out));
}
template<class Dictionary>
static void parse(const char* input, Dictionary&& dic) {
    parse(internal::cstring(input), std::forward<Dictionary>(dic), internal::from_ios(std::cout));
}

static void to_render_data(const nlohmann::json& json, std::map<std::string, object>& render_map);
template<typename Object>
static void to_render_data_impl(const nlohmann::json& json, Object&& render_data, const std::string& key);

template<typename Object>
static void to_render_data_impl(const nlohmann::json& json, Object&& render_data, const std::string& key)
{
	if (json.is_object()) {
		for (auto it = json.begin(); it != json.end(); ++it)
		{
			auto name = it.key();
			to_render_data_impl(it.value(), render_data[key], name);
		}
	}
	else if (json.is_array()) {
		std::vector<object> list;
		for (auto it = json.begin(); it != json.end(); ++it)
		{
			if (!(*it).is_object()) {
				if ((*it).is_string()) {
					list.push_back((*it).get<std::string>());
				}
				else if ((*it).is_number_float()) {
					list.push_back((*it).get<double>());
				}
				else if ((*it).is_number_integer()) {
					list.push_back((*it).get<int>());
				}
				else if ((*it).is_boolean()) {
					list.push_back((*it).get<bool>());
				}
				else if ((*it).is_null()) {
					list.push_back("null");
				}
				else if ((*it).is_number_unsigned()) {
					list.push_back((*it).get<unsigned int>());
				}
			}
			else {
				std::map<std::string, object> object_tmp;
				to_render_data(*it, object_tmp);
				list.push_back(object_tmp);
			}
		}
		render_data[key] = list;
	}
	else if (json.is_string()) {
		render_data[key] = json.get<std::string>();
	}
	else if (json.is_number_float()) {
		render_data[key] = json.get<double>();
	}
	else if (json.is_number_integer()) {
		render_data[key] = json.get<int>();
	}
	else if (json.is_boolean()) {
		render_data[key] = json.get<bool>();
	}
	else if (json.is_null()) {
		render_data[key] = "null";
	}
	else if (json.is_number_unsigned()) {
		render_data[key] = json.get<unsigned int>();
	}
}

static void to_render_data(const nlohmann::json& json, std::map<std::string, object>& render_map)
{
	for (auto iter = json.begin(); iter != json.end(); ++iter) {
		to_render_data_impl(iter.value(), render_map, iter.key());
	}
}

static std::string render_file(const std::string& tpl_filepath, const nlohmann::json& data)
{
	std::stringstream buff;
	std::ifstream file(tpl_filepath);
	if (!file.is_open()) {
		throw std::runtime_error("html template file can not open");
	}
	buff << file.rdbuf();
	std::stringstream result;
	std::map<std::string, object> render_map;
	to_render_data(data, render_map);
	render::parse(buff.str(), render_map, internal::from_ios(result));
	return result.str();
}

static std::string render_string(const std::string& tpl_str, const nlohmann::json& data)
{
	std::stringstream result;
	std::map<std::string, object> render_map;
	to_render_data(data, render_map);
	render::parse(tpl_str, render_map, internal::from_ios(result));
	return result.str();
}

}

