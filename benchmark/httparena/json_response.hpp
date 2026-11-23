#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iguana/json_writer.hpp"

namespace httparena {

struct Rating {
  int64_t score;
  int64_t count;
};
YLT_REFL(Rating, score, count);

struct Item {
  int64_t id;
  std::string name;
  std::string category;
  int64_t price;
  int64_t quantity;
  bool active;
  std::vector<std::string> tags;
  Rating rating;
};
YLT_REFL(Item, id, name, category, price, quantity, active, tags, rating);

struct ResponseItem {
  int64_t id;
  std::string_view name;
  std::string_view category;
  int64_t price;
  int64_t quantity;
  bool active;
  std::span<const std::string> tags;
  Rating rating;
  int64_t total;
};
YLT_REFL(ResponseItem, id, name, category, price, quantity, active, tags,
         rating, total);

// The dataset owns the fields until the synchronous write completes.
struct JsonRows {
  JsonRows(std::span<const Item> items, int64_t multiplier)
      : items(items), multiplier(multiplier) {}

  std::span<const Item> items;
  int64_t multiplier;
};

template <bool Escape, typename Stream>
void to_json_impl(Stream &output, const JsonRows &rows) {
  output.push_back('[');
  for (size_t i = 0; i < rows.items.size(); ++i) {
    if (i != 0) {
      output.push_back(',');
    }
    const auto &item = rows.items[i];
    iguana::to_json<Escape>(
        ResponseItem{item.id, item.name, item.category, item.price,
                     item.quantity, item.active, item.tags, item.rating,
                     item.price * item.quantity * rows.multiplier},
        output);
  }
  output.push_back(']');
}

struct JsonResponse {
  JsonRows items;
  size_t count;
};
YLT_REFL(JsonResponse, items, count);

}  // namespace httparena

template <>
inline constexpr bool iguana::reflect26_excluded_v<httparena::JsonRows> = true;
