#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

#include "bmdfh/protocol.hpp"

namespace bmdfh {

class OrderBook {
 public:
  struct Level {
    std::uint32_t price {};
    std::uint32_t quantity {};

    auto operator<=>(const Level&) const = default;
  };

  struct BboSnapshot {
    std::optional<Level> best_bid {};
    std::optional<Level> best_ask {};

    auto operator<=>(const BboSnapshot&) const = default;
  };

  struct ApplyResult {
    BboSnapshot before {};
    BboSnapshot after {};

    [[nodiscard]] bool bbo_changed() const;
  };

  [[nodiscard]] ApplyResult apply(const protocol::MessageView& message);
  [[nodiscard]] BboSnapshot bbo() const;
  [[nodiscard]] static std::string format_bbo(const BboSnapshot& bbo);
  [[nodiscard]] std::string top_of_book() const;
  [[nodiscard]] std::size_t live_order_count() const;

 private:
  struct OrderState {
    protocol::Side side {protocol::Side::Buy};
    std::uint32_t quantity {};
    std::uint32_t price {};
  };

  void apply_add(const protocol::AddOrderMsg& add);
  void apply_modify(const protocol::ModifyOrderMsg& modify);
  void apply_delete(const protocol::DeleteOrderMsg& deleted);
  void apply_trade(const protocol::TradeMsg& trade);
  void erase_order(std::uint64_t order_id);
  void add_level(protocol::Side side, std::uint32_t price, std::uint32_t quantity);
  void subtract_level(protocol::Side side, std::uint32_t price, std::uint32_t quantity);

  std::unordered_map<std::uint64_t, OrderState> live_orders_;
  std::map<std::uint32_t, std::uint32_t> bids_;
  std::map<std::uint32_t, std::uint32_t> asks_;
};

}  // namespace bmdfh
