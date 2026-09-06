#include <gtest/gtest.h>
#include <slick/queue.hpp>
#include <stdexcept>
#include <string>
#include <thread>

#include "test_traits.h"

namespace {
/// Returns the message of the std::runtime_error `attach` is expected to throw, so a
/// rejection test can pin *why* the attach failed - every other attach-time check
/// (readiness, size, element size) throws the same type.
template <typename Fn>
std::string attach_failure_message(Fn&& attach) {
  try {
    attach();
  } catch (const std::runtime_error& e) {
    return e.what();
  }
  ADD_FAILURE() << "expected attaching to throw";
  return {};
}

/// The feature nibble is the whole point of the check, so match on it rather than on the
/// sentence around it.
void expect_read_last_mismatch(const std::string& message, bool segment_tracks) {
  const std::string expected =
      std::string("enable_read_last=") + (segment_tracks ? "true" : "false") +
      " but this queue has enable_read_last=" + (segment_tracks ? "false" : "true");
  EXPECT_NE(message.find(expected), std::string::npos) << "actual: " << message;
}
}  // namespace

TEST(ShmTests, ReadEmptyQueue) {
  slick::queue<int> queue(2, "sq_read_empty");
  uint64_t read_cursor = 0;
  auto read = queue.read(read_cursor);
  EXPECT_EQ(read.first, nullptr);
}

TEST(ShmTests, Reserve) {
  slick::queue<int> queue(2, "sq_reserve");
  auto reserved = queue.reserve();
  EXPECT_EQ(reserved, 0);
  EXPECT_EQ(queue.reserve(), 1);
  EXPECT_EQ(queue.reserve(), 2);
}

TEST(ShmTests, ReadShouldFailWithoutPublish) {
  slick::queue<int> queue(2, "sq_read_fail");
  uint64_t read_cursor = 0;
  auto reserved = queue.reserve();
  auto read = queue.read(read_cursor);
  EXPECT_EQ(read.first, nullptr);
  EXPECT_EQ(read_cursor, 0);
}

TEST(ShmTests, PublishAndRead) {
  slick::queue<int> queue(2, "sq_publish_read");
  uint64_t read_cursor = 0;
  auto reserved = queue.reserve();
  *queue[reserved] = 5;
  queue.publish(reserved);
  auto read = queue.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(read_cursor, 1);
  EXPECT_EQ(*read.first, 5);
}

TEST(ShmTests, PublishAndReadMultiple) {
  slick::queue<int> queue(4, "sq_publish_read_multiple");
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

TEST(ShmTests, ServerClient) {
  slick::queue<int> server(4, "sq_server_cleint");
  slick::queue<int> client("sq_server_cleint");
  EXPECT_EQ(client.size(), 4);

  auto reserved = server.reserve();
  *server[reserved] = 5;
  server.publish(reserved);
  auto reserved1 = server.reserve();
  *server[reserved1] = 12;
  auto reserved2 = server.reserve();
  *server[reserved2] = 23;
  server.publish(reserved2);

  uint64_t read_cursor = 0;
  auto read = client.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(read_cursor, 1);
  EXPECT_EQ(*read.first, 5);

  read = client.read(read_cursor);
  EXPECT_EQ(read.first, nullptr);
  EXPECT_EQ(read_cursor, 1);

  server.publish(reserved1);
  read = client.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(read_cursor, 2);
  EXPECT_EQ(*read.first, 12);

  read = client.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(read_cursor, 3);
  EXPECT_EQ(*read.first, 23);
}

TEST(ShmTests, AtomicCursorWorkStealing) {
  slick::queue<int> server(1024, "sq_atomic_cursor_shm");
  slick::queue<int> client1("sq_atomic_cursor_shm");
  slick::queue<int> client2("sq_atomic_cursor_shm");

  std::atomic<uint64_t> shared_cursor{0};
  std::atomic<int> total_consumed{0};

  // Server: publish 100 items
  std::thread producer([&]() {
    for (int i = 0; i < 100; ++i) {
      auto slot = server.reserve();
      *server[slot] = i;
      server.publish(slot);
    }
  });

  // Multiple clients sharing atomic cursor via shared memory
  auto consumer = [&](slick::queue<int>& client) {
    int local_count = 0;
    while (total_consumed.load() < 100) {
      auto result = client.read(shared_cursor);
      if (result.first != nullptr) {
        local_count++;
        total_consumed.fetch_add(1);
      }
    }
    return local_count;
  };

  std::thread c1([&]() { consumer(client1); });
  std::thread c2([&]() { consumer(client2); });

  producer.join();
  c1.join();
  c2.join();

  // Verify all 100 items were consumed exactly once
  EXPECT_EQ(total_consumed.load(), 100);
  EXPECT_EQ(shared_cursor.load(), 100);
}

TEST(ShmTests, LossyOverwriteSkipsOldData) {
  // loss_traits pins the counter on, so this assertion runs in every build.
  slick::queue<int, loss_traits> server(2, "sq_lossy_overwrite");
  slick::queue<int, loss_traits> client("sq_lossy_overwrite");

  auto s0 = server.reserve();
  *server[s0] = 10;
  server.publish(s0);

  auto s1 = server.reserve();
  *server[s1] = 20;
  server.publish(s1);

  auto s2 = server.reserve();
  *server[s2] = 30;
  server.publish(s2);

  uint64_t read_cursor = 0;
  auto read = client.read(read_cursor);
  EXPECT_NE(read.first, nullptr);
  EXPECT_EQ(*read.first, 30);
  EXPECT_EQ(read_cursor, 3);

  EXPECT_EQ(client.loss_count(), 2u);

  read = client.read(read_cursor);
  EXPECT_EQ(read.first, nullptr);
}

TEST(ShmTests, ElementSizeMismatch) {
  slick::queue<int> server(4, "sq_element_mismatch");
  EXPECT_THROW({
    slick::queue<double> client("sq_element_mismatch");
  }, std::runtime_error);
}

TEST(ShmTests, SizeMismatch) {
  // Create a shared memory queue with size 4
  slick::queue<int> server(4, "sq_size_mismatch");

  // Try to create another queue with same name but different size
  // This should throw an exception
  EXPECT_THROW({
    try {
      slick::queue<int> client(8, "sq_size_mismatch");
    } catch (const std::runtime_error& e) {
      EXPECT_TRUE(std::string(e.what()).find("Shared memory size mismatch") != std::string::npos);
      throw;
    }
  }, std::runtime_error);
}

TEST(ShmTests, ReadLastUsesLatestReserveSize) {
  slick::queue<int> queue(8, "sq_read_last");
  slick::queue<int> reader_queue(8, "sq_read_last");

  auto first = queue.reserve(2);
  *queue[first] = 1;
  *queue[first + 1] = 2;
  queue.publish(first, 2);

  auto last = queue.reserve(1);
  *queue[last] = 3;
  queue.publish(last, 1);

  auto [latest, size] = reader_queue.read_last();
  ASSERT_NE(latest, nullptr);
  EXPECT_EQ(*latest, 3);
  EXPECT_EQ(size, 1);
}

TEST(ShmTests, ReadLastIgnoresUnpublishedReservation) {
  slick::queue<int> queue(8, "sq_read_last2");
  slick::queue<int> reader_queue(8, "sq_read_last2");

  auto first = queue.reserve(2);
  *queue[first] = 1;
  *queue[first + 1] = 2;
  queue.publish(first, 2);

  auto last = queue.reserve(1);
  *queue[last] = 3;

  auto [latest, size] = reader_queue.read_last();
  ASSERT_NE(latest, nullptr);
  EXPECT_EQ(*latest, 1);
  EXPECT_EQ(size, 2);
}

TEST(ShmTests, ReadLastUsesLatestReserveSizeMultiple) {
  slick::queue<char> queue(256, "sq_read_last_multi");
  slick::queue<char> reader_queue(256, "sq_read_last_multi");

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

  auto [latest, size] = reader_queue.read_last();
  ASSERT_NE(latest, nullptr);
  std::string s(latest, size);
  EXPECT_EQ(strncmp(latest, last_str, size), 0);
}

TEST(ShmTests, ReadLastIgnoresUnpublishedReservationMultiple) {
  slick::queue<char> queue(256, "sq_read_last_multi2");
  slick::queue<char> reader_queue(256, "sq_read_last_multi2");

  const char* first_str = "One";
  uint32_t length = static_cast<uint32_t>(std::strlen(first_str) + 1);
  auto first = queue.reserve(length);
  std::strcpy(queue[first], first_str);
  queue.publish(first, length);

  const char* last_str = "Four";
  length = static_cast<uint32_t>(strlen(first_str) + 1);
  auto last = queue.reserve(length);
  std::strcpy(queue[last], last_str);

  auto [latest, size] = reader_queue.read_last();
  ASSERT_NE(latest, nullptr);
  std::string s(latest, size);
  EXPECT_EQ(strncmp(latest, first_str, size), 0);
}

TEST(ShmTests, ReadLastAfterWrap) {
  // Regression: same out-of-bounds read as SlickQueueTests.ReadLastAfterWrap,
  // exercised through a shared-memory segment. The reader uses the attacher
  // constructor, so read_last()'s masking is validated against a mask_ derived
  // from the segment header rather than one supplied locally.
  slick::queue<int> queue(4, "sq_read_last_wrap");
  slick::queue<int> reader_queue("sq_read_last_wrap");
  ASSERT_EQ(reader_queue.size(), 4u);

  for (int i = 0; i < 10; ++i) {
    auto slot = queue.reserve();
    *queue[slot] = i;
    queue.publish(slot);
  }

  auto [latest, size] = reader_queue.read_last();
  ASSERT_NE(latest, nullptr);
  EXPECT_EQ(size, 1u);
  EXPECT_EQ(*latest, 9);
}

TEST(ShmTests, ResetRestartsSegment) {
  // reset_check_traits compiles the opt-in reset-detection path into these reads.
  slick::queue<int, reset_check_traits> queue(4, "sq_reset");
  slick::queue<int, reset_check_traits> reader_queue("sq_reset");

  for (int i = 0; i < 10; ++i) {
    auto slot = queue.reserve();
    *queue[slot] = i;
    queue.publish(slot);
  }
  ASSERT_NE(reader_queue.read_last().first, nullptr);

  queue.reset();

  EXPECT_EQ(queue.read_last().first, nullptr);
  EXPECT_EQ(reader_queue.read_last().first, nullptr);
  EXPECT_EQ(queue.initial_reading_index(), 0u);

  auto slot = queue.reserve();
  EXPECT_EQ(slot, 0u);
  *queue[slot] = 42;
  queue.publish(slot);

  uint64_t cursor = 0;
  auto read = reader_queue.read(cursor);
  ASSERT_NE(read.first, nullptr);
  EXPECT_EQ(*read.first, 42);
  EXPECT_EQ(cursor, 1u);

  auto [latest, size] = reader_queue.read_last();
  ASSERT_NE(latest, nullptr);
  EXPECT_EQ(size, 1u);
  EXPECT_EQ(*latest, 42);
}

TEST(ShmTests, MixedTraitsOnOneSegment) {
  // Traits that differ only in locally-held features (loss detection here) share a
  // segment freely: they leave the same feature nibble in the marker, which is the only
  // cross-process signal of what a peer maintains.
  slick::queue<int> creator(4, "sq_mixed_traits");
  slick::queue<int, loss_traits> attacher("sq_mixed_traits");
  ASSERT_EQ(attacher.size(), 4u);

  auto slot = creator.reserve();
  *creator[slot] = 7;
  creator.publish(slot);

  auto [latest, size] = attacher.read_last();
  ASSERT_NE(latest, nullptr);
  EXPECT_EQ(size, 1u);
  EXPECT_EQ(*latest, 7);
}

TEST(ShmTests, AttachingWithReadLastToSegmentWithoutItThrows) {
  // The creator never maintains the last-published index, so its marker clears the
  // feature bit. An attacher that does read that index would otherwise trust a counter
  // nobody writes; it must be turned away at attach time instead.
  slick::queue<int, no_read_last_traits> creator(4, "sq_feature_mismatch_off");

  expect_read_last_mismatch(
      attach_failure_message([] { slick::queue<int>("sq_feature_mismatch_off"); }), false);
  expect_read_last_mismatch(
      attach_failure_message([] { slick::queue<int>(4, "sq_feature_mismatch_off"); }), false);
}

TEST(ShmTests, AttachingWithoutReadLastToSegmentWithItThrows) {
  // The mirror image, and the case the old validity flag could not catch at all: this
  // attacher's publish() would never advance the index, silently freezing read_last()
  // for the creator and every other peer that does maintain it.
  slick::queue<int> creator(4, "sq_feature_mismatch_on");

  expect_read_last_mismatch(
      attach_failure_message(
          [] { slick::queue<int, no_read_last_traits>("sq_feature_mismatch_on"); }),
      true);
  expect_read_last_mismatch(
      attach_failure_message(
          [] { slick::queue<int, no_read_last_traits>(4, "sq_feature_mismatch_on"); }),
      true);
}

TEST(ShmTests, MatchingReadLastOffTraitsShareSegment) {
  // Both sides agree the index is not maintained, so the markers match and the segment
  // works normally through the cursor-based read path.
  slick::queue<int, no_read_last_traits> creator(4, "sq_no_read_last");
  slick::queue<int, no_read_last_traits> attacher("sq_no_read_last");
  ASSERT_EQ(attacher.size(), 4u);

  auto slot = creator.reserve();
  *creator[slot] = 7;
  creator.publish(slot);

  uint64_t cursor = 0;
  auto read = attacher.read(cursor);
  ASSERT_NE(read.first, nullptr);
  EXPECT_EQ(*read.first, 7);
}
