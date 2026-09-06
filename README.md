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
- **Shared memory support** for inter-process communication, backed by [slick-shm](https://github.com/SlickQuant/slick-shm)
- **Cross-platform** - supports Windows, Linux, and macOS
- **Modern C++20** implementation

## Requirements

- C++20 compatible compiler
- CMake 3.10+ (for building tests)
- [slick-shm](https://github.com/SlickQuant/slick-shm) v0.1.5+ - header-only shared memory library used by `slick/queue.h`

`slick/queue.h` includes `<slick/shm/shared_memory.hpp>`, so slick-shm must be available on the
include path even when the queue is used purely in-process. The vcpkg port and the CMake build both
resolve this dependency for you; only a manual copy-the-headers installation requires action.

## Installation

### Header-Only (manual)

SlickQueue is header-only, but so is its slick-shm dependency - copy **both** `include` directories
into your project and add **both** to your include path:

```
third_party/
    slick-queue/include/slick/queue.h
    slick-shm/include/slick/shm/...
```

```bash
g++ -std=c++20 -Ithird_party/slick-queue/include -Ithird_party/slick-shm/include main.cpp
```

Both libraries install under the same `slick/` prefix, so their `include` directories can also be
merged into a single tree (`include/slick/queue.h` alongside `include/slick/shm/`) and passed as one
`-I` path. Then:

```cpp
#include "slick/queue.h"
```

On Linux, also link `rt`, `pthread`, and `atomic` (`-lrt -lpthread -latomic`); macOS needs only
`-lpthread` and Windows needs no extra libraries.

### Using vcpkg

SlickQueue is available in the [vcpkg](https://github.com/microsoft/vcpkg) package manager:

```bash
vcpkg install slick-queue
```

The port declares `slick-shm` as a dependency, so it is installed automatically. Then in your
CMakeLists.txt:

```cmake
find_package(slick-queue CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE slick::queue)
```

Linking `slick::queue` transitively pulls in `slick::shm` and the platform libraries it needs.

### Using CMake FetchContent

```cmake
include(FetchContent)

# Disable tests for slick-queue
set(BUILD_SLICK_QUEUE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    slick-queue
    GIT_REPOSITORY https://github.com/SlickQuant/slick-queue.git
    GIT_TAG v2.0.0  # See https://github.com/SlickQuant/slick-queue/releases for latest version
)
FetchContent_MakeAvailable(slick-queue)

target_link_libraries(your_target PRIVATE slick::queue)
```

slick-queue first looks for an installed slick-shm with `find_package(slick-shm CONFIG)`; if none is
found it fetches slick-shm itself via FetchContent, so no extra declaration is needed. To pin your
own version, make `slick::shm` available (installed or declared) before
`FetchContent_MakeAvailable(slick-queue)`.

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
- `uint64_t loss_count() const` - Get count of skipped items due to overwrite (0 when the feature is off)
- `void reset()` - Reset the queue, invalidating all existing data

### Configuring Features (Traits)

Optional features are selected through a traits template parameter, not preprocessor macros:

```cpp
template<typename T, queue_traits_type Traits = default_queue_traits>
class SlickQueue;                 // slick::queue<T, Traits> is the same template
```

Derive from `slick::queue_traits` and override only what you need:

```cpp
struct my_traits : slick::queue_traits {
    static constexpr bool enable_reset_check = true;   // opt in
    static constexpr bool enable_read_last   = false;  // opt out
};

slick::queue<int, my_traits> lean(1024);
slick::queue<int>            standard(1024);  // default traits - both can coexist
```

| Trait | Default | Effect when enabled |
| --- | --- | --- |
| `enable_read_last` | `true` | `publish()` maintains a last-published index so `read_last()` works. Costs one CAS per publish and one cacheline per instance. |
| `enable_reset_check` | `false` | `read()` loads the producer's reservation counter and rewinds the cursor to 0 if it has run past it, which happens only when `reset()` rewound the counter. Without this, a reader holding a pre-`reset()` cursor returns `nullptr` indefinitely and then skips the start of the new generation. |
| `enable_loss_detection` | Debug only | Per-instance skipped-item counter, reported by `loss_count()`. Costs one cacheline per instance. |
| `enable_cpu_relax` | `true` | pause/yield backoff on contended CAS loops. Disabling may cut latency in short bursts but raises CPU use under load. |

`default_queue_traits` is `debug_queue_traits` (loss detection on) in Debug builds and `queue_traits` (off) in Release, matching the previous `NDEBUG`-keyed behaviour. `slick::queue<T>` and `slick::SlickQueue<T>` therefore keep working exactly as before.

Notes:

- **`read_last()` requires `enable_read_last`.** Calling it otherwise is a compile error, not a silent fallback to the old reserved-cursor heuristic, which reported reservations that were never published and truncated sizes above 65,535.
- **`enable_read_last` must match across a shared-memory segment.** It is the one trait that changes the shared header protocol, so the creator records it in the segment's layout marker (`'SLQ1'` when the last-published index is maintained, `'SLQ0'` when it is not) and every attacher checks it. A peer that disagrees is rejected with a `std::runtime_error` at construction instead of silently corrupting the other side's view - in either direction: an attacher that expects the index would read a counter nobody writes, and one that does not maintain it would freeze `read_last()` for every peer that does. The other traits are local to each process and can differ freely on one segment.
- **A misspelled override is silent.** `enable_reset_chek = true` in a derived traits struct leaves the inherited member visible and keeps the base value. The `queue_traits_type` concept catches a wrong *type*, but cannot catch a typo.

> **Migrating from `SLICK_QUEUE_ENABLE_*`**: the two macros are gone. Defining one now emits a warning and has no effect - move the setting into a traits struct. Because differently configured queues are different types, mismatched translation units fail to link instead of silently violating the ODR as the macros did.

### Important Constraints

**Lock-Free Atomics Implementation**: slick::queue uses a packed 64-bit atomic internally to guarantee lock-free operations on all platforms. This packs both the write index (48 bits) and the reservation size (16 bits) into a single atomic value.

**Lossy Semantics**: slick::queue does not apply backpressure. If producers advance by at least the queue size before a consumer reads, older entries will be overwritten and the consumer will skip ahead to the latest value for a slot. Size the queue and read frequency to bound loss.

**Reserve Size**: `read_last()` reads the last published index, which is tracked separately from the packed reservation atomic, so the 16-bit reservation size no longer bounds it. Segments created before v1.4.0 carry no layout marker and are rejected at attach time, so that limitation cannot be reached any more.

- The 48-bit index supports up to 2^48 (281 trillion) iterations, sufficient for any practical application

## Performance Characteristics

- **Lock-free**: No mutex contention between producers/consumers
- **Wait-free reads**: Consumers never block each other
- **Cache-friendly**: Ring buffer design with power-of-2 sizing
- **Predictable**: No allocations or system calls on hot path (except for initial reserve when full)
- **Tunable hot path**: per-instantiation traits remove per-operation atomics from the reader/writer hot path when a feature is not needed, and drop its aligned counter from the object (see [Configuring Features (Traits)](#configuring-features-traits))

## Building and Testing

### Build Tests

```bash
cmake -S . -B build
cmake --build build
```

The configure step resolves slick-shm with `find_package(slick-shm CONFIG)` and falls back to
downloading it with FetchContent, so the first configure needs network access unless slick-shm is
already installed (for example via vcpkg, using `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`).
GoogleTest is fetched the same way for the test target.

### Run Tests

```bash
# Using CTest
cd build
ctest --output-on-failure

# Or run directly
./build/tests/slick-queue-tests
```

### Build Options

- `BUILD_SLICK_QUEUE_TESTS` - Enable/disable test building (default: ON)
- `CMAKE_BUILD_TYPE` - Set to `Release` or `Debug`

## License

SlickQueue is released under the [MIT License](LICENSE).

**Made with ⚡ by [SlickQuant](https://github.com/SlickQuant)**


