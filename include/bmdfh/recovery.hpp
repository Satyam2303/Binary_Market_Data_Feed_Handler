#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace bmdfh {

class PacketHistory {
 public:
  void store(std::uint32_t seq_num, std::span<const std::uint8_t> packet);
  [[nodiscard]] std::vector<std::vector<std::uint8_t>> lookup_range(
    std::uint32_t start_seq,
    std::uint32_t end_seq
  ) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::uint32_t, std::vector<std::uint8_t>> packets_;
};

class RecoveryServer {
 public:
  RecoveryServer(std::uint16_t port, const PacketHistory& packet_history);
  ~RecoveryServer();

  RecoveryServer(const RecoveryServer&) = delete;
  RecoveryServer& operator=(const RecoveryServer&) = delete;

 private:
  void run();
  void handle_client(int client_fd) const;

  int listen_fd_ {-1};
  std::uint16_t port_ {};
  const PacketHistory& packet_history_;
  std::atomic_bool stop_ {false};
  std::thread thread_;
};

class RecoveryClient {
 public:
  RecoveryClient(std::string host, std::uint16_t port);
  ~RecoveryClient();

  RecoveryClient(const RecoveryClient&) = delete;
  RecoveryClient& operator=(const RecoveryClient&) = delete;

  [[nodiscard]] std::vector<std::vector<std::uint8_t>> request_packets(
    std::uint32_t start_seq,
    std::uint32_t end_seq
  );

 private:
  void ensure_connected();
  void close_connection();

  std::string host_;
  std::uint16_t port_ {};
  int socket_fd_ {-1};
};

}  // namespace bmdfh
