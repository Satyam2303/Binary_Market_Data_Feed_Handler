# Binary Market Data Feed Handler Project

## 1. Project Overview

This project is a Linux-based market data pipeline that simulates a simplified exchange and a matching feed handler.

It has two main executables:

1. `exchange_sim`
2. `feed_handler`

The simulator generates binary market data messages, batches them into UDP multicast packets, and publishes them.

The feed handler subscribes to the multicast group, reads packets with `epoll`, parses the binary protocol with zero-copy techniques, maintains a limit order book, detects packet loss, requests replay over TCP, and measures end-to-end latency in CPU cycles.

This project was built in phases:

1. Environment and architecture setup
2. Binary protocol definition
3. Exchange simulator
4. Feed handler network I/O
5. Zero-copy parsing and endianness handling
6. Limit order book
7. Gap fill and recovery
8. Benchmarking and profiling

## 2. Real-World Motivation

Real exchanges do not send human-readable JSON for their hottest market data paths. They send compact binary packets because:

1. Binary formats are smaller
2. Fewer bytes means fewer cache misses
3. Fewer copies means lower latency
4. Fixed-size messages are simple and fast to parse
5. Multicast lets one publisher fan out to many consumers

This project mirrors those ideas in a smaller and easier-to-understand form.

## 3. High-Level Architecture

```text
MarketDataGenerator
        |
        v
exchange_sim
  - builds binary packet
  - stamps publish_tsc
  - stores packet in replay history
  - sends UDP multicast
        |
        v
239.255.42.99:30001
        |
        v
feed_handler
  - epoll_wait()
  - recvfrom() into pre-allocated buffer
  - parse packed structs directly
  - detect sequence gaps
  - request replay over TCP if needed
  - update order book
  - compute BBO
  - record latency statistics
```

There is also a recovery side channel:

```text
feed_handler <---- TCP replay ----> exchange_sim
                 127.0.0.1:31001
```

## 4. Project Layout

Important files and directories:

| Path | Purpose |
| --- | --- |
| `CMakeLists.txt` | Build configuration |
| `include/bmdfh/protocol.hpp` | Packed binary wire protocol |
| `src/common/protocol.cpp` | Serialization, parsing, endian conversion |
| `include/bmdfh/order_book.hpp` | Order book interface |
| `src/common/order_book.cpp` | Order book logic |
| `include/bmdfh/udp_multicast.hpp` | UDP multicast socket wrappers |
| `src/common/udp_multicast.cpp` | Linux multicast sender and receiver |
| `include/bmdfh/recovery.hpp` | TCP replay interfaces |
| `src/common/recovery.cpp` | Replay server and replay client |
| `include/bmdfh/benchmark.hpp` | `rdtsc` helper |
| `src/exchange_sim/main.cpp` | Simulator entry point |
| `src/feed_handler/main.cpp` | Feed handler entry point |
| `README.md` | Quickstart and short notes |
| `project.md` | Full technical explanation |

## 5. Build System

The project uses CMake and C++20.

Build facts:

1. Minimum CMake version: `3.20`
2. C++ standard: `20`
3. Shared library target: `bmdfh_core`
4. Executables:
   - `exchange_sim`
   - `feed_handler`
5. Warning flags on GCC or Clang:
   - `-Wall`
   - `-Wextra`
   - `-Wpedantic`
   - `-Wconversion`
   - `-Wshadow`

Build commands:

```bash
cmake -S . -B build
cmake --build build
```

## 6. Runtime Defaults

### 6.1 `exchange_sim`

| Flag | Default | Meaning |
| --- | --- | --- |
| `--group` | `239.255.42.99` | Multicast IPv4 group |
| `--port` | `30001` | UDP multicast port |
| `--recovery-port` | `31001` | TCP replay port |
| `--messages` | `20` | Number of generated messages |
| `--messages-per-packet` | `4` | Number of fixed-size messages per UDP packet |
| `--skip-every` | `0` | Intentionally drop every Nth packet, `0` means disabled |
| `--interval-ms` | `100` | Delay between packets in milliseconds |
| `--recovery-grace-ms` | `1000` | Keep replay server alive after publishing finishes |
| `--quiet` | `false` | Suppress detailed send logs |

### 6.2 `feed_handler`

| Flag | Default | Meaning |
| --- | --- | --- |
| `--group` | `239.255.42.99` | Multicast IPv4 group |
| `--port` | `30001` | UDP multicast port |
| `--recovery-host` | `127.0.0.1` | Replay server host |
| `--recovery-port` | `31001` | Replay server port |
| `--messages` | `0` | Number of messages to process, `0` means unlimited |
| `--cpu-core` | `0` | CPU core for thread affinity |
| `--benchmark` | `false` | Suppress verbose per-message logs and print summary stats |

## 7. Binary Protocol

The project uses a packed binary protocol.

All packets are in network byte order:

1. `uint16_t` uses `htons` and `ntohs`
2. `uint32_t` uses `htonl` and `ntohl`
3. `uint64_t` uses `__builtin_bswap64()` on little-endian x86 hosts

### 7.1 Packet Header

Every UDP packet begins with this packed header:

```cpp
struct __attribute__((packed)) PacketHeader {
  uint32_t seq_num;
  uint16_t msg_count;
  uint64_t publish_tsc;
};
```

Field sizes:

| Field | Bytes | Meaning |
| --- | --- | --- |
| `seq_num` | 4 | Packet sequence number |
| `msg_count` | 2 | Number of messages in this UDP packet |
| `publish_tsc` | 8 | `rdtsc` stamp captured just before `sendto()` |

Total header size:

```text
4 + 2 + 8 = 14 bytes
```

### 7.2 Message Types

All messages are fixed at `24` bytes.

Supported types:

1. `AddOrder`
2. `ModifyOrder`
3. `DeleteOrder`
4. `Trade`

### 7.3 Exact Message Layouts

#### AddOrderMsg

```cpp
struct __attribute__((packed)) AddOrderMsg {
  char msg_type;        // 'A'
  uint64_t order_id;
  char side;            // 'B' or 'S'
  uint32_t price;
  uint32_t quantity;
  uint8_t reserved[6];
};
```

Size math:

```text
1 + 8 + 1 + 4 + 4 + 6 = 24 bytes
```

#### ModifyOrderMsg

```cpp
struct __attribute__((packed)) ModifyOrderMsg {
  char msg_type;        // 'M'
  uint64_t order_id;
  uint32_t price;
  uint32_t quantity;
  uint8_t reserved[7];
};
```

Size math:

```text
1 + 8 + 4 + 4 + 7 = 24 bytes
```

#### DeleteOrderMsg

```cpp
struct __attribute__((packed)) DeleteOrderMsg {
  char msg_type;        // 'D'
  uint64_t order_id;
  uint8_t reserved[15];
};
```

Size math:

```text
1 + 8 + 15 = 24 bytes
```

#### TradeMsg

```cpp
struct __attribute__((packed)) TradeMsg {
  char msg_type;        // 'T'
  uint64_t order_id;
  uint32_t price;
  uint32_t quantity;
  uint8_t reserved[7];
};
```

Size math:

```text
1 + 8 + 4 + 4 + 7 = 24 bytes
```

### 7.4 Packet Size Formula

If a packet contains `N` messages:

```text
packet_size = 14 + 24 * N
```

Examples:

| `msg_count` | Packet size |
| --- | --- |
| `1` | `14 + 24 = 38` bytes |
| `2` | `14 + 48 = 62` bytes |
| `4` | `14 + 96 = 110` bytes |
| `10` | `14 + 240 = 254` bytes |
| `42` | `14 + 1008 = 1022` bytes |

### 7.5 Why 42 Messages Is the Maximum

The simulator uses a fixed packet buffer of `1024` bytes.

Maximum message count:

```text
floor((1024 - 14) / 24)
= floor(1010 / 24)
= 42
```

Remaining unused bytes:

```text
1024 - 1022 = 2 bytes
```

So:

1. `42` messages fit
2. `43` messages do not fit
3. The maximum UDP payload used by this project is `1022` bytes

This is safely below a common Ethernet MTU path limit:

```text
UDP payload  = 1022
UDP header   = 8
IPv4 header  = 20
Total        = 1050 bytes
```

`1050` bytes is comfortably below `1500`, so the default packet size avoids IP fragmentation on a normal Ethernet MTU.

## 8. Numeric Representation

### 8.1 Price Encoding

Prices are stored as integer ticks, effectively cents in this project.

Examples:

| Human price | Encoded integer |
| --- | --- |
| `$100.00` | `10000` |
| `$100.29` | `10029` |
| `$150.25` | `15025` |

Reason:

1. Integer math is faster and safer than floating point for price fields
2. No rounding drift
3. Exact byte layout is simpler

### 8.2 Side Encoding

| Side | Encoded byte |
| --- | --- |
| Buy | `'B'` |
| Sell | `'S'` |

### 8.3 Message Type Encoding

| Message | Encoded byte |
| --- | --- |
| Add | `'A'` |
| Modify | `'M'` |
| Delete | `'D'` |
| Trade | `'T'` |

## 9. Exchange Simulator

The simulator is the publisher side of the system.

Its responsibilities are:

1. Generate plausible market activity
2. Keep internal order state valid
3. Serialize messages into a binary buffer
4. Stamp sequence numbers and `publish_tsc`
5. Send UDP multicast
6. Store packet history for replay
7. Intentionally drop packets when testing recovery

### 9.1 Market Data Generation Rules

The simulator maintains a live vector of orders.

Warm-up rule:

1. If there are fewer than `4` active orders, it always generates `AddOrder`

After warm-up, action probabilities are:

| Action | Probability |
| --- | --- |
| Add | `45%` |
| Modify | `35%` |
| Delete | `20%` |

This comes from:

1. `action < 45` => add
2. `45 <= action < 80` => modify
3. `80 <= action <= 99` => delete

### 9.2 Simulator Price and Quantity Ranges

| Field | Range |
| --- | --- |
| Bid price | `9950` to `10000` |
| Ask price | `10001` to `10050` |
| Quantity | `10` to `250` |

These ranges guarantee that bids are normally below asks, so the book looks realistic.

### 9.3 Packet Sequence Numbers

Packet sequence numbers start at `1`.

After each emitted packet:

```text
packet_sequence = packet_sequence + 1
```

This sequence is the basis for packet-loss detection in the feed handler.

### 9.4 Why the Simulator Stores Packets Before Dropping Them

The simulator stores each fully serialized packet in `PacketHistory` before deciding whether to skip live transmission.

That is important because:

1. A deliberately skipped live packet still needs to exist for replay
2. The replay path must be able to resend the original exact packet bytes

## 10. UDP Multicast Transport

The hot market data path uses UDP multicast.

### 10.1 Sender

The multicast sender:

1. Opens an IPv4 UDP socket
2. Sets `IP_MULTICAST_TTL = 1`
3. Sets `IP_MULTICAST_LOOP = 1`
4. Sends packets with `sendto()`

### 10.2 Receiver

The multicast receiver:

1. Opens an IPv4 UDP socket
2. Sets `SO_REUSEADDR`
3. Binds to `INADDR_ANY:port`
4. Joins the multicast group with `IP_ADD_MEMBERSHIP`
5. Switches the socket into non-blocking mode

### 10.3 Buffers

Important sizes:

| Buffer | Size |
| --- | --- |
| Simulator packet buffer | `1024` bytes |
| Feed handler UDP receive buffer | `4096` bytes |
| Recovery replay packet guard | `4096` bytes |

The receive buffer is much larger than the maximum live packet size:

```text
4096 - 1022 = 3074 bytes spare
```

## 11. Feed Handler Core

The feed handler is the consumer side of the system.

Its responsibilities are:

1. Wait for socket readiness using `epoll`
2. Read UDP packets into a pre-allocated buffer
3. Parse the packed binary protocol without copying fields
4. Detect missing packet sequences
5. Buffer out-of-order packets
6. Request replay over TCP
7. Update the order book
8. Compute best bid and best ask
9. Measure latency

### 11.1 epoll Model

The feed handler uses:

1. `epoll_create1()`
2. `epoll_ctl(..., EPOLL_CTL_ADD, ...)`
3. `epoll_wait()`

Flow:

1. Wait for readability
2. When readable, call `recvfrom()` repeatedly
3. Stop only when `EAGAIN` or `EWOULDBLOCK` is returned
4. Return to `epoll_wait()`

This is a classic Linux event-loop pattern.

### 11.2 Zero-Copy Parsing

The handler does not deserialize the packet field-by-field into a new structure.

Instead:

1. It receives raw bytes into a buffer
2. It casts the first bytes to `PacketHeader`
3. It advances over the header
4. It casts each `24`-byte message slot according to `msg_type`

Why this matters:

1. Fewer copies
2. Less CPU work
3. Simpler hot path

The only conversions performed are endian corrections when the values are actually used.

## 12. Limit Order Book Design

The order book keeps two types of state:

1. Individual live orders
2. Aggregated quantity at each price level

### 12.1 Data Structures

| Structure | Type | Reason |
| --- | --- | --- |
| Live orders | `std::unordered_map<uint64_t, OrderState>` | Fast lookup by `order_id` |
| Bid levels | `std::map<uint32_t, uint32_t>` | Sorted price levels |
| Ask levels | `std::map<uint32_t, uint32_t>` | Sorted price levels |

### 12.2 Complexity

Average-case complexity:

| Operation | Complexity |
| --- | --- |
| Find order by `order_id` | `O(1)` average |
| Insert or erase price level in map | `O(log P)` |
| Modify order | `O(1) + O(log P)` |
| Delete order | `O(1) + O(log P)` |
| Trade update | `O(1) + O(log P)` |
| Best bid lookup | `O(1)` via `rbegin()` |
| Best ask lookup | `O(1)` via `begin()` |

Where `P` is the number of distinct price levels, not the number of orders.

### 12.3 Message Semantics

#### Add

1. Decode `side`, `order_id`, `price`, `quantity`
2. Remove any existing order with the same `order_id`
3. Insert the new order into `live_orders_`
4. Add quantity to bid or ask map

#### Modify

1. Find the existing order
2. Remove its old contribution from the aggregated price level
3. Replace price and quantity
4. If new quantity is `0`, erase the order
5. Otherwise add the new contribution

#### Delete

1. Find the order
2. Remove its quantity from the correct price level
3. Erase it from `live_orders_`

#### Trade

1. Find the resting order
2. Execute `min(resting_qty, trade_qty)`
3. Subtract executed quantity from the price level
4. Reduce the order's remaining quantity
5. Erase the order if it reaches `0`

### 12.4 Best Bid and Offer

The book uses:

1. `bids_.rbegin()` for the highest bid
2. `asks_.begin()` for the lowest ask

Example:

If the aggregated book is:

```text
Bids:
10000 -> 99
 9998 -> 50

Asks:
10008 -> 145
10025 -> 144
```

Then the BBO is:

```text
BID 10000 x 99 | ASK 10008 x 145
```

### 12.5 BBO Change Alert

Each message application returns:

1. BBO before the message
2. BBO after the message

If they differ, the handler prints an alert in normal mode and records benchmark timing for BBO-change latency.

## 13. Gap Fill and Recovery Logic

UDP does not guarantee:

1. Delivery
2. Ordering
3. Duplicate suppression

So the feed handler implements gap recovery.

### 13.1 Sequence Tracking

The handler maintains:

```text
expected_packet_seq
```

If it expects `100` and receives:

1. `100` => process normally
2. `99` => stale or duplicate, discard
3. `102` => gap detected, packets `100` and `101` are missing

### 13.2 Temporary Buffer for Out-of-Order Packets

Out-of-order packets are stored in:

```text
std::map<uint32_t, std::vector<uint8_t>>
```

Why a `std::map`:

1. Keys stay sorted by sequence number
2. The handler can efficiently find the next expected buffered packet

Complexity:

1. Insert buffered packet: `O(log G)`
2. Lookup next expected sequence: `O(log G)`

Where `G` is the number of buffered out-of-order packets.

### 13.3 Recovery TCP Protocol

The replay side channel uses three packed structs.

#### Recovery request

```cpp
struct RecoveryRequestWire {
  uint32_t start_seq;
  uint32_t end_seq;
};
```

Size:

```text
4 + 4 = 8 bytes
```

#### Recovery response

```cpp
struct RecoveryResponseWire {
  uint32_t packet_count;
};
```

Size:

```text
4 bytes
```

#### Recovery packet frame

```cpp
struct RecoveryPacketFrameWire {
  uint16_t packet_size;
};
```

Size:

```text
2 bytes
```

### 13.4 Recovery Flow Example

Suppose the handler receives packets in this order:

```text
1, 2, 4, 5
```

But it expected:

```text
1, 2, 3, 4, 5
```

Then the recovery flow is:

1. Receive `1`, process
2. Receive `2`, process
3. Receive `4`, detect gap
4. Buffer packet `4`
5. Request replay for `3..3` over TCP
6. Receive replayed packet `3`
7. Process packet `3`
8. Drain buffered packet `4`
9. Continue with live stream

### 13.5 Replay History Design

The simulator stores replay history in:

```text
std::map<uint32_t, std::vector<uint8_t>>
```

`lookup_range(start_seq, end_seq)` scans the inclusive range and returns the packets it finds.

Current behavior:

1. If the replay server returns fewer packets than requested, the handler throws an error
2. This means the replay channel expects exact recovery for the requested range

### 13.6 Important Recovery Limitations

Current limitations:

1. Final dropped packet detection is incomplete if no later packet arrives
2. There is no heartbeat or timeout-based end-of-stream recovery trigger
3. `PacketHistory` currently grows without eviction
4. Recovery server is simple and intended for a small number of clients
5. Replay server handles accepted clients sequentially

The missing final-packet case is important.

Example:

1. Expected next packet is `10`
2. Packet `10` is lost
3. Publisher stops
4. No packet `11` arrives
5. The handler never learns there was a gap

Production systems usually solve that with:

1. Heartbeats
2. End-of-batch markers
3. Time-based gap detection
4. Snapshot plus incremental replay

## 14. Benchmarking and Profiling

Phase 8 adds benchmarking support.

### 14.1 TSC Timestamping

The project uses:

```cpp
__builtin_ia32_rdtsc()
```

This returns the x86 Time Stamp Counter.

The simulator stamps:

```text
publish_tsc
```

right before it calls `sendto()`.

The handler measures:

1. Packet publish to packet fully applied
2. Packet publish to BBO updated

### 14.2 Why This Is Useful

Cycle counts are useful because:

1. They avoid wall-clock timer granularity issues
2. They are cheap to read
3. They are commonly used in low-latency systems

### 14.3 Important Accuracy Note

This project does **not** measure the exact moment the packet leaves kernel space.

What it measures is the closest practical user-space point:

1. `publish_tsc` immediately before `sendto()`
2. Update TSC in the handler after application or BBO change

So the measured path includes:

1. User-space send call overhead
2. Kernel networking path
3. Loopback or local multicast delivery path
4. User-space receive path
5. Parsing
6. Order book update

### 14.4 CPU Pinning

The feed handler pins its thread with:

```cpp
pthread_setaffinity_np()
```

Benefits:

1. Less scheduler migration noise
2. Better cache locality
3. More stable latency measurements

Validation:

1. The code checks the number of online CPUs with `sysconf(_SC_NPROCESSORS_ONLN)`
2. The requested core must be `< cpu_count`

### 14.5 Benchmark Output

In benchmark mode the handler prints summary lines such as:

```text
summary: messages=5000 packets=1250 ...
packet_publish_to_apply: samples=1250 min=16077 avg=983966 max=4074501 cycles
packet_publish_to_bbo_update: samples=196 min=14560 avg=851286 max=4066415 cycles
```

Interpretation:

1. `samples=1250` means 1250 packets were timed for full-apply latency
2. `samples=196` means only 196 packets changed the BBO
3. `min`, `avg`, and `max` are in CPU cycles, not microseconds

### 14.6 Throughput Math Example

For the local benchmark example:

```text
messages = 5000
elapsed_s = 1.01199
```

Message rate:

```text
5000 / 1.01199 = 4940.78 messages/second
```

Packet count:

```text
5000 messages / 4 messages per packet = 1250 packets
```

Packet rate:

```text
1250 / 1.01199 = 1235.20 packets/second
```

### 14.7 perf Usage

Recommended commands on a Linux machine with matching kernel tools:

```bash
perf stat ./build/feed_handler --group 239.255.42.99 --port 30001 --recovery-host 127.0.0.1 --recovery-port 31001 --messages 20000 --cpu-core 0 --benchmark
perf record -g ./build/feed_handler --group 239.255.42.99 --port 30001 --recovery-host 127.0.0.1 --recovery-port 31001 --messages 20000 --cpu-core 0 --benchmark
perf report
```

Important WSL2 note:

If running under WSL2, `perf` may fail unless the matching `linux-tools` package for the active kernel is installed.

## 15. End-to-End Processing Path

This is the exact logical flow of one packet.

### 15.1 On the simulator side

1. Generate `N` messages
2. Compute `publish_tsc`
3. Fill `PacketHeader`
4. Copy `N` packed messages into the `1024` byte buffer
5. Save the serialized bytes into packet history
6. Optionally drop the packet for testing
7. Otherwise call `sendto()`

### 15.2 On the handler side

1. `epoll_wait()` wakes up
2. `recvfrom()` fills the pre-allocated `4096` byte buffer
3. Cast the packet header directly from the buffer
4. Decode `seq_num`, `msg_count`, `publish_tsc`
5. Detect stale, in-order, or out-of-order packet
6. If out-of-order, buffer it and request replay
7. Iterate the `msg_count` fixed-size message slots
8. Apply each message to the order book
9. If the BBO changes, record BBO latency
10. Record packet-apply latency
11. Drain any buffered packets that now match the expected sequence

## 16. Complexity Summary

| Area | Complexity |
| --- | --- |
| Packet header parse | `O(1)` |
| Iterate packet messages | `O(M)` where `M = msg_count` |
| Per-message order lookup | `O(1)` average |
| Per-message price-level update | `O(log P)` |
| Packet replay buffer insert | `O(log G)` |
| Replay history range lookup | `O(R log H)` in this implementation |
| Best bid or ask lookup | `O(1)` |

Where:

1. `M` = number of messages in one packet
2. `P` = number of active price levels
3. `G` = number of buffered out-of-order packets
4. `R` = number of requested replay sequence values
5. `H` = number of stored historical packets

## 17. Safety Checks and Validation

The code includes several practical checks:

1. Rejects malformed packets if the byte count does not match `14 + 24 * msg_count`
2. Rejects invalid message types
3. Rejects invalid side bytes
4. Rejects stale packets
5. Rejects replay responses that return more packets than requested
6. Rejects replay responses whose count does not match the gap requested
7. Rejects oversize replay packet frames
8. Validates `--messages-per-packet` against the `1024` byte packet buffer
9. Validates requested CPU affinity core

## 18. Known Limitations

This project is intentionally focused and educational, so it does not yet include:

1. Full NASDAQ ITCH compatibility
2. Snapshot recovery
3. Persistent storage
4. Multiple feed handler threads
5. Lock-free order book structures
6. Packet-history eviction or bounded replay memory
7. Heartbeat-based gap detection
8. Exact kernel egress timestamps
9. Authentication or encryption
10. IPv6 multicast support

## 19. Suggested Future Improvements

If this project were extended further, the next strong upgrades would be:

1. Add heartbeats so the handler can detect a missing final packet
2. Add replay history retention limits or ring buffers
3. Add a snapshot channel for cold-start recovery
4. Replace `std::map` price levels with flatter cache-friendly structures
5. Add histogram-based latency reporting instead of only min, avg, max
6. Add sequence-gap timeout logic
7. Add unit tests for protocol, recovery, and order book logic
8. Add integration tests that intentionally drop and replay packets

## 20. Final Summary

This project demonstrates the core ideas behind a market data feed handler:

1. Fixed-length packed binary messages
2. Zero-copy parsing
3. Network byte order handling
4. UDP multicast for the live path
5. TCP replay for reliability
6. `epoll` for scalable Linux I/O
7. A limit order book for stateful market reconstruction
8. BBO calculation
9. Cycle-based benchmarking
10. CPU affinity for more stable latency measurement

In short, this is a compact but realistic market data systems project that combines:

1. Systems programming
2. Linux networking
3. binary protocols
4. event-driven I/O
5. data-structure design
6. loss recovery
7. low-latency measurement
