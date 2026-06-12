#include <gtest/gtest.h>
#include <slick/queue.h>
#include <thread>
#include <cstring>

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

#if SLICK_QUEUE_ENABLE_LOSS_DETECTION
TEST(SlickQueueTests, LossDetectionCountsOverrun) {
  slick::queue<int> queue(4);
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
#endif

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
