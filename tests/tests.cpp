#include <gtest/gtest.h>
#include <slick/queue.hpp>
#include <atomic>
#include <string>
#include <thread>
#include <cstring>

#include "test_traits.h"

using namespace slick;

TEST(SlickQueueTests, ReadEmptyQueue) {
  slick::queue<int> queue(2);
  uint64_t read_cursor = 0;
  auto read = queue.read(read_cursor);
  EXPECT_EQ(read.first, nullptr);
}

TEST(SlickQueueTests, Reserve) {
  slick::queue<int> queue(2);
  auto reserved = queue.reserve();
  EXPECT_EQ(reserved, 0);
  EXPECT_EQ(queue.reserve(), 1);
  EXPECT_EQ(queue.reserve(), 2);
}

TEST(SlickQueueTests, ReadShouldFailWithoutPublish) {
  slick::queue<int> queue(2);
  uint64_t read_cursor = 0;
  auto reserved = queue.reserve();
  auto read = queue.read(read_cursor);
  EXPECT_EQ(read.first, nullptr);
  EXPECT_EQ(read_cursor, 0);
}

TEST(SlickQueueTests, InvalidSizeThrows) {
  EXPECT_THROW({
    slick::queue<int> queue(3);
  }, std::invalid_argument);
}

TEST(SlickQueueTests, ReserveZeroThrows) {
  slick::queue<int> queue(2);
  EXPECT_THROW({
    queue.reserve(0);
  }, std::invalid_argument);
}

TEST(SlickQueueTests, PublishAndRead) {
  slick::queue<int> queue(2);
  uint64_t read_cursor = 0;
  auto reserved = queue.reserve();
  *queue[reserved] = 5;
  queue.publish(reserved);
  auto read = queue.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(read_cursor, 1);
  EXPECT_EQ(*read.first, 5);
}

TEST(SlickQueueTests, PublishAndReadMultiple) {
  slick::queue<int> queue(4);
  uint64_t read_cursor = 0;
  auto reserved = queue.reserve();
  *queue[reserved] = 5;
  queue.publish(reserved);
  auto reserved1 = queue.reserve();
  *queue[reserved1] = 12;
  auto reserved2 = queue.reserve();
  *queue[reserved2] = 23;
  queue.publish(reserved2);
  auto read = queue.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(read_cursor, 1);
  EXPECT_EQ(*read.first, 5);

  read = queue.read(read_cursor);
  EXPECT_EQ(read.first, nullptr);
  EXPECT_EQ(read_cursor, 1);

  queue.publish(reserved1);
  read = queue.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(read_cursor, 2);
  EXPECT_EQ(*read.first, 12);

  read = queue.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(read_cursor, 3);
  EXPECT_EQ(*read.first, 23);
}

TEST(SlickQueueTests, BufferWrap) {
  slick::queue<char> queue(8);
  uint64_t read_cursor = 0;

  auto reserved = queue.reserve(3);
  EXPECT_EQ(reserved, 0);
  memcpy(queue[reserved], "123", 3);
  queue.publish(reserved, 3);
  auto read = queue.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(read_cursor, 3);
  EXPECT_EQ(strncmp(read.first, "123", 3), 0);

  reserved = queue.reserve(3);
  EXPECT_EQ(reserved, 3);
  memcpy(queue[reserved], "456", 3);
  queue.publish(reserved, 3);
  read = queue.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(read_cursor, 6);
  EXPECT_EQ(strncmp(read.first, "456", 3), 0);

  reserved = queue.reserve(3);
  EXPECT_EQ(reserved, 8);
  memcpy(queue[reserved], "789", 3);

  // read before publish, the read_cursor should changed to new location
  read = queue.read(read_cursor);
  EXPECT_EQ(read_cursor, 8);
  EXPECT_EQ(read.first, nullptr);
  EXPECT_EQ(read.second, 0);

  queue.publish(reserved, 3);
  read = queue.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(read_cursor, 11);
  EXPECT_EQ(strncmp(read.first, "789", 3), 0);
}

TEST(SlickQueueTests, ReadLastUsesLatestReserveSize) {
  slick::queue<int> queue(8);

  auto first = queue.reserve(2);
  *queue[first] = 1;
  *queue[first + 1] = 2;
  queue.publish(first, 2);

  auto last = queue.reserve(1);
  *queue[last] = 3;
  queue.publish(last, 1);

  auto [latest, size] = queue.read_last();
  ASSERT_NE(latest, nullptr);
  EXPECT_EQ(*latest, 3);
  EXPECT_EQ(size, 1);
}

TEST(SlickQueueTests, ReadLastIgnoresUnpublishedReservation) {
  slick::queue<int> queue(8);

  auto first = queue.reserve(2);
  *queue[first] = 1;
  *queue[first + 1] = 2;
  queue.publish(first, 2);

  auto last = queue.reserve(1);
  *queue[last] = 3;

  auto [latest, size] = queue.read_last();
  ASSERT_NE(latest, nullptr);
  EXPECT_EQ(*latest, 1);
  EXPECT_EQ(size, 2);
}

TEST(SlickQueueTests, ReadLastUsesLatestReserveSizeMultiple) {
  slick::queue<char> queue(256);

  const char* first_str = "One";
  uint32_t length = static_cast<uint32_t>(std::strlen(first_str) + 1);
  auto first = queue.reserve(length);
  std::strcpy(queue[first], first_str);
  queue.publish(first, length);

  const char* last_str = "Four";
  length = static_cast<uint32_t>(strlen(first_str) + 1);
  auto last = queue.reserve(length);
  std::strcpy(queue[last], last_str);
  queue.publish(last, length);

  auto [latest, size] = queue.read_last();
  ASSERT_NE(latest, nullptr);
  std::string s(latest, size);
  EXPECT_EQ(strncmp(latest, last_str, size), 0);
}

TEST(SlickQueueTests, ReadLastIgnoresUnpublishedReservationMultiple) {
  slick::queue<char> queue(256);

  const char* first_str = "One";
  uint32_t length = static_cast<uint32_t>(std::strlen(first_str) + 1);
  auto first = queue.reserve(length);
  std::strcpy(queue[first], first_str);
  queue.publish(first, length);

  const char* last_str = "Four";
  length = static_cast<uint32_t>(strlen(first_str) + 1);
  auto last = queue.reserve(length);
  std::strcpy(queue[last], last_str);

  auto [latest, size] = queue.read_last();
  ASSERT_NE(latest, nullptr);
  std::string s(latest, size);
  EXPECT_EQ(strncmp(latest, first_str, size), 0);
}

TEST(SlickQueueTests, LossyOverwriteSkipsOldData) {
  slick::queue<int> queue(2);
  uint64_t read_cursor = 0;

  auto s0 = queue.reserve();
  *queue[s0] = 10;
  queue.publish(s0);

  auto s1 = queue.reserve();
  *queue[s1] = 20;
  queue.publish(s1);

  auto s2 = queue.reserve();
  *queue[s2] = 30;
  queue.publish(s2);

  auto read = queue.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(*read.first, 30);
  EXPECT_EQ(read_cursor, 3);

  read = queue.read(read_cursor);
  EXPECT_EQ(read.first, nullptr);
}

TEST(SlickQueueTests, LossDetectionCountsOverrun) {
  // loss_traits pins the counter on, so this runs in Release as well as Debug.
  slick::queue<int, loss_traits> queue(4);
  for (int i = 0; i < 8; ++i) {
    auto slot = queue.reserve();
    *queue[slot] = i;
    queue.publish(slot);
  }

  uint64_t read_cursor = 0;
  auto read = queue.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(*read.first, 4);
  EXPECT_EQ(queue.loss_count(), 4u);
}

TEST(SlickQueueTests, LossDetectionDisabledReportsZero) {
  // The default traits in a Release build, and any traits with the counter off,
  // must still answer loss_count() - with 0 rather than a compile error.
  slick::queue<int, slick::queue_traits> queue(4);
  for (int i = 0; i < 8; ++i) {
    auto slot = queue.reserve();
    *queue[slot] = i;
    queue.publish(slot);
  }

  uint64_t read_cursor = 0;
  auto read = queue.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(queue.loss_count(), 0u);
}

TEST(SlickQueueTests, AtomicCursorWorkStealing) {
  slick::queue<int> queue(1024);
  std::atomic<uint64_t> shared_cursor{0};
  std::atomic<int> total_consumed{0};

  // Producer: publish 200 items
  std::thread producer([&]() {
    for (int i = 0; i < 200; ++i) {
      auto slot = queue.reserve();
      *queue[slot] = i;
      queue.publish(slot);
    }
  });

  // Multiple consumers sharing atomic cursor
  auto consumer = [&]() {
    int local_count = 0;
    while (total_consumed.load() < 200) {
      auto result = queue.read(shared_cursor);
      if (result.first != nullptr) {
        local_count++;
        total_consumed.fetch_add(1);
      }
    }
    return local_count;
  };

  std::thread c1(consumer);
  std::thread c2(consumer);
  std::thread c3(consumer);

  producer.join();
  c1.join();
  c2.join();
  c3.join();

  // Verify all 200 items were consumed exactly once
  EXPECT_EQ(total_consumed.load(), 200);
  EXPECT_EQ(shared_cursor.load(), 200);
}

TEST(SlickQueueTests, WrappingProducerKeepsCursorAndSizeConsistent) {
  // read() used to load slot.size twice without validating it - once to advance the
  // cursor and once to build the returned pair. A producer recycling the slot between
  // those loads could therefore advance the cursor by one record's size while handing
  // the caller another's, leaving the cursor pointing into the middle of a record.
  //
  // The invariant that pins it: read() only returns a record living in the slot the
  // cursor selected, so the record's index - which is (cursor after the call) minus the
  // size returned - must map back to that same slot. The publish sizes cycle 1,1,2 and
  // sum to 4, which divides the 16-slot queue exactly, so reserve() never takes its
  // mid-buffer wrap path and read() never skips the cursor forward inside the call;
  // any slot mismatch is then the bug and nothing else.
  //
  // It is a probabilistic guard, not a deterministic reproduction: it needs the producer
  // to recycle the slot inside the window between the reader's two loads. What makes it
  // worth keeping is that the invariant is exact, so a future regression fails here
  // rather than being explained away as loss.
  constexpr uint32_t kSize = 16;
  constexpr uint64_t kMask = kSize - 1;
  constexpr uint32_t kPattern[] = {1, 1, 2};

  // loss_traits pins loss detection on in every build, so the run can prove it really
  // lapped the reader instead of quietly keeping up and testing nothing.
  slick::queue<int, loss_traits> queue(kSize);
  std::atomic<bool> stop{false};

  std::thread producer([&]() {
    for (uint64_t i = 0; !stop.load(std::memory_order_relaxed); ++i) {
      uint32_t n = kPattern[i % 3];
      auto index = queue.reserve(n);
      for (uint32_t k = 0; k < n; ++k) {
        *queue[index + k] = static_cast<int>(n);
      }
      queue.publish(index, n);
    }
  });

  uint64_t cursor = 0;
  int reads = 0;
  std::string failure;
  for (int spins = 0; spins < 2000000 && reads < 200000 && failure.empty(); ++spins) {
    const uint64_t before = cursor;
    auto [data, n] = queue.read(cursor);
    if (data == nullptr) {
      continue;
    }
    ++reads;
    if (n != 1 && n != 2) {
      failure = "returned size " + std::to_string(n) + " was never published";
    } else if (cursor < before + n) {
      failure = "cursor " + std::to_string(cursor) + " did not advance past " +
                std::to_string(before) + " by size " + std::to_string(n);
    } else if (((cursor - n) & kMask) != (before & kMask)) {
      failure = "cursor advanced by a size other than the " + std::to_string(n) +
                " returned: record index " + std::to_string(cursor - n) +
                " is not in the slot cursor " + std::to_string(before) + " selected";
    }
  }

  // Stop the producer before asserting - a failed ASSERT_* would return with the thread
  // still running.
  stop.store(true, std::memory_order_relaxed);
  producer.join();

  EXPECT_TRUE(failure.empty()) << failure;
  EXPECT_GT(reads, 0);
  EXPECT_GT(queue.loss_count(), 0u)
      << "the producer never lapped the reader, so no slot was recycled mid-read and the "
         "invariant above was never actually stressed";
}

TEST(SlickQueueTests, ReadLastAfterWrap) {
  // Regression: read_last() previously indexed the control array with the raw
  // absolute publish index, reading out of bounds once the published index
  // exceeded the queue capacity and returning garbage. The index is now masked.
  slick::queue<int> queue(4);
  for (int i = 0; i < 10; ++i) {
    auto slot = queue.reserve();
    *queue[slot] = i;
    queue.publish(slot);
  }
  auto [latest, size] = queue.read_last();
  ASSERT_NE(latest, nullptr);
  EXPECT_EQ(size, 1u);
  EXPECT_EQ(*latest, 9);
}

TEST(SlickQueueTests, ResetRestartsQueue) {
  slick::queue<int> queue(4);
  for (int i = 0; i < 10; ++i) {
    auto slot = queue.reserve();
    *queue[slot] = i;
    queue.publish(slot);
  }
  ASSERT_NE(queue.read_last().first, nullptr);

  queue.reset();

  // Everything published before the reset is gone.
  EXPECT_EQ(queue.read_last().first, nullptr);
  uint64_t cursor = 0;
  EXPECT_EQ(queue.read(cursor).first, nullptr);
  EXPECT_EQ(cursor, 0u);
  EXPECT_EQ(queue.initial_reading_index(), 0u);

  // The queue is usable again and numbering restarts at 0.
  auto slot = queue.reserve();
  EXPECT_EQ(slot, 0u);
  *queue[slot] = 42;
  queue.publish(slot);

  auto read = queue.read(cursor);
  ASSERT_NE(read.first, nullptr);
  EXPECT_EQ(*read.first, 42);
  EXPECT_EQ(cursor, 1u);

  auto [latest, size] = queue.read_last();
  ASSERT_NE(latest, nullptr);
  EXPECT_EQ(size, 1u);
  EXPECT_EQ(*latest, 42);
}

TEST(SlickQueueTests, ResetClearsLossCount) {
  slick::queue<int, loss_traits> queue(4);
  for (int i = 0; i < 10; ++i) {
    auto slot = queue.reserve();
    *queue[slot] = i;
    queue.publish(slot);
  }

  uint64_t cursor = 0;
  while (queue.read(cursor).first != nullptr) {
  }
  ASSERT_GT(queue.loss_count(), 0u);

  queue.reset();
  EXPECT_EQ(queue.loss_count(), 0u);
}

TEST(SlickQueueTests, ReadWithStaleCursorAfterReset) {
  // reset_check_traits compiles in the opt-in reset-detection branch, which is then
  // evaluated on every read. A reader that kept its pre-reset cursor must not hang and
  // must never hand back a record from the previous generation.
  slick::queue<int, reset_check_traits> queue(4);
  for (int i = 0; i < 10; ++i) {
    auto slot = queue.reserve();
    *queue[slot] = i;
    queue.publish(slot);
  }

  uint64_t stale_cursor = 0;
  while (queue.read(stale_cursor).first != nullptr) {
  }
  ASSERT_GT(stale_cursor, 4u);

  queue.reset();
  for (int i = 100; i < 103; ++i) {
    auto slot = queue.reserve();
    *queue[slot] = i;
    queue.publish(slot);
  }

  // The stale reader must rewind and pick up the new generation from its start - not
  // return nullptr while silently keeping a cursor that will skip past it.
  auto stale_read = queue.read(stale_cursor);
  ASSERT_NE(stale_read.first, nullptr);
  EXPECT_EQ(*stale_read.first, 100);
  EXPECT_EQ(stale_cursor, 1u);

  // ...and it keeps reading the rest of the new generation in order.
  stale_read = queue.read(stale_cursor);
  ASSERT_NE(stale_read.first, nullptr);
  EXPECT_EQ(*stale_read.first, 101);

  // A reader starting fresh sees the same thing.
  uint64_t cursor = 0;
  auto read = queue.read(cursor);
  ASSERT_NE(read.first, nullptr);
  EXPECT_EQ(*read.first, 100);
}

TEST(SlickQueueTests, StaleCursorRewindsOnEmptyQueueAfterReset) {
  // The case the old slot-based predicate could never catch: after reset() the control
  // array holds only kInvalidIndex, so there is no slot whose index is ahead of the
  // reservation counter. The cursor must still be rewound.
  slick::queue<int, reset_check_traits> queue(4);
  for (int i = 0; i < 10; ++i) {
    auto slot = queue.reserve();
    *queue[slot] = i;
    queue.publish(slot);
  }

  uint64_t stale_cursor = 0;
  while (queue.read(stale_cursor).first != nullptr) {
  }
  ASSERT_GT(stale_cursor, 4u);

  queue.reset();

  // Nothing republished yet: no data to hand back, but the cursor must be rewound so the
  // reader does not skip the beginning of the next generation.
  auto read = queue.read(stale_cursor);
  EXPECT_EQ(read.first, nullptr);
  EXPECT_EQ(stale_cursor, 0u);

  auto slot = queue.reserve();
  *queue[slot] = 100;
  queue.publish(slot);

  read = queue.read(stale_cursor);
  ASSERT_NE(read.first, nullptr);
  EXPECT_EQ(*read.first, 100);
  EXPECT_EQ(stale_cursor, 1u);
}

TEST(SlickQueueTests, StaleCursorIsNotRewoundWithoutResetCheck) {
  // Documents what opting out costs: with enable_reset_check false the cursor is left
  // alone, which is why the feature exists.
  slick::queue<int, slick::queue_traits> queue(4);
  static_assert(!slick::queue_traits::enable_reset_check);
  for (int i = 0; i < 10; ++i) {
    auto slot = queue.reserve();
    *queue[slot] = i;
    queue.publish(slot);
  }

  uint64_t stale_cursor = 0;
  while (queue.read(stale_cursor).first != nullptr) {
  }
  const uint64_t before = stale_cursor;
  ASSERT_GT(before, 4u);

  queue.reset();
  auto slot = queue.reserve();
  *queue[slot] = 100;
  queue.publish(slot);

  EXPECT_EQ(queue.read(stale_cursor).first, nullptr);
  EXPECT_EQ(stale_cursor, before);
}

TEST(SlickQueueTests, AtomicCursorReadAfterReset) {
  slick::queue<int, reset_check_traits> queue(4);
  for (int i = 0; i < 10; ++i) {
    auto slot = queue.reserve();
    *queue[slot] = i;
    queue.publish(slot);
  }

  std::atomic<uint64_t> stale_cursor{0};
  while (queue.read(stale_cursor).first != nullptr) {
  }
  ASSERT_GT(stale_cursor.load(), 4u);

  queue.reset();
  for (int i = 100; i < 103; ++i) {
    auto slot = queue.reserve();
    *queue[slot] = i;
    queue.publish(slot);
  }

  // Same requirement on the shared-cursor overload: rewind, then deliver the new
  // generation from its start.
  auto stale_read = queue.read(stale_cursor);
  ASSERT_NE(stale_read.first, nullptr);
  EXPECT_EQ(*stale_read.first, 100);
  EXPECT_EQ(stale_cursor.load(), 1u);

  std::atomic<uint64_t> cursor{0};
  auto read = queue.read(cursor);
  ASSERT_NE(read.first, nullptr);
  EXPECT_EQ(*read.first, 100);
  EXPECT_EQ(cursor.load(), 1u);
}

// ---------------------------------------------------------------------------
// Traits configuration
// ---------------------------------------------------------------------------

// The optional counters must occupy the cacheline the old raw members occupied when their
// feature is on, and take no space when it is off. The absolute sizeof is not portable -
// the trailing slick::shm::shared_memory and std::string members differ by platform and
// standard library - so the assertions below check the deltas between configurations,
// which is the property the traits refactor had to preserve on every ABI.
struct all_off_traits : slick::queue_traits {
  static constexpr bool enable_read_last = false;
  static constexpr bool enable_loss_detection = false;
};
struct all_on_traits : slick::queue_traits {
  static constexpr bool enable_read_last = true;
  static constexpr bool enable_loss_detection = true;
};
// A disabled counter is an empty type held by [[no_unique_address]], so it must add
// nothing; an enabled one is cacheline-aligned, so it must add exactly one cacheline.
static_assert(std::is_empty_v<slick::detail::last_published_storage<false>>,
              "optional counters no longer collapse to zero size");
static_assert(std::is_empty_v<slick::detail::loss_counter_storage<false>>,
              "optional counters no longer collapse to zero size");
static_assert(sizeof(slick::queue<int, slick::queue_traits>) ==
              sizeof(slick::queue<int, all_off_traits>) + slick::detail::cacheline_size,
              "object layout changed: read_last no longer costs exactly one cacheline");
static_assert(sizeof(slick::queue<int, all_on_traits>) ==
              sizeof(slick::queue<int, slick::queue_traits>) + slick::detail::cacheline_size,
              "object layout changed: loss detection no longer costs exactly one cacheline");
static_assert(sizeof(slick::queue<int, all_on_traits>) ==
              sizeof(slick::queue<int, slick::debug_queue_traits>),
              "debug_queue_traits should match the all-features-on layout");

TEST(SlickQueueTests, MixedTraitsCoexist) {
  // The capability a global macro could not provide: two differently configured queues
  // alive at once, each behaving per its own traits.
  slick::queue<int, loss_traits> counting(4);
  slick::queue<int, all_off_traits> lean(4);

  for (int i = 0; i < 8; ++i) {
    auto a = counting.reserve();
    *counting[a] = i;
    counting.publish(a);

    auto b = lean.reserve();
    *lean[b] = i;
    lean.publish(b);
  }

  uint64_t c1 = 0;
  uint64_t c2 = 0;
  ASSERT_NE(counting.read(c1).first, nullptr);
  ASSERT_NE(lean.read(c2).first, nullptr);

  EXPECT_EQ(counting.loss_count(), 4u);
  EXPECT_EQ(lean.loss_count(), 0u);  // feature off, always reports 0

  // Both moved through the data identically; only the instrumentation differs.
  EXPECT_EQ(c1, c2);
}

TEST(SlickQueueTests, DefaultTraitsMatchBuildConfiguration) {
  // slick::queue<T> keeps its previous behaviour: the counter is on in Debug and off in
  // Release, exactly as the NDEBUG-keyed macro did.
#ifdef NDEBUG
  static_assert(std::is_same_v<slick::default_queue_traits, slick::queue_traits>);
  EXPECT_FALSE(slick::default_queue_traits::enable_loss_detection);
#else
  static_assert(std::is_same_v<slick::default_queue_traits, slick::debug_queue_traits>);
  EXPECT_TRUE(slick::default_queue_traits::enable_loss_detection);
#endif
  // The other three defaults do not vary by build.
  EXPECT_TRUE(slick::default_queue_traits::enable_read_last);
  EXPECT_FALSE(slick::default_queue_traits::enable_reset_check);
  EXPECT_TRUE(slick::default_queue_traits::enable_cpu_relax);
}
