#pragma once

#include <string>
#include <vector>
#include <tuple>

#include <async_simple/coro/Lazy.h>

#include <functional>
#include <string>
#include <string_view>
#include "cinatra/coro_http_request.hpp"
#include "coro_http_response.hpp"
#include "ylt/util/type_traits.h"

namespace cinatra {
constexpr char type_asterisk = '*';
constexpr char type_colon = ':';
constexpr char type_slash = '/';

typedef std::tuple<
    bool, std::function<void(coro_http_request &req, coro_http_response &resp)>,
    params_t>
    parse_result;

struct handler_t {
  std::string method;
  std::function<void(coro_http_request &req, coro_http_response &resp)> handler;
};

struct radix_tree_node {
  std::string path; // 节点路径
  std::vector<handler_t> handlers; // 处理函数
  std::string indices;  // 
  std::vector<radix_tree_node*> children; // 儿子节点
  int max_params; // 最大参数个数

  radix_tree_node() = default;
  radix_tree_node(const std::string &path) {
    this->path = path;
  }
  ~radix_tree_node() {
    for (auto &c : this->children)
      delete c;
  }

  std::function<void(coro_http_request &req, coro_http_response &resp)>
  get_handler(const std::string &method) {
    for (auto &h : this->handlers) {
      if (h.method == method) {
        return h.handler;
      }
    }
    return nullptr;
  }

  int add_handler(
      std::function<void(coro_http_request &req, coro_http_response &resp)>
          handler,
      const std::vector<std::string> &methods) {
    for (auto &m : methods) {
      auto old_handler = this->get_handler(m);
      this->handlers.push_back(handler_t{m, handler});
    }
    return 0;
  }

  radix_tree_node* insert_child(char index, radix_tree_node *child) {
    auto i = this->get_index_position(index);
    this->indices.insert(this->indices.begin() + i, index);
    this->children.insert(this->children.begin() + i, child);
    return child;
  }

  radix_tree_node *get_child(char index) {
    auto i = this->get_index_position(index);
    return this->indices[i] != index ? nullptr : this->children[i];
  }

  int get_index_position(char target) {
    int low = 0, high = this->indices.size(), mid;

    while (low < high) {
      mid = low + ((high - low) >> 1);
      if (this->indices[mid] < target)
        low = mid + 1;
      else
        high = mid;
    }
    return low;
  }
};

class radix_tree {
public:
  radix_tree() {
    this->root = new radix_tree_node();
  }

  ~radix_tree() {
    delete this->root;
  }

  int insert(
      const std::string &path,
      std::function<void(coro_http_request &req, coro_http_response &resp)>
          handler,
      const std::vector<std::string> &methods) {
    auto root = this->root;
    int i = 0, n = path.size(), param_count = 0, code = 0;
    while (i < n) {
      if (!root->indices.empty() && (
        root->indices[0] == type_asterisk ||
        path[i] == type_asterisk ||
        (path[i] != type_colon && root->indices[0] == type_colon) ||
        (path[i] == type_colon && root->indices[0] != type_colon) ||
        (path[i] == type_colon && root->indices[0] == type_colon && path.substr(
        i + 1, find_pos(path, type_slash, i) - i - 1) != root->children[0]->path))) {
        code = -1;
        break;
      }

      auto child = root->get_child(path[i]);
      if (!child) {
        auto p = find_pos(path, type_colon, i);

        if (p == n) {
          p = find_pos(path, type_asterisk, i);

          root = root->insert_child(path[i], new radix_tree_node(path.substr(i, p - i)));

          if (p < n) {
            root = root->insert_child(type_asterisk, new radix_tree_node(path.substr(p + 1)));
            ++param_count;
          }

          code = root->add_handler(handler, methods);
          break;
        }

        root = root->insert_child(path[i], new radix_tree_node(path.substr(i , p - i)));

        i = find_pos(path, type_slash, p);

        root = root->insert_child(type_colon, new radix_tree_node(path.substr(p + 1, i - p - 1)));
        ++param_count;

        if (i == n) {
          code = root->add_handler(handler, methods);
          break;
        }
      }
      else {

        root = child;

        if (path[i] == type_colon) {
          ++param_count;
          i += root->path.size() + 1;

          if (i == n) {
            code = root->add_handler(handler, methods);
            break;
          }
        }
        else {
          auto j = 0UL;
          auto m = root->path.size();

          for (; i < n && j < m && path[i] == root->path[j]; ++i, ++j) {}

          if (j < m) {
            auto child = new radix_tree_node(root->path.substr(j));
            child->handlers = root->handlers;
            child->indices = root->indices;
            child->children = root->children;

            root->path = root->path.substr(0, j);
            root->handlers = {};
            root->indices = child->path[0];
            root->children = {child};
          }

          if (i == n) {
            code = root->add_handler(handler, methods);
            break;
          }
        }
      }
    }

    if (param_count > this->root->max_params)
      this->root->max_params = param_count;

    return code;
  }

  parse_result get(const std::string &path, const std::string &method) {
    params_t params = {{}, 0};
    auto root = this->root;

    int i = 0, n = path.size(), p;

    while (i < n) {
      if (root->indices.empty())
        return parse_result();

      if (root->indices[0] == type_colon) {
        root = root->children[0];

        p = find_pos(path, type_slash, i);
        params.parameters.push_back(paramters_t{
          root->path, path.substr(i, p - i)
        });
        params.size++;
        i = p;
      }
      else if (root->indices[0] == type_asterisk) {
        root = root->children[0];
        params.parameters.push_back(paramters_t{root->path, path.substr(i)});
        params.size++;
        break;
      }
      else {
        root = root->get_child(path[i]);
        if (!root || path.substr(i, root->path.size()) != root->path)
          return parse_result();
        i += root->path.size();
      }
    }

    return parse_result{true, root->get_handler(method), params};
  }
private:

  int find_pos(const std::string &str, char target, int start) {
    auto i = str.find(target, start);
    return i == -1 ? str.size() : i;
  }

  radix_tree_node *root;
};
}