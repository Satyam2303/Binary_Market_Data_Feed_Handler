#include "bmdfh/protocol.hpp"

#include <arpa/inet.h>
#include <bit>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace bmdfh::protocol {
namespace {

template <typename WireStruct>
const WireStruct* as_wire(std::span<const std::uint8_t> bytes) {
  static_assert(std::is_trivially_copyable_v<WireStruct>);
  return reinterpret_cast<const WireStruct*>(bytes.data());
}

template <typename WireStruct>
void copy_wire(std::uint8_t* destination, const WireStruct& wire_struct) {
  static_assert(std::is_trivially_copyable_v<WireStruct>);
  std::memcpy(destination, &wire_struct, sizeof(WireStruct));
}

std::uint64_t host_to_network_u64(std::uint64_t value) {
  if constexpr (std::endian::native == std::endian::little) {
    return __builtin_bswap64(value);
  }
  return value;
}

std::uint64_t network_to_host_u64(std::uint64_t value) {
  if constexpr (std::endian::native == std::endian::little) {
    return __builtin_bswap64(value);
  }
  return value;
}

}  // namespace

PacketHeader make_packet_header(
  std::uint32_t seq_num,
  std::uint16_t msg_count,
  std::uint64_t publish_tsc
) {
  return PacketHeader {
    .seq_num = htonl(seq_num),
    .msg_count = htons(msg_count),
    .publish_tsc = host_to_network_u64(publish_tsc),
  };
}

AddOrderMsg make_add_order_msg(
  std::uint64_t order_id,
  Side side,
  std::uint32_t price,
  std::uint32_t quantity
) {
  return AddOrderMsg {
    .msg_type = static_cast<char>(MessageType::AddOrder),
    .order_id = host_to_network_u64(order_id),
    .side = static_cast<char>(side),
    .price = htonl(price),
    .quantity = htonl(quantity),
  };
}

ModifyOrderMsg make_modify_order_msg(
  std::uint64_t order_id,
  std::uint32_t price,
  std::uint32_t quantity
) {
  return ModifyOrderMsg {
    .msg_type = static_cast<char>(MessageType::ModifyOrder),
    .order_id = host_to_network_u64(order_id),
    .price = htonl(price),
    .quantity = htonl(quantity),
  };
}

DeleteOrderMsg make_delete_order_msg(std::uint64_t order_id) {
  return DeleteOrderMsg {
    .msg_type = static_cast<char>(MessageType::DeleteOrder),
    .order_id = host_to_network_u64(order_id),
  };
}

TradeMsg make_trade_msg(std::uint64_t order_id, std::uint32_t price, std::uint32_t quantity) {
  return TradeMsg {
    .msg_type = static_cast<char>(MessageType::Trade),
    .order_id = host_to_network_u64(order_id),
    .price = htonl(price),
    .quantity = htonl(quantity),
  };
}

std::uint32_t decode_seq_num(const PacketHeader& header) {
  return ntohl(header.seq_num);
}

std::uint16_t decode_msg_count(const PacketHeader& header) {
  return ntohs(header.msg_count);
}

std::uint64_t decode_publish_tsc(const PacketHeader& header) {
  return network_to_host_u64(header.publish_tsc);
}

std::optional<Side> decode_side(const AddOrderMsg& message) {
  return side_from_char(message.side);
}

std::uint64_t decode_order_id(const AddOrderMsg& message) {
  return network_to_host_u64(message.order_id);
}

std::uint64_t decode_order_id(const ModifyOrderMsg& message) {
  return network_to_host_u64(message.order_id);
}

std::uint64_t decode_order_id(const DeleteOrderMsg& message) {
  return network_to_host_u64(message.order_id);
}

std::uint64_t decode_order_id(const TradeMsg& message) {
  return network_to_host_u64(message.order_id);
}

std::uint32_t decode_price(const AddOrderMsg& message) {
  return ntohl(message.price);
}

std::uint32_t decode_price(const ModifyOrderMsg& message) {
  return ntohl(message.price);
}

std::uint32_t decode_price(const TradeMsg& message) {
  return ntohl(message.price);
}

std::uint32_t decode_quantity(const AddOrderMsg& message) {
  return ntohl(message.quantity);
}

std::uint32_t decode_quantity(const ModifyOrderMsg& message) {
  return ntohl(message.quantity);
}

std::uint32_t decode_quantity(const TradeMsg& message) {
  return ntohl(message.quantity);
}

std::size_t packet_size_bytes(std::uint16_t msg_count) {
  return sizeof(PacketHeader) + static_cast<std::size_t>(msg_count) * kFixedMessageSize;
}

std::optional<PacketView> parse_packet(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < sizeof(PacketHeader)) {
    return std::nullopt;
  }

  const auto* header = as_wire<PacketHeader>(bytes.first(sizeof(PacketHeader)));
  if (bytes.size() != packet_size_bytes(decode_msg_count(*header))) {
    return std::nullopt;
  }

  return PacketView {
    .header = header,
    .message_bytes = bytes.subspan(sizeof(PacketHeader)),
  };
}

std::optional<MessageView> message_at(const PacketView& packet, std::size_t index) {
  if (packet.header == nullptr || index >= decode_msg_count(*packet.header)) {
    return std::nullopt;
  }

  const auto offset = index * kFixedMessageSize;
  const auto chunk = packet.message_bytes.subspan(offset, kFixedMessageSize);
  if (chunk.empty()) {
    return std::nullopt;
  }

  const auto message_type = message_type_from_char(static_cast<char>(chunk.front()));
  if (!message_type) {
    return std::nullopt;
  }

  switch (*message_type) {
    case MessageType::AddOrder: {
      const auto* add = as_wire<AddOrderMsg>(chunk);
      if (!side_from_char(add->side)) {
        return std::nullopt;
      }
      return MessageView {add};
    }
    case MessageType::ModifyOrder:
      return MessageView {as_wire<ModifyOrderMsg>(chunk)};
    case MessageType::DeleteOrder:
      return MessageView {as_wire<DeleteOrderMsg>(chunk)};
    case MessageType::Trade:
      return MessageView {as_wire<TradeMsg>(chunk)};
  }

  return std::nullopt;
}

MessageView view_of(const Message& message) {
  return std::visit(
    [](const auto& wire_message) -> MessageView { return MessageView {&wire_message}; },
    message
  );
}

std::vector<std::uint8_t> serialize_packet(
  std::uint32_t seq_num,
  std::span<const Message> messages,
  std::uint64_t publish_tsc
) {
  if (messages.size() > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("too many messages in one packet");
  }

  const auto msg_count = static_cast<std::uint16_t>(messages.size());
  std::vector<std::uint8_t> bytes(packet_size_bytes(msg_count));

  const auto header = make_packet_header(seq_num, msg_count, publish_tsc);
  copy_wire(bytes.data(), header);

  auto* cursor = bytes.data() + sizeof(PacketHeader);
  for (const auto& message : messages) {
    std::visit(
      [&cursor](const auto& wire_message) {
        copy_wire(cursor, wire_message);
        cursor += kFixedMessageSize;
      },
      message
    );
  }

  return bytes;
}

std::optional<MessageType> message_type_from_char(char msg_type) {
  switch (msg_type) {
    case static_cast<char>(MessageType::AddOrder):
      return MessageType::AddOrder;
    case static_cast<char>(MessageType::ModifyOrder):
      return MessageType::ModifyOrder;
    case static_cast<char>(MessageType::DeleteOrder):
      return MessageType::DeleteOrder;
    case static_cast<char>(MessageType::Trade):
      return MessageType::Trade;
  }

  return std::nullopt;
}

std::optional<Side> side_from_char(char side) {
  switch (side) {
    case static_cast<char>(Side::Buy):
      return Side::Buy;
    case static_cast<char>(Side::Sell):
      return Side::Sell;
  }

  return std::nullopt;
}

std::string to_string(MessageType type) {
  switch (type) {
    case MessageType::AddOrder:
      return "ADD";
    case MessageType::ModifyOrder:
      return "MODIFY";
    case MessageType::DeleteOrder:
      return "DELETE";
    case MessageType::Trade:
      return "TRADE";
  }

  return "UNKNOWN";
}

std::string to_string(Side side) {
  switch (side) {
    case Side::Buy:
      return "BUY";
    case Side::Sell:
      return "SELL";
  }

  return "UNKNOWN";
}

std::string describe(std::uint32_t seq_num, std::size_t message_index, const MessageView& message) {
  std::ostringstream stream;
  stream << "pkt_seq=" << seq_num << " msg=" << (message_index + 1) << ' ';

  std::visit(
    [&stream](const auto* wire_message) {
      using MessagePtr = std::decay_t<decltype(wire_message)>;
      using WireMessage = std::remove_cv_t<std::remove_pointer_t<MessagePtr>>;

      if constexpr (std::is_same_v<WireMessage, AddOrderMsg>) {
        const auto side = decode_side(*wire_message);
        stream << "ADD"
               << " order_id=" << decode_order_id(*wire_message)
               << " side=" << (side ? to_string(*side) : "INVALID")
               << " price=" << decode_price(*wire_message)
               << " qty=" << decode_quantity(*wire_message);
      } else if constexpr (std::is_same_v<WireMessage, ModifyOrderMsg>) {
        stream << "MODIFY"
               << " order_id=" << decode_order_id(*wire_message)
               << " price=" << decode_price(*wire_message)
               << " qty=" << decode_quantity(*wire_message);
      } else if constexpr (std::is_same_v<WireMessage, DeleteOrderMsg>) {
        stream << "DELETE"
               << " order_id=" << decode_order_id(*wire_message);
      } else {
        stream << "TRADE"
               << " order_id=" << decode_order_id(*wire_message)
               << " price=" << decode_price(*wire_message)
               << " qty=" << decode_quantity(*wire_message);
      }
    },
    message
  );

  return stream.str();
}

}  // namespace bmdfh::protocol
