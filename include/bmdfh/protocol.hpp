#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bmdfh::protocol {

constexpr std::size_t kFixedMessageSize = 24;

enum class Side : char {
  Buy = 'B',
  Sell = 'S',
};

enum class MessageType : char {
  AddOrder = 'A',
  ModifyOrder = 'M',
  DeleteOrder = 'D',
  Trade = 'T',
};

struct __attribute__((packed)) PacketHeader {
  std::uint32_t seq_num;
  std::uint16_t msg_count;
  std::uint64_t publish_tsc;
};

struct __attribute__((packed)) AddOrderMsg {
  char msg_type {'A'};
  std::uint64_t order_id {};
  char side {'B'};
  std::uint32_t price {};
  std::uint32_t quantity {};
  std::uint8_t reserved[6] {};
};

struct __attribute__((packed)) ModifyOrderMsg {
  char msg_type {'M'};
  std::uint64_t order_id {};
  std::uint32_t price {};
  std::uint32_t quantity {};
  std::uint8_t reserved[7] {};
};

struct __attribute__((packed)) DeleteOrderMsg {
  char msg_type {'D'};
  std::uint64_t order_id {};
  std::uint8_t reserved[15] {};
};

struct __attribute__((packed)) TradeMsg {
  char msg_type {'T'};
  std::uint64_t order_id {};
  std::uint32_t price {};
  std::uint32_t quantity {};
  std::uint8_t reserved[7] {};
};

static_assert(sizeof(PacketHeader) == 14);
static_assert(alignof(PacketHeader) == 1);
static_assert(sizeof(AddOrderMsg) == kFixedMessageSize);
static_assert(sizeof(ModifyOrderMsg) == kFixedMessageSize);
static_assert(sizeof(DeleteOrderMsg) == kFixedMessageSize);
static_assert(sizeof(TradeMsg) == kFixedMessageSize);

using Message = std::variant<AddOrderMsg, ModifyOrderMsg, DeleteOrderMsg, TradeMsg>;
using MessageView =
  std::variant<const AddOrderMsg*, const ModifyOrderMsg*, const DeleteOrderMsg*, const TradeMsg*>;

struct PacketView {
  const PacketHeader* header {};
  std::span<const std::uint8_t> message_bytes {};
};

[[nodiscard]] PacketHeader make_packet_header(
  std::uint32_t seq_num,
  std::uint16_t msg_count,
  std::uint64_t publish_tsc = 0
);
[[nodiscard]] AddOrderMsg make_add_order_msg(
  std::uint64_t order_id,
  Side side,
  std::uint32_t price,
  std::uint32_t quantity
);
[[nodiscard]] ModifyOrderMsg make_modify_order_msg(
  std::uint64_t order_id,
  std::uint32_t price,
  std::uint32_t quantity
);
[[nodiscard]] DeleteOrderMsg make_delete_order_msg(std::uint64_t order_id);
[[nodiscard]] TradeMsg make_trade_msg(
  std::uint64_t order_id,
  std::uint32_t price,
  std::uint32_t quantity
);

[[nodiscard]] std::uint32_t decode_seq_num(const PacketHeader& header);
[[nodiscard]] std::uint16_t decode_msg_count(const PacketHeader& header);
[[nodiscard]] std::uint64_t decode_publish_tsc(const PacketHeader& header);
[[nodiscard]] std::optional<Side> decode_side(const AddOrderMsg& message);
[[nodiscard]] std::uint64_t decode_order_id(const AddOrderMsg& message);
[[nodiscard]] std::uint64_t decode_order_id(const ModifyOrderMsg& message);
[[nodiscard]] std::uint64_t decode_order_id(const DeleteOrderMsg& message);
[[nodiscard]] std::uint64_t decode_order_id(const TradeMsg& message);
[[nodiscard]] std::uint32_t decode_price(const AddOrderMsg& message);
[[nodiscard]] std::uint32_t decode_price(const ModifyOrderMsg& message);
[[nodiscard]] std::uint32_t decode_price(const TradeMsg& message);
[[nodiscard]] std::uint32_t decode_quantity(const AddOrderMsg& message);
[[nodiscard]] std::uint32_t decode_quantity(const ModifyOrderMsg& message);
[[nodiscard]] std::uint32_t decode_quantity(const TradeMsg& message);

[[nodiscard]] std::size_t packet_size_bytes(std::uint16_t msg_count);

[[nodiscard]] std::optional<PacketView> parse_packet(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<MessageView> message_at(const PacketView& packet, std::size_t index);
[[nodiscard]] MessageView view_of(const Message& message);

[[nodiscard]] std::vector<std::uint8_t> serialize_packet(
  std::uint32_t seq_num,
  std::span<const Message> messages,
  std::uint64_t publish_tsc = 0
);

[[nodiscard]] std::optional<MessageType> message_type_from_char(char msg_type);
[[nodiscard]] std::optional<Side> side_from_char(char side);
[[nodiscard]] std::string to_string(MessageType type);
[[nodiscard]] std::string to_string(Side side);
[[nodiscard]] std::string describe(std::uint32_t seq_num, std::size_t message_index, const MessageView& message);

}  // namespace bmdfh::protocol
