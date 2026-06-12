# SlickQueue

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Header-only](https://img.shields.io/badge/header--only-yes-brightgreen.svg)](#installation)
[![Lock-free](https://img.shields.io/badge/concurrency-lock--free-orange.svg)](#architecture)
[![CI](https://github.com/SlickQuant/slick-queue/actions/workflows/ci.yml/badge.svg)](https://github.com/SlickQuant/slick-queue/actions/workflows/ci.yml)
[![GitHub release](https://img.shields.io/github/v/release/SlickQuant/slick-queue)](https://github.com/SlickQuant/slick-queue/releases)

SlickQueue is a header-only C++ library that provides a lock-free,
multi-producer multi-consumer (MPMC) queue built on a ring buffer. It is
designed for high throughput concurrent messaging and can optionally operate
over shared memory for inter-process communication.

## Features

- **Lock-free operations** for multiple producers and consumers
- **Header-only implementation** - just include and go
- **Zero dynamic allocation** on the hot path for predictable performance
- **Shared memory support** for inter-process communication
- **Cross-platform** - supports Windows, Linux, and macOS
- **Modern C++20** implementation

## Requirements

- C++20 compatible compiler
- CMake 3.10+ (for building tests)

## Installation

SlickQueue is header-only. Simply add the `include` directory to your project's include path:

```cpp
#include "slick/queue.h"
```

### Using vcpkg

SlickQueue is available in the [vcpkg](https://github.com/microsoft/vcpkg) package manager:

```bash
vcpkg install slick-queue
```

Then in your CMakeLists.txt:

```cmake
find_package(slick-queue CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE slick::queue)
```

### Using CMake FetchContent

```cmake
include(FetchContent)

# Disable tests for slick-queue
set(BUILD_SLICK_QUEUE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    slick-queue
    GIT_REPOSITORY https://github.com/SlickQuant/slick-queue.git
    GIT_TAG v1.1.0.0  # See https://github.com/SlickQuant/slick-queue/releases for latest version
)
FetchContent_MakeAvailable(slick-queue)

target_link_libraries(your_target PRIVATE slick::queue)
```

## Usage

> **Note**: `slick::queue<T>` is the preferred name and is used throughout this documentation. It is an alias template for `slick::SlickQueue<T>`, which remains available for backward compatibility.

### Basic Example

```cpp
#include "slick/queue.h"

// Create a queue with 1024 slots (must be power of 2)
slick::queue<int> queue(1024);

// Producer: reserve a slot, write data, and publish
auto slot = queue.reserve();
*queue[slot] = 42;
queue.publish(slot);

// Consumer: read from queue using a cursor
uint64_t cursor = 0;
auto result = queue.read(cursor);
if (result.first != nullptr) {
    int value = *result.first;  // value == 42
}
```

### Shared Memory Example (IPC)

```cpp
#include "slick/queue.h"

// Process 1 (Server/Writer)
slick::queue<int> server(1024, "my_queue");
auto slot = server.reserve();
*server[slot] = 100;
server.publish(slot);

// Process 2 (Client/Reader)
slick::queue<int> client("my_queue");
uint64_t cursor = 0;
auto result = client.read(cursor);
if (result.first != nullptr) {
    int value = *result.first;  // value == 100
}
```

### Multi-Producer Multi-Consumer

```cpp
#include "slick/queue.h"
#include <thread>

slick::queue<int> queue(1024);

// Multiple producers
auto producer = [&](int id) {
    for (int i = 0; i < 100; ++i) {
        auto slot = queue.reserve();
        *queue[slot] = id * 1000 + i;
        queue.publish(slot);
    }
};

// Multiple consumers (each maintains independent cursor)
// Note: Each consumer will see ALL published items (broadcast pattern)
auto consumer = [&](int id) {
    uint64_t cursor = 0;
    int count = 0;
    while (count < 200) {  // 2 producers × 100 items each
        auto result = queue.read(cursor);
        if (result.first != nullptr) {
            int value = *result.first;
            ++count;
        }
    }
};

std::thread p1(producer, 1);
std::thread p2(producer, 2);
std::thread c1(consumer, 1);
std::thread c2(consumer, 2);

p1.join(); p2.join();
c1.join(); c2.join();
```

### Work-Stealing with Shared Atomic Cursor

```cpp
#include "slick/queue.h"
#include <thread>
#include <atomic>

slick::queue<int> queue(1024);
std::atomic<uint64_t> shared_cursor{0};

// Multiple producers
auto producer = [&](int id) {
    for (int i = 0; i < 100; ++i) {
        auto slot = queue.reserve();
        *queue[slot] = id * 1000 + i;
        queue.publish(slot);
    }
};

// Multiple consumers sharing atomic cursor (work-stealing/load-balancing)
// Each item is consumed by exactly ONE consumer
auto consumer = [&]() {
    int count = 0;
    for (int i = 0; i < 100; ++i) {
        auto result = queue.read(shared_cursor);
        if (result.first != nullptr) {
            int value = *result.first;
            ++count;
        }
    }
    return count;
};

std::thread p1(producer, 1);
std::thread p2(producer, 2);
std::thread c1(consumer);
std::thread c2(consumer);

p1.join(); p2.join();
c1.join(); c2.join();
// Total items consumed: 200 (each item consumed exactly once)
```

## API Overview

### Constructor

```cpp
// In-process queue
queue(uint32_t size);

// Shared memory queue
queue(uint32_t size, const char* shm_name);  // Writer/Creator
queue(const char* shm_name);                  // Reader/Attacher
```

### Core Methods

- `uint64_t reserve(uint32_t n = 1)` - Reserve `n` slots for writing (non-blocking; may overwrite old data if consumers lag)
- `T* operator[](uint64_t slot)` - Access reserved slot
- `void publish(uint64_t slot, uint32_t n = 1)` - Publish `n` written items to consumers
- `std::pair<T*, uint32_t> read(uint64_t& cursor)` - Read next available item (independent cursor)
- `std::pair<T*, uint32_t> read(std::atomic<uint64_t>& cursor)` - Read next available item (shared atomic cursor for work-stealing)
- `std::pair<T*, uint32_t> read_last()` - Read the most recently published item without a cursor
- `uint32_t size()` - Get queue capacity
- `uint64_t loss_count() const` - Get count of skipped items due to overwrite (debug-only if enabled)
- `void reset()` - Reset the queue, invalidating all existing data

### Important Constraints

**Lock-Free Atomics Implementation**: slick::queue uses a packed 64-bit atomic internally to guarantee lock-free operations on all platforms. This packs both the write index (48 bits) and the reservation size (16 bits) into a single atomic value.

**Lossy Semantics**: slick::queue does not apply backpressure. If producers advance by at least the queue size before a consumer reads, older entries will be overwritten and the consumer will skip ahead to the latest value for a slot. Size the queue and read frequency to bound loss.

**Debug Loss Detection**: Define `SLICK_QUEUE_ENABLE_LOSS_DETECTION=1` to enable a per-instance skipped-item counter (enabled by default in Debug builds). Use `loss_count()` to inspect how many items were skipped.

**CPU Relax Backoff**: Define `SLICK_QUEUE_ENABLE_CPU_RELAX=0` to disable the pause/yield backoff used on contended CAS loops (default is enabled). Disabling may reduce latency in very short contention bursts but can increase CPU usage under load.

**⚠️ Reserve Size Limitation (legacy shared memory)**: Older shared-memory segments used the 16-bit size stored in the packed reservation atomic to compute `read_last()`. New segments track the last published index separately, so this limit no longer applies in normal use.

- If you attach to a shared-memory segment created by older versions, keep `reserve(n) <= 65,535` when using `read_last()`
- For typical use cases with `reserve()` or `reserve(1)`, this limit is not a concern
- The 48-bit index supports up to 2^48 (281 trillion) iterations, sufficient for any practical application

## Performance Characteristics

- **Lock-free**: No mutex contention between producers/consumers
- **Wait-free reads**: Consumers never block each other
- **Cache-friendly**: Ring buffer design with power-of-2 sizing
- **Predictable**: No allocations or system calls on hot path (except for initial reserve when full)

## Building and Testing

### Build Tests

```bash
cmake -S . -B build
cmake --build build
```

### Run Tests

```bash
# Using CTest
cd build
ctest --output-on-failure

# Or run directly
./build/tests/slick_queue_tests
```

### Build Options

- `BUILD_SLICK_QUEUE_TESTS` - Enable/disable test building (default: ON)
- `CMAKE_BUILD_TYPE` - Set to `Release` or `Debug`

## License

SlickQueue is released under the [MIT License](LICENSE).

**Made with ⚡ by [SlickQuant](https://github.com/SlickQuant)**


