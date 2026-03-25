#include "bmdfh/benchmark.hpp"
#include "bmdfh/protocol.hpp"
#include "bmdfh/recovery.hpp"
#include "bmdfh/udp_multicast.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

constexpr std::size_t kPacketBufferSize = 1024;
constexpr std::size_t kMaxMessagesPerPacket =
  (kPacketBufferSize - sizeof(bmdfh::protocol::PacketHeader)) / bmdfh::protocol::kFixedMessageSize;

struct Options {
  std::string group {"239.255.42.99"};
  std::uint16_t port {30001};
  std::uint16_t recovery_port {31001};
  std::size_t messages {20};
  std::size_t messages_per_packet {4};
  std::size_t skip_every {0};
  std::uint64_t interval_ms {100};
  std::uint64_t recovery_grace_ms {1000};
  bool quiet {false};
};

template <typename Integer>
Integer parse_unsigned(std::string_view text, std::string_view flag_name) {
  Integer value {};
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [pointer, error_code] = std::from_chars(begin, end, value);

  if (error_code != std::errc {} || pointer != end) {
    throw std::runtime_error("invalid numeric value for " + std::string(flag_name));
  }

  return value;
}

void print_usage() {
  std::cout
    << "Usage: exchange_sim [--group ADDRESS] [--port PORT] [--messages COUNT] "
       "[--recovery-port PORT] [--messages-per-packet COUNT] [--skip-every N] "
       "[--interval-ms DELAY] [--recovery-grace-ms DELAY] [--quiet]\n";
}

Options parse_args(int argc, char** argv) {
  Options options;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    auto require_value = [&](std::string_view flag) -> std::string_view {
      if (index + 1 >= argc) {
        throw std::runtime_error("missing value for " + std::string(flag));
      }
      ++index;
      return argv[index];
    };

    if (argument == "--group") {
      options.group = std::string(require_value(argument));
    } else if (argument == "--port") {
      options.port = parse_unsigned<std::uint16_t>(require_value(argument), argument);
    } else if (argument == "--recovery-port") {
      options.recovery_port = parse_unsigned<std::uint16_t>(require_value(argument), argument);
    } else if (argument == "--messages") {
      options.messages = parse_unsigned<std::size_t>(require_value(argument), argument);
    } else if (argument == "--messages-per-packet") {
      options.messages_per_packet = parse_unsigned<std::size_t>(require_value(argument), argument);
    } else if (argument == "--skip-every") {
      options.skip_every = parse_unsigned<std::size_t>(require_value(argument), argument);
    } else if (argument == "--interval-ms") {
      options.interval_ms = parse_unsigned<std::uint64_t>(require_value(argument), argument);
    } else if (argument == "--recovery-grace-ms") {
      options.recovery_grace_ms = parse_unsigned<std::uint64_t>(require_value(argument), argument);
    } else if (argument == "--quiet") {
      options.quiet = true;
    } else if (argument == "--help") {
      print_usage();
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + std::string(argument));
    }
  }

  if (options.messages_per_packet == 0) {
    throw std::runtime_error("--messages-per-packet must be greater than zero");
  }
  if (options.messages_per_packet > kMaxMessagesPerPacket) {
    throw std::runtime_error(
      "--messages-per-packet is too large for the fixed 1024-byte packet buffer"
    );
  }

  return options;
}

std::size_t serialize_packet_to_buffer(
  std::uint32_t seq_num,
  std::uint64_t publish_tsc,
  std::span<const bmdfh::protocol::Message> messages,
  char* buffer,
  std::size_t buffer_size
) {
  if (messages.size() > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("too many messages in one packet");
  }

  const auto msg_count = static_cast<std::uint16_t>(messages.size());
  const auto packet_size = bmdfh::protocol::packet_size_bytes(msg_count);
  if (packet_size > buffer_size) {
    throw std::runtime_error("packet would overflow the fixed publisher buffer");
  }

  char* cursor = buffer;

  // The packed structs let us write the wire image directly into the UDP buffer.
  auto* header = reinterpret_cast<bmdfh::protocol::PacketHeader*>(cursor);
  *header = bmdfh::protocol::make_packet_header(seq_num, msg_count, publish_tsc);
  cursor += sizeof(bmdfh::protocol::PacketHeader);

  for (const auto& message : messages) {
    std::visit(
      [&cursor](const auto& wire_message) {
        using WireMessage = std::remove_cv_t<std::remove_reference_t<decltype(wire_message)>>;
        auto* slot = reinterpret_cast<WireMessage*>(cursor);
        *slot = wire_message;
        cursor += sizeof(WireMessage);
      },
      message
    );
  }

  return packet_size;
}

struct LiveOrder {
  std::uint64_t order_id {};
  bmdfh::protocol::Side side {bmdfh::protocol::Side::Buy};
  std::uint32_t quantity {};
  std::uint32_t price {};
};

class MarketDataGenerator {
 public:
  bmdfh::protocol::Message next() {
    if (live_orders_.size() < 4) {
      return add_order();
    }

    const auto action = action_distribution_(random_engine_);
    if (action < 45) {
      return add_order();
    }
    if (action < 80) {
      return modify_order();
    }
    return delete_order();
  }

 private:
  bmdfh::protocol::Message add_order() {
    const auto side = side_distribution_(random_engine_) == 0 ? bmdfh::protocol::Side::Buy
                                                              : bmdfh::protocol::Side::Sell;
    const auto price = side == bmdfh::protocol::Side::Buy ? bid_price_distribution_(random_engine_)
                                                          : ask_price_distribution_(random_engine_);
    const auto quantity = quantity_distribution_(random_engine_);

    LiveOrder order {
      .order_id = next_order_id_++,
      .side = side,
      .quantity = quantity,
      .price = price,
    };

    live_orders_.push_back(order);

    return bmdfh::protocol::make_add_order_msg(
      order.order_id,
      order.side,
      order.price,
      order.quantity
    );
  }

  bmdfh::protocol::Message modify_order() {
    if (live_orders_.empty()) {
      return add_order();
    }

    auto& order = live_orders_[pick_order_index()];
    order.price =
      order.side == bmdfh::protocol::Side::Buy ? bid_price_distribution_(random_engine_)
                                               : ask_price_distribution_(random_engine_);
    order.quantity = quantity_distribution_(random_engine_);

    return bmdfh::protocol::make_modify_order_msg(order.order_id, order.price, order.quantity);
  }

  bmdfh::protocol::Message delete_order() {
    if (live_orders_.empty()) {
      return add_order();
    }

    const auto index = pick_order_index();
    const auto order_id = live_orders_[index].order_id;
    live_orders_[index] = live_orders_.back();
    live_orders_.pop_back();

    return bmdfh::protocol::make_delete_order_msg(order_id);
  }

  std::size_t pick_order_index() {
    return std::uniform_int_distribution<std::size_t>(0, live_orders_.size() - 1)(random_engine_);
  }

  std::uint64_t next_order_id_ {1};
  std::vector<LiveOrder> live_orders_ {};
  std::mt19937 random_engine_ {std::random_device {}()};
  std::uniform_int_distribution<int> action_distribution_ {0, 99};
  std::uniform_int_distribution<int> side_distribution_ {0, 1};
  std::uniform_int_distribution<std::uint32_t> bid_price_distribution_ {9'950, 10'000};
  std::uniform_int_distribution<std::uint32_t> ask_price_distribution_ {10'001, 10'050};
  std::uniform_int_distribution<std::uint32_t> quantity_distribution_ {10, 250};
};

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_args(argc, argv);
    bmdfh::UdpMulticastSender sender(options.group, options.port);
    bmdfh::PacketHistory packet_history;
    bmdfh::RecoveryServer recovery_server(options.recovery_port, packet_history);
    MarketDataGenerator generator;

    std::cout << "exchange_sim starting on " << options.group << ':' << options.port
              << " with recovery TCP " << options.recovery_port
              << " for " << options.messages << " messages"
              << " using " << options.messages_per_packet << " messages/packet";
    if (options.skip_every != 0) {
      std::cout << " and dropping every " << options.skip_every << " packet";
    }
    std::cout << '\n';

    std::size_t emitted_messages = 0;
    std::size_t sent_packets = 0;
    std::size_t dropped_packets = 0;
    std::size_t sent_messages = 0;
    std::size_t dropped_messages = 0;
    std::uint32_t packet_sequence = 1;

    while (emitted_messages < options.messages) {
      const auto batch_size = std::min(options.messages_per_packet, options.messages - emitted_messages);
      std::vector<bmdfh::protocol::Message> batch;
      batch.reserve(batch_size);

      for (std::size_t index = 0; index < batch_size; ++index) {
        batch.push_back(generator.next());
      }

      char packet_buffer[kPacketBufferSize] {};
      const auto publish_tsc = bmdfh::benchmark::read_tsc();
      const auto packet_size =
        serialize_packet_to_buffer(
          packet_sequence,
          publish_tsc,
          batch,
          packet_buffer,
          sizeof(packet_buffer)
        );
      packet_history.store(
        packet_sequence,
        std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(packet_buffer),
          packet_size
        )
      );
      const bool should_skip = options.skip_every != 0 && packet_sequence % options.skip_every == 0;

      if (should_skip) {
        ++dropped_packets;
        dropped_messages += batch.size();
        if (!options.quiet) {
          std::cout << "dropped packet seq=" << packet_sequence
                    << " msg_count=" << batch.size()
                    << " publish_tsc=" << publish_tsc
                    << " (intentional)\n";
        }
      } else {
        sender.send(
          std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(packet_buffer),
            packet_size
          )
        );
        ++sent_packets;
        sent_messages += batch.size();
        if (!options.quiet) {
          std::cout << "sent packet seq=" << packet_sequence
                    << " msg_count=" << batch.size()
                    << " bytes=" << packet_size
                    << " publish_tsc=" << publish_tsc << '\n';
        }
      }

      if (!options.quiet) {
        for (std::size_t index = 0; index < batch.size(); ++index) {
          std::cout
            << "  "
            << (should_skip ? "drop " : "send ")
            << bmdfh::protocol::describe(
                 packet_sequence,
                 index,
                 bmdfh::protocol::view_of(batch[index])
               )
            << '\n';
        }
      }

      emitted_messages += batch.size();
      ++packet_sequence;

      if (emitted_messages < options.messages) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options.interval_ms));
      }
    }

    std::cout << "summary: sent_packets=" << sent_packets
              << " dropped_packets=" << dropped_packets
              << " sent_messages=" << sent_messages
              << " dropped_messages=" << dropped_messages << '\n';

    if (options.recovery_grace_ms != 0) {
      std::cout << "keeping recovery server alive for " << options.recovery_grace_ms
                << " ms\n";
      std::this_thread::sleep_for(std::chrono::milliseconds(options.recovery_grace_ms));
    }

    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "exchange_sim error: " << exception.what() << '\n';
    return 1;
  }
}
