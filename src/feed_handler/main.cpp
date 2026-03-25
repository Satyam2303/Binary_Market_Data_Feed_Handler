#include "bmdfh/benchmark.hpp"
#include "bmdfh/order_book.hpp"
#include "bmdfh/protocol.hpp"
#include "bmdfh/recovery.hpp"
#include "bmdfh/udp_multicast.hpp"

#include <array>
#include <chrono>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <unistd.h>
#include <vector>

namespace {

struct Options {
  std::string group {"239.255.42.99"};
  std::uint16_t port {30001};
  std::string recovery_host {"127.0.0.1"};
  std::uint16_t recovery_port {31001};
  std::size_t messages {0};
  std::size_t cpu_core {0};
  bool benchmark {false};
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
    << "Usage: feed_handler [--group ADDRESS] [--port PORT] [--recovery-host ADDRESS] "
       "[--recovery-port PORT] [--messages COUNT] [--cpu-core CORE] [--benchmark]\n";
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
    } else if (argument == "--recovery-host") {
      options.recovery_host = std::string(require_value(argument));
    } else if (argument == "--recovery-port") {
      options.recovery_port = parse_unsigned<std::uint16_t>(require_value(argument), argument);
    } else if (argument == "--messages") {
      options.messages = parse_unsigned<std::size_t>(require_value(argument), argument);
    } else if (argument == "--cpu-core") {
      options.cpu_core = parse_unsigned<std::size_t>(require_value(argument), argument);
    } else if (argument == "--benchmark") {
      options.benchmark = true;
    } else if (argument == "--help") {
      print_usage();
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + std::string(argument));
    }
  }

  return options;
}

enum class PacketSource {
  Live,
  Replay,
  Drain,
};

[[nodiscard]] std::string_view to_string(PacketSource source) {
  switch (source) {
    case PacketSource::Live:
      return "live";
    case PacketSource::Replay:
      return "replay";
    case PacketSource::Drain:
      return "drain";
  }

  return "unknown";
}

void pin_current_thread_to_cpu(std::size_t cpu_core) {
  const auto cpu_count_raw = ::sysconf(_SC_NPROCESSORS_ONLN);
  if (cpu_count_raw <= 0) {
    throw std::runtime_error("sysconf(_SC_NPROCESSORS_ONLN) failed");
  }

  const auto cpu_count = static_cast<std::size_t>(cpu_count_raw);
  if (cpu_core >= cpu_count) {
    throw std::runtime_error(
      "--cpu-core must be smaller than the online CPU count (" + std::to_string(cpu_count) + ")"
    );
  }

  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  CPU_SET(static_cast<int>(cpu_core), &cpu_set);

  const int result = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set), &cpu_set);
  if (result != 0) {
    throw std::runtime_error(
      "pthread_setaffinity_np failed: " + std::string(std::strerror(result))
    );
  }
}

struct CycleStats {
  std::size_t samples {0};
  std::uint64_t min_cycles {std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t max_cycles {0};
  long double total_cycles {0.0L};

  void record(std::uint64_t cycles) {
    ++samples;
    if (cycles < min_cycles) {
      min_cycles = cycles;
    }
    if (cycles > max_cycles) {
      max_cycles = cycles;
    }
    total_cycles += static_cast<long double>(cycles);
  }

  [[nodiscard]] std::string summary(std::string_view label) const {
    std::ostringstream stream;
    stream << label << ": ";
    if (samples == 0) {
      stream << "no samples";
      return stream.str();
    }

    const auto average_cycles = total_cycles / static_cast<long double>(samples);
    stream << "samples=" << samples
           << " min=" << min_cycles
           << " avg=" << static_cast<std::uint64_t>(average_cycles)
           << " max=" << max_cycles
           << " cycles";
    return stream.str();
  }
};

class EpollInstance {
 public:
  EpollInstance() {
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) {
      throw std::runtime_error("epoll_create1 failed");
    }
  }

  ~EpollInstance() {
    if (epoll_fd_ >= 0) {
      ::close(epoll_fd_);
    }
  }

  EpollInstance(const EpollInstance&) = delete;
  EpollInstance& operator=(const EpollInstance&) = delete;

  void add_readable_fd(int fd, std::uint64_t data) const {
    epoll_event event {};
    event.events = EPOLLIN;
    event.data.u64 = data;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) < 0) {
      throw std::runtime_error("epoll_ctl(EPOLL_CTL_ADD) failed");
    }
  }

  int wait(std::span<epoll_event> events) const {
    while (true) {
      const auto ready_count = ::epoll_wait(
        epoll_fd_,
        events.data(),
        static_cast<int>(events.size()),
        -1
      );
      if (ready_count >= 0) {
        return ready_count;
      }

      if (errno != EINTR) {
        throw std::runtime_error("epoll_wait failed");
      }
    }
  }

 private:
  int epoll_fd_ {-1};
};

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_args(argc, argv);
    pin_current_thread_to_cpu(options.cpu_core);
    bmdfh::UdpMulticastReceiver receiver(options.group, options.port);
    bmdfh::RecoveryClient recovery_client(options.recovery_host, options.recovery_port);
    EpollInstance epoll;
    bmdfh::OrderBook order_book;
    std::array<std::uint8_t, 4096> buffer {};
    std::array<epoll_event, 8> events {};
    std::map<std::uint32_t, std::vector<std::uint8_t>> buffered_packets;
    CycleStats packet_latency_cycles;
    CycleStats bbo_update_latency_cycles;
    std::size_t processed_packets = 0;
    std::size_t live_packets = 0;
    std::size_t replay_packets = 0;
    std::size_t drain_packets = 0;
    std::size_t replay_requests = 0;
    std::size_t out_of_order_packets = 0;
    const auto run_start = std::chrono::steady_clock::now();
    const bool verbose = !options.benchmark;

    epoll.add_readable_fd(receiver.native_handle(), 1);

    std::cout << "feed_handler listening on " << options.group << ':' << options.port
              << " with epoll and recovery TCP " << options.recovery_host
              << ':' << options.recovery_port
              << " pinned to cpu " << options.cpu_core;
    if (options.benchmark) {
      std::cout << " in benchmark mode";
    }
    std::cout << '\n';

    std::size_t received_messages = 0;
    std::uint32_t expected_packet_seq = 1;
    const auto limit_reached = [&]() {
      return options.messages != 0 && received_messages >= options.messages;
    };

    const auto process_packet_bytes = [&](std::span<const std::uint8_t> packet_bytes,
                                          PacketSource source) {
      const auto packet = bmdfh::protocol::parse_packet(packet_bytes);
      if (!packet) {
        throw std::runtime_error(std::string(to_string(source)) + " packet is malformed");
      }

      const auto packet_seq = bmdfh::protocol::decode_seq_num(*packet->header);
      const auto msg_count = bmdfh::protocol::decode_msg_count(*packet->header);
      const auto publish_tsc = bmdfh::protocol::decode_publish_tsc(*packet->header);
      if (packet_seq != expected_packet_seq) {
        throw std::runtime_error(
          std::string(to_string(source)) + " packet sequence mismatch: expected seq=" +
          std::to_string(expected_packet_seq) + " got seq=" + std::to_string(packet_seq)
        );
      }

      if (verbose) {
        std::cout << to_string(source) << " packet seq=" << packet_seq
                  << " msg_count=" << msg_count
                  << " publish_tsc=" << publish_tsc << '\n';
      }

      for (std::size_t index = 0; index < msg_count; ++index) {
        const auto message = bmdfh::protocol::message_at(*packet, index);
        if (!message) {
          throw std::runtime_error(
            "malformed message from " + std::string(to_string(source)) + " packet seq=" +
            std::to_string(packet_seq) + " at index=" + std::to_string(index)
          );
        }

        const auto apply_result = order_book.apply(*message);
        ++received_messages;

        if (apply_result.bbo_changed()) {
          const auto update_tsc = bmdfh::benchmark::read_tsc();
          const auto latency_cycles = update_tsc - publish_tsc;
          if (publish_tsc != 0) {
            bbo_update_latency_cycles.record(latency_cycles);
          }

          if (verbose) {
            std::cout << "alert BBO changed: "
                      << bmdfh::OrderBook::format_bbo(apply_result.before)
                      << " -> "
                      << bmdfh::OrderBook::format_bbo(apply_result.after)
                      << " | latency_cycles=" << latency_cycles << '\n';
          }
        }

        if (verbose) {
          std::cout << to_string(source) << " recv "
                    << bmdfh::protocol::describe(packet_seq, index, *message)
                    << " | " << order_book.top_of_book()
                    << " | live_orders=" << order_book.live_order_count() << '\n';
        }

        if (limit_reached()) {
          break;
        }
      }

      const auto applied_tsc = bmdfh::benchmark::read_tsc();
      if (publish_tsc != 0) {
        packet_latency_cycles.record(applied_tsc - publish_tsc);
      }
      ++processed_packets;
      switch (source) {
        case PacketSource::Live:
          ++live_packets;
          break;
        case PacketSource::Replay:
          ++replay_packets;
          break;
        case PacketSource::Drain:
          ++drain_packets;
          break;
      }

      expected_packet_seq = packet_seq + 1;
    };

    const auto drain_buffered_packets = [&]() {
      while (!limit_reached()) {
        const auto iterator = buffered_packets.find(expected_packet_seq);
        if (iterator == buffered_packets.end()) {
          break;
        }

        auto packet_bytes = std::move(iterator->second);
        buffered_packets.erase(iterator);
        process_packet_bytes(packet_bytes, PacketSource::Drain);
      }
    };

    const auto request_replay = [&](std::uint32_t start_seq, std::uint32_t end_seq) {
      if (start_seq > end_seq || limit_reached()) {
        return;
      }

      ++replay_requests;
      if (verbose) {
        std::cout << "requesting replay for packets " << start_seq
                  << " to " << end_seq << " over TCP\n";
      }

      auto replay_batch = recovery_client.request_packets(start_seq, end_seq);
      const auto expected_count = static_cast<std::size_t>(end_seq - start_seq) + 1;
      if (replay_batch.size() != expected_count) {
        throw std::runtime_error(
          "recovery server returned " + std::to_string(replay_batch.size()) +
          " packets for gap [" + std::to_string(start_seq) + ", " + std::to_string(end_seq) +
          "]"
        );
      }

      for (auto& replay_packet : replay_batch) {
        if (limit_reached()) {
          break;
        }
        process_packet_bytes(replay_packet, PacketSource::Replay);
      }

      drain_buffered_packets();
    };

    while (!limit_reached()) {
      const auto ready_count = epoll.wait(events);
      for (int event_index = 0; event_index < ready_count && !limit_reached(); ++event_index) {
        const auto& event = events[static_cast<std::size_t>(event_index)];
        if ((event.events & (EPOLLERR | EPOLLHUP)) != 0U) {
          throw std::runtime_error("epoll reported a socket error");
        }

        if ((event.events & EPOLLIN) == 0U) {
          continue;
        }

        while (!limit_reached()) {
          const auto received_bytes = receiver.try_receive(buffer);
          if (!received_bytes) {
            break;
          }

          const auto packet = bmdfh::protocol::parse_packet(
            std::span<const std::uint8_t>(buffer.data(), *received_bytes)
          );

          if (!packet) {
            std::cerr << "discarded malformed packet (" << *received_bytes << " bytes)\n";
            continue;
          }

          const auto packet_seq = bmdfh::protocol::decode_seq_num(*packet->header);
          const auto msg_count = bmdfh::protocol::decode_msg_count(*packet->header);
          const auto packet_bytes = std::span<const std::uint8_t>(buffer.data(), *received_bytes);

          if (packet_seq < expected_packet_seq) {
            std::cerr << "discarded stale packet seq=" << packet_seq
                      << " expected seq=" << expected_packet_seq << '\n';
            continue;
          }

          if (packet_seq > expected_packet_seq) {
            ++out_of_order_packets;
            buffered_packets[packet_seq] =
              std::vector<std::uint8_t>(packet_bytes.begin(), packet_bytes.end());
            if (verbose) {
              std::cout << "buffered out-of-order packet seq=" << packet_seq
                        << " msg_count=" << msg_count << '\n';
            }

            const auto gap_end = buffered_packets.begin()->first - 1;
            request_replay(expected_packet_seq, gap_end);
            continue;
          }

          process_packet_bytes(packet_bytes, PacketSource::Live);
          drain_buffered_packets();
        }

        if (limit_reached()) {
          break;
        }
      }
    }

    const auto elapsed_seconds =
      std::chrono::duration<long double>(std::chrono::steady_clock::now() - run_start).count();
    const auto message_rate = elapsed_seconds > 0.0L
                                ? static_cast<long double>(received_messages) / elapsed_seconds
                                : 0.0L;
    const auto packet_rate = elapsed_seconds > 0.0L
                               ? static_cast<long double>(processed_packets) / elapsed_seconds
                               : 0.0L;

    std::cout << "summary: messages=" << received_messages
              << " packets=" << processed_packets
              << " live_packets=" << live_packets
              << " replay_packets=" << replay_packets
              << " drain_packets=" << drain_packets
              << " replay_requests=" << replay_requests
              << " out_of_order_packets=" << out_of_order_packets
              << " elapsed_s=" << static_cast<double>(elapsed_seconds)
              << " msg_rate=" << static_cast<double>(message_rate)
              << "/s packet_rate=" << static_cast<double>(packet_rate)
              << "/s\n";
    std::cout << packet_latency_cycles.summary("packet_publish_to_apply") << '\n';
    std::cout << bbo_update_latency_cycles.summary("packet_publish_to_bbo_update") << '\n';

    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "feed_handler error: " << exception.what() << '\n';
    return 1;
  }
}
