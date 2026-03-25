#pragma once

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace bmdfh {

class UdpMulticastSender {
 public:
  UdpMulticastSender(const std::string& group, std::uint16_t port);
  ~UdpMulticastSender();

  UdpMulticastSender(const UdpMulticastSender&) = delete;
  UdpMulticastSender& operator=(const UdpMulticastSender&) = delete;

  void send(std::span<const std::uint8_t> payload) const;

 private:
  int socket_fd_ {-1};
  struct sockaddr_in destination_;
};

class UdpMulticastReceiver {
 public:
  UdpMulticastReceiver(const std::string& group, std::uint16_t port);
  ~UdpMulticastReceiver();

  UdpMulticastReceiver(const UdpMulticastReceiver&) = delete;
  UdpMulticastReceiver& operator=(const UdpMulticastReceiver&) = delete;

  [[nodiscard]] int native_handle() const;
  [[nodiscard]] std::optional<std::size_t> try_receive(std::span<std::uint8_t> buffer) const;

 private:
  int socket_fd_ {-1};
};

}  // namespace bmdfh
