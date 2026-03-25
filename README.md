# Binary Market Data Feed Handler

Phase 1 through Phase 8 bootstrap for a Linux-based binary market data simulator and feed handler.

## Toolchain

This project targets Linux and builds with:

- GCC or Clang
- CMake
- Git

## Project Layout

- `exchange_sim`: emits fake binary market data packets over UDP multicast
- `feed_handler`: joins the multicast group, decodes packets, and updates an in-memory order book
- `bmdfh_core`: shared library for the packed wire protocol, Linux multicast sockets, and order-book logic

## Feed Handler I/O

The feed handler now uses Linux `epoll` for network I/O:

- The multicast receiver still binds the UDP socket locally and joins the multicast group with `IP_ADD_MEMBERSHIP`.
- The receiver socket is placed into non-blocking mode.
- `feed_handler` creates an `epoll` instance with `epoll_create1`, registers the UDP socket with `epoll_ctl`, and waits for readability with `epoll_wait`.
- When the socket becomes readable, the handler drains all available datagrams before returning to the event loop.

## Exchange Simulator

The simulator now behaves like a simple publisher:

- It maintains a live-order pool so `ModifyOrder` and `DeleteOrder` only target valid orders.
- It assembles each UDP datagram into a fixed 1024-byte `char` buffer by writing a packed `PacketHeader` first and then advancing a pointer through packed messages.
- It can intentionally drop packet sequence numbers with `--skip-every N` so the receiver can observe packet gaps.

## Binary Protocol

Each UDP datagram starts with a packed 14-byte header:

```cpp
struct __attribute__((packed)) PacketHeader {
  uint32_t seq_num;
  uint16_t msg_count;
  uint64_t publish_tsc;
};
```

The payload is `msg_count` fixed-size 24-byte packed messages. The project currently supports:

- `AddOrder`
- `ModifyOrder`
- `DeleteOrder`
- `Trade`

This layout is designed for zero-copy parsing in the handler: after validating the packet size, the code reinterprets the packet buffer directly as packed structs instead of deserializing field-by-field.

## Zero-Copy Parsing

- `feed_handler` reads into a pre-allocated buffer and points a packed `PacketHeader` directly at the start of the datagram.
- Packet and message integers are carried in Big-Endian network byte order.
- The handler keeps parsing zero-copy, but converts fields to host order with `ntohs` / `ntohl` and a 64-bit byte-swap helper before using sequence numbers, prices, quantities, and order IDs.
- Message iteration works by decoding `msg_count`, advancing past the header, and casting each fixed-size message slot based on `msg_type`.

## Limit Order Book

- Active orders are stored in a `std::unordered_map<uint64_t, Order>` for O(1) lookup by `order_id`.
- Aggregated price levels are stored in two sorted `std::map<uint32_t, uint32_t>` containers: one for bids and one for asks.
- The best bid is read from `bids.rbegin()` and the best ask is read from `asks.begin()`.
- `feed_handler` prints an alert whenever the BBO snapshot changes after applying a message.

## Gap Fill / Recovery

- The handler tracks `expected_packet_seq` and treats any higher sequence number as a packet gap.
- Out-of-order live packets are parked in a temporary `std::map<uint32_t, std::vector<uint8_t>>` instead of being applied immediately.
- The simulator keeps a history of serialized packets keyed by sequence number.
- A secondary TCP recovery channel lets `feed_handler` request a missing range such as packets `100` through `101`.
- The simulator replays the missing packets over TCP, the handler applies them in order, and then it drains any buffered live packets before returning to the multicast stream.

## Benchmarking & Profiling

- Each packet now carries a send-side `publish_tsc` stamped with `__builtin_ia32_rdtsc()` immediately before the simulator calls `sendto()`.
- `feed_handler` records the cycle count when the packet has been fully applied and when the BBO changes, then prints latency summaries at shutdown.
- The handler pins its epoll thread with `pthread_setaffinity_np()` and accepts `--cpu-core CORE` to keep benchmark runs on a fixed CPU.
- `--benchmark` suppresses per-message logging in `feed_handler`, and `--quiet` suppresses per-packet logs in `exchange_sim`, so stdout does not dominate the profile.
- Exact kernel egress is not directly observable from user space with `rdtsc`; this phase records the closest practical user-space stamp immediately before `sendto()`.

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

Start the feed handler in one terminal:

```bash
./build/feed_handler --group 239.255.42.99 --port 30001 --recovery-host 127.0.0.1 --recovery-port 31001 --messages 20 --cpu-core 0
```

Start the simulator in another:

```bash
./build/exchange_sim --group 239.255.42.99 --port 30001 --recovery-port 31001 --messages 20 --messages-per-packet 4 --interval-ms 100
```

To test gap fill, intentionally drop every second packet while leaving the TCP recovery server on:

```bash
./build/exchange_sim --group 239.255.42.99 --port 30001 --recovery-port 31001 --messages 12 --messages-per-packet 3 --skip-every 2 --interval-ms 100
```

For a low-noise benchmark run:

```bash
./build/feed_handler --group 239.255.42.99 --port 30001 --recovery-host 127.0.0.1 --recovery-port 31001 --messages 20000 --cpu-core 0 --benchmark
./build/exchange_sim --group 239.255.42.99 --port 30001 --recovery-port 31001 --messages 20000 --messages-per-packet 4 --interval-ms 0 --quiet
```

On a Linux machine with matching `perf` tooling installed, profile the handler with:

```bash
perf stat ./build/feed_handler --group 239.255.42.99 --port 30001 --recovery-host 127.0.0.1 --recovery-port 31001 --messages 20000 --cpu-core 0 --benchmark
perf record -g ./build/feed_handler --group 239.255.42.99 --port 30001 --recovery-host 127.0.0.1 --recovery-port 31001 --messages 20000 --cpu-core 0 --benchmark
perf report
```

## Notes

- The packed wire format is defined in `include/bmdfh/protocol.hpp`.
- The wire format uses network byte order, so the handler must decode integer fields before using them on the host CPU.
- TCP replay support lives in `include/bmdfh/recovery.hpp` and `src/common/recovery.cpp`.
