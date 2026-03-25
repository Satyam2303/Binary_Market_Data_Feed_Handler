#include "bmdfh/udp_multicast.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace bmdfh {
namespace {

std::runtime_error socket_error(std::string_view action) {
  return std::runtime_error(std::string(action) + ": " + std::strerror(errno));
}

void close_if_open(int& socket_fd) {
  if (socket_fd >= 0) {
    ::close(socket_fd);
    socket_fd = -1;
  }
}

void set_non_blocking(int socket_fd) {
  const auto flags = ::fcntl(socket_fd, F_GETFL, 0);
  if (flags < 0) {
    throw socket_error("fcntl(F_GETFL)");
  }

  if (::fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    throw socket_error("fcntl(F_SETFL)");
  }
}

sockaddr_in make_address(const std::string& group, std::uint16_t port) {
  sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);

  if (::inet_pton(AF_INET, group.c_str(), &address.sin_addr) != 1) {
    throw std::runtime_error("invalid multicast group: " + group);
  }

  return address;
}

}  // namespace

UdpMulticastSender::UdpMulticastSender(const std::string& group, std::uint16_t port)
  : destination_(make_address(group, port)) {
  socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd_ < 0) {
    throw socket_error("socket");
  }

  const int ttl = 1;
  if (::setsockopt(socket_fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
    close_if_open(socket_fd_);
    throw socket_error("setsockopt(IP_MULTICAST_TTL)");
  }

  const int loopback = 1;
  if (::setsockopt(socket_fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loopback, sizeof(loopback)) < 0) {
    close_if_open(socket_fd_);
    throw socket_error("setsockopt(IP_MULTICAST_LOOP)");
  }
}

UdpMulticastSender::~UdpMulticastSender() {
  close_if_open(socket_fd_);
}

void UdpMulticastSender::send(std::span<const std::uint8_t> payload) const {
  const auto sent = ::sendto(
    socket_fd_,
    payload.data(),
    payload.size(),
    0,
    reinterpret_cast<const sockaddr*>(&destination_),
    sizeof(destination_)
  );

  if (sent < 0) {
    throw socket_error("sendto");
  }

  if (static_cast<std::size_t>(sent) != payload.size()) {
    throw std::runtime_error("partial UDP datagram send");
  }
}

UdpMulticastReceiver::UdpMulticastReceiver(const std::string& group, std::uint16_t port) {
  socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd_ < 0) {
    throw socket_error("socket");
  }

  const int reuse_address = 1;
  if (::setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) <
      0) {
    close_if_open(socket_fd_);
    throw socket_error("setsockopt(SO_REUSEADDR)");
  }

  sockaddr_in local_address {};
  local_address.sin_family = AF_INET;
  local_address.sin_port = htons(port);
  local_address.sin_addr.s_addr = htonl(INADDR_ANY);

  if (::bind(socket_fd_, reinterpret_cast<const sockaddr*>(&local_address), sizeof(local_address)) <
      0) {
    close_if_open(socket_fd_);
    throw socket_error("bind");
  }

  ip_mreq membership {};
  membership.imr_multiaddr = make_address(group, port).sin_addr;
  membership.imr_interface.s_addr = htonl(INADDR_ANY);

  if (::setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) <
      0) {
    close_if_open(socket_fd_);
    throw socket_error("setsockopt(IP_ADD_MEMBERSHIP)");
  }

  try {
    set_non_blocking(socket_fd_);
  } catch (...) {
    close_if_open(socket_fd_);
    throw;
  }
}

UdpMulticastReceiver::~UdpMulticastReceiver() {
  close_if_open(socket_fd_);
}

int UdpMulticastReceiver::native_handle() const {
  return socket_fd_;
}

std::optional<std::size_t> UdpMulticastReceiver::try_receive(std::span<std::uint8_t> buffer) const {
  const auto received = ::recvfrom(socket_fd_, buffer.data(), buffer.size(), 0, nullptr, nullptr);
  if (received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return std::nullopt;
    }
    throw socket_error("recvfrom");
  }

  return static_cast<std::size_t>(received);
}

}  // namespace bmdfh
