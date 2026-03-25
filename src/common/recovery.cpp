#include "bmdfh/recovery.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace bmdfh {
namespace {

constexpr std::size_t kMaxRecoveryPacketSize = 4096;

struct __attribute__((packed)) RecoveryRequestWire {
  std::uint32_t start_seq;
  std::uint32_t end_seq;
};

struct __attribute__((packed)) RecoveryResponseWire {
  std::uint32_t packet_count;
};

struct __attribute__((packed)) RecoveryPacketFrameWire {
  std::uint16_t packet_size;
};

static_assert(sizeof(RecoveryRequestWire) == 8);
static_assert(sizeof(RecoveryResponseWire) == 4);
static_assert(sizeof(RecoveryPacketFrameWire) == 2);

std::runtime_error socket_error(std::string_view action) {
  return std::runtime_error(std::string(action) + ": " + std::strerror(errno));
}

void close_if_open(int& socket_fd) {
  if (socket_fd >= 0) {
    ::close(socket_fd);
    socket_fd = -1;
  }
}

void send_all(int socket_fd, const void* data, std::size_t size) {
  const auto* cursor = static_cast<const std::uint8_t*>(data);
  std::size_t remaining = size;

  while (remaining > 0) {
    const auto sent = ::send(socket_fd, cursor, remaining, 0);
    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw socket_error("send");
    }

    if (sent == 0) {
      throw std::runtime_error("send returned zero bytes");
    }

    cursor += static_cast<std::size_t>(sent);
    remaining -= static_cast<std::size_t>(sent);
  }
}

bool recv_exact_or_eof(int socket_fd, void* data, std::size_t size) {
  auto* cursor = static_cast<std::uint8_t*>(data);
  std::size_t received_total = 0;

  while (received_total < size) {
    const auto received = ::recv(socket_fd, cursor + received_total, size - received_total, 0);
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw socket_error("recv");
    }

    if (received == 0) {
      if (received_total == 0) {
        return false;
      }
      throw std::runtime_error("unexpected EOF on recovery socket");
    }

    received_total += static_cast<std::size_t>(received);
  }

  return true;
}

int create_listen_socket(std::uint16_t port) {
  const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    throw socket_error("socket");
  }

  const int reuse_address = 1;
  if (::setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) < 0) {
    const auto error = socket_error("setsockopt(SO_REUSEADDR)");
    ::close(socket_fd);
    throw error;
  }

  sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_ANY);

  if (::bind(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
    const auto error = socket_error("bind");
    ::close(socket_fd);
    throw error;
  }

  if (::listen(socket_fd, 4) < 0) {
    const auto error = socket_error("listen");
    ::close(socket_fd);
    throw error;
  }

  return socket_fd;
}

}  // namespace

void PacketHistory::store(std::uint32_t seq_num, std::span<const std::uint8_t> packet) {
  std::lock_guard lock(mutex_);
  packets_[seq_num] = std::vector<std::uint8_t>(packet.begin(), packet.end());
}

std::vector<std::vector<std::uint8_t>> PacketHistory::lookup_range(
  std::uint32_t start_seq,
  std::uint32_t end_seq
) const {
  std::vector<std::vector<std::uint8_t>> packets;
  if (start_seq > end_seq) {
    return packets;
  }

  std::lock_guard lock(mutex_);
  for (std::uint32_t seq_num = start_seq; seq_num <= end_seq; ++seq_num) {
    const auto iterator = packets_.find(seq_num);
    if (iterator != packets_.end()) {
      packets.push_back(iterator->second);
    }
    if (seq_num == std::numeric_limits<std::uint32_t>::max()) {
      break;
    }
  }

  return packets;
}

RecoveryServer::RecoveryServer(std::uint16_t port, const PacketHistory& packet_history)
  : listen_fd_(create_listen_socket(port)), port_(port), packet_history_(packet_history),
    thread_([this] { run(); }) {}

RecoveryServer::~RecoveryServer() {
  stop_ = true;
  if (thread_.joinable()) {
    thread_.join();
  }
  close_if_open(listen_fd_);
}

void RecoveryServer::run() {
  while (!stop_) {
    pollfd listen_poll {};
    listen_poll.fd = listen_fd_;
    listen_poll.events = POLLIN;

    const auto poll_result = ::poll(&listen_poll, 1, 200);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "recovery_server error: " << socket_error("poll").what() << '\n';
      return;
    }

    if (poll_result == 0 || (listen_poll.revents & POLLIN) == 0) {
      continue;
    }

    const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "recovery_server error: " << socket_error("accept").what() << '\n';
      continue;
    }

    try {
      handle_client(client_fd);
    } catch (const std::exception& exception) {
      std::cerr << "recovery_server client error: " << exception.what() << '\n';
    }

    ::close(client_fd);
  }
}

void RecoveryServer::handle_client(int client_fd) const {
  while (!stop_) {
    RecoveryRequestWire request_wire {};
    if (!recv_exact_or_eof(client_fd, &request_wire, sizeof(request_wire))) {
      return;
    }

    const auto start_seq = ntohl(request_wire.start_seq);
    const auto end_seq = ntohl(request_wire.end_seq);
    const auto packets = packet_history_.lookup_range(start_seq, end_seq);

    const auto packet_count = static_cast<std::uint32_t>(packets.size());
    RecoveryResponseWire response_wire {
      .packet_count = htonl(packet_count),
    };
    send_all(client_fd, &response_wire, sizeof(response_wire));

    for (const auto& packet : packets) {
      if (packet.empty() || packet.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("recovery packet size is out of bounds");
      }

      RecoveryPacketFrameWire frame_wire {
        .packet_size = htons(static_cast<std::uint16_t>(packet.size())),
      };
      send_all(client_fd, &frame_wire, sizeof(frame_wire));
      send_all(client_fd, packet.data(), packet.size());
    }
  }
}

RecoveryClient::RecoveryClient(std::string host, std::uint16_t port)
  : host_(std::move(host)), port_(port) {}

RecoveryClient::~RecoveryClient() {
  close_connection();
}

std::vector<std::vector<std::uint8_t>> RecoveryClient::request_packets(
  std::uint32_t start_seq,
  std::uint32_t end_seq
) {
  if (start_seq > end_seq) {
    return {};
  }

  ensure_connected();

  RecoveryRequestWire request_wire {
    .start_seq = htonl(start_seq),
    .end_seq = htonl(end_seq),
  };
  send_all(socket_fd_, &request_wire, sizeof(request_wire));

  RecoveryResponseWire response_wire {};
  if (!recv_exact_or_eof(socket_fd_, &response_wire, sizeof(response_wire))) {
    close_connection();
    throw std::runtime_error("recovery server closed the connection");
  }

  const auto packet_count = ntohl(response_wire.packet_count);
  const auto expected_count = static_cast<std::uint64_t>(end_seq - start_seq) + 1;
  if (packet_count > expected_count) {
    throw std::runtime_error("recovery server returned more packets than requested");
  }

  std::vector<std::vector<std::uint8_t>> packets;
  packets.reserve(packet_count);

  for (std::uint32_t index = 0; index < packet_count; ++index) {
    RecoveryPacketFrameWire frame_wire {};
    if (!recv_exact_or_eof(socket_fd_, &frame_wire, sizeof(frame_wire))) {
      close_connection();
      throw std::runtime_error("recovery server closed during packet replay");
    }

    const auto packet_size = ntohs(frame_wire.packet_size);
    if (packet_size == 0 || packet_size > kMaxRecoveryPacketSize) {
      throw std::runtime_error("recovery server returned an invalid packet size");
    }

    std::vector<std::uint8_t> packet(packet_size);
    if (!recv_exact_or_eof(socket_fd_, packet.data(), packet.size())) {
      close_connection();
      throw std::runtime_error("recovery server closed during packet replay");
    }
    packets.push_back(std::move(packet));
  }

  return packets;
}

void RecoveryClient::ensure_connected() {
  if (socket_fd_ >= 0) {
    return;
  }

  socket_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd_ < 0) {
    throw socket_error("socket");
  }

  sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port_);
  if (::inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) {
    close_connection();
    throw std::runtime_error("invalid recovery host: " + host_);
  }

  if (::connect(socket_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
    const auto error = socket_error("connect");
    close_connection();
    throw error;
  }
}

void RecoveryClient::close_connection() {
  close_if_open(socket_fd_);
}

}  // namespace bmdfh
