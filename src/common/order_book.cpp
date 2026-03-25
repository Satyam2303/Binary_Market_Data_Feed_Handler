#include "bmdfh/order_book.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <type_traits>
#include <variant>

namespace bmdfh {

bool OrderBook::ApplyResult::bbo_changed() const {
  return before != after;
}

OrderBook::ApplyResult OrderBook::apply(const protocol::MessageView& message) {
  const auto before = bbo();

  std::visit(
    [this](const auto* wire_message) {
      using MessagePtr = std::decay_t<decltype(wire_message)>;
      using WireMessage = std::remove_cv_t<std::remove_pointer_t<MessagePtr>>;

      if constexpr (std::is_same_v<WireMessage, protocol::AddOrderMsg>) {
        apply_add(*wire_message);
      } else if constexpr (std::is_same_v<WireMessage, protocol::ModifyOrderMsg>) {
        apply_modify(*wire_message);
      } else if constexpr (std::is_same_v<WireMessage, protocol::DeleteOrderMsg>) {
        apply_delete(*wire_message);
      } else {
        apply_trade(*wire_message);
      }
    },
    message
  );

  return ApplyResult {
    .before = before,
    .after = bbo(),
  };
}

OrderBook::BboSnapshot OrderBook::bbo() const {
  BboSnapshot snapshot;

  if (!bids_.empty()) {
    const auto bid = bids_.rbegin();
    snapshot.best_bid = Level {
      .price = bid->first,
      .quantity = bid->second,
    };
  }

  if (!asks_.empty()) {
    const auto ask = asks_.begin();
    snapshot.best_ask = Level {
      .price = ask->first,
      .quantity = ask->second,
    };
  }

  return snapshot;
}

std::string OrderBook::format_bbo(const BboSnapshot& bbo) {
  std::ostringstream stream;

  if (bbo.best_bid) {
    stream << "BID " << bbo.best_bid->price << " x " << bbo.best_bid->quantity;
  } else {
    stream << "BID --";
  }

  stream << " | ";

  if (bbo.best_ask) {
    stream << "ASK " << bbo.best_ask->price << " x " << bbo.best_ask->quantity;
  } else {
    stream << "ASK --";
  }

  return stream.str();
}

void OrderBook::apply_add(const protocol::AddOrderMsg& add) {
  const auto side = protocol::decode_side(add);
  if (!side) {
    return;
  }

  const auto order_id = protocol::decode_order_id(add);
  erase_order(order_id);

  OrderState state {
    .side = *side,
    .quantity = protocol::decode_quantity(add),
    .price = protocol::decode_price(add),
  };

  add_level(state.side, state.price, state.quantity);
  live_orders_[order_id] = state;
}

void OrderBook::apply_modify(const protocol::ModifyOrderMsg& modify) {
  const auto iterator = live_orders_.find(protocol::decode_order_id(modify));
  if (iterator == live_orders_.end()) {
    return;
  }

  subtract_level(iterator->second.side, iterator->second.price, iterator->second.quantity);
  iterator->second.price = protocol::decode_price(modify);
  iterator->second.quantity = protocol::decode_quantity(modify);

  if (iterator->second.quantity == 0) {
    live_orders_.erase(iterator);
    return;
  }

  add_level(iterator->second.side, iterator->second.price, iterator->second.quantity);
}

void OrderBook::apply_delete(const protocol::DeleteOrderMsg& deleted) {
  erase_order(protocol::decode_order_id(deleted));
}

void OrderBook::apply_trade(const protocol::TradeMsg& trade) {
  const auto iterator = live_orders_.find(protocol::decode_order_id(trade));
  if (iterator == live_orders_.end()) {
    return;
  }

  const auto executed_quantity = std::min(iterator->second.quantity, protocol::decode_quantity(trade));
  subtract_level(iterator->second.side, iterator->second.price, executed_quantity);
  iterator->second.quantity -= executed_quantity;

  if (iterator->second.quantity == 0) {
    live_orders_.erase(iterator);
  }
}

void OrderBook::erase_order(std::uint64_t order_id) {
  const auto iterator = live_orders_.find(order_id);
  if (iterator == live_orders_.end()) {
    return;
  }

  subtract_level(iterator->second.side, iterator->second.price, iterator->second.quantity);
  live_orders_.erase(iterator);
}

void OrderBook::add_level(protocol::Side side, std::uint32_t price, std::uint32_t quantity) {
  if (side == protocol::Side::Buy) {
    bids_[price] += quantity;
    return;
  }

  asks_[price] += quantity;
}

void OrderBook::subtract_level(protocol::Side side, std::uint32_t price, std::uint32_t quantity) {
  if (side == protocol::Side::Buy) {
    const auto iterator = bids_.find(price);
    if (iterator == bids_.end()) {
      return;
    }

    if (iterator->second <= quantity) {
      bids_.erase(iterator);
      return;
    }

    iterator->second -= quantity;
    return;
  }

  const auto iterator = asks_.find(price);
  if (iterator == asks_.end()) {
    return;
  }

  if (iterator->second <= quantity) {
    asks_.erase(iterator);
    return;
  }

  iterator->second -= quantity;
}

std::string OrderBook::top_of_book() const {
  return format_bbo(bbo());
}

std::size_t OrderBook::live_order_count() const {
  return live_orders_.size();
}

}  // namespace bmdfh
