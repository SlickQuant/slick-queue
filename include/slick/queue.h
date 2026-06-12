/********************************************************************************
 * Copyright (c) 2020-2026 Slick Quant LLC
 * All rights reserved
 *
 * This file is part of the SlickQueue. Redistribution and use in source and
 * binary forms, with or without modification, are permitted exclusively under
 * the terms of the MIT license which is available at
 * https://github.com/SlickQuant/slick-queue/blob/main/LICENSE
 *
 ********************************************************************************/

#pragma once

// Prevent Windows min/max macros from conflicting with std::numeric_limits
// This must be defined BEFORE any Windows headers (including those from slick-shm)
#if defined(_MSC_VER) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <slick/shm/shared_memory.hpp>

// Undef Windows min/max macros that slick-shm may have pulled in
#if defined(_WIN32) || defined(_MSC_VER)
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#endif

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <stdexcept>
#include <string>
#include <cassert>
#include <thread>
#include <chrono>
#include <limits>
#include <new>

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <immintrin.h>
#endif

#ifndef SLICK_QUEUE_ENABLE_LOSS_DETECTION
#if !defined(NDEBUG)
#define SLICK_QUEUE_ENABLE_LOSS_DETECTION 1
#else
#define SLICK_QUEUE_ENABLE_LOSS_DETECTION 0
#endif
#endif

#ifndef SLICK_QUEUE_ENABLE_CPU_RELAX
#define SLICK_QUEUE_ENABLE_CPU_RELAX 1
#endif

namespace slick {

/**
 * @brief A lock-free multi-producer multi-consumer queue with optional shared memory support.
 * 
 * This queue allows a multiple producer thread to write data and a multiple consumer thread to read data concurrently without locks.
 * It can optionally use shared memory for inter-process communication.
 * This queue is lossy: if producers outrun consumers, older data may be overwritten.
 * 
 * @tparam T The type of elements stored in the queue.
 */
template<typename T>
class SlickQueue {
    static constexpr uint64_t kInvalidIndex = std::numeric_limits<uint64_t>::max();

    struct slot {
        std::atomic_uint_fast64_t data_index{ kInvalidIndex };
        uint32_t size = 1;
    };

    using reserved_info = uint64_t;

#if defined(__cpp_lib_hardware_interference_size)
    static constexpr std::size_t cacheline_size = std::hardware_destructive_interference_size;
#else
    static constexpr std::size_t cacheline_size = 64;
#endif

    uint32_t size_;
    uint32_t mask_;
    T* data_ = nullptr;
    slot* control_ = nullptr;
    std::atomic<reserved_info>* reserved_ = nullptr;
    std::atomic<uint64_t>* last_published_ = nullptr;
    alignas(cacheline_size) std::atomic<reserved_info> reserved_local_{0};
    alignas(cacheline_size) std::atomic<uint64_t> last_published_local_{kInvalidIndex};
#if SLICK_QUEUE_ENABLE_LOSS_DETECTION
    alignas(cacheline_size) std::atomic<uint64_t> loss_count_{0};
#endif
    bool own_ = false;
    bool use_shm_ = false;
    bool last_published_valid_ = false;
    slick::shm::shared_memory shm_;  // RAII wrapper for shared memory
    void* lpvMem_ = nullptr;          // Cached data pointer
    std::string shm_name_;            // Stored for cleanup

    // Shared memory layout constants
    //
    // The shared memory segment is organized as follows:
    //
    // [HEADER: 64 bytes]
    //   Offset 0-7   (8 bytes):  std::atomic<reserved_info> - reservation cursor
    //   Offset 8-11  (4 bytes):  size_ - queue capacity (uint32_t)
    //   Offset 12-15 (4 bytes):  element_size - sizeof(T) for validation (uint32_t)
    //   Offset 16-23 (8 bytes):  std::atomic<uint64_t> - last published index
    //   Offset 24-27 (4 bytes):  header_magic - layout/version marker
    //   Offset 28-47 (20 bytes): PADDING - reserved for future use
    //   Offset 48-51 (4 bytes):  init_state - atomic init state (0=uninit,1=legacy,2=init,3=ready)
    //   Offset 52-63 (12 bytes): PADDING - reserved for future use
    //
    // [CONTROL ARRAY: sizeof(slot) * size_]
    //   Array of slot structures containing atomic indices and sizes
    //
    // [DATA ARRAY: sizeof(T) * size_]
    //   Array of queue elements
    //
    static constexpr uint32_t HEADER_SIZE = 64;
    static constexpr uint32_t LAST_PUBLISHED_OFFSET = 16;
    static constexpr uint32_t HEADER_MAGIC_OFFSET = 24;
    static constexpr uint32_t INIT_STATE_OFFSET = 48;  // Offset in header for atomic init state
    static constexpr uint32_t HEADER_MAGIC = 0x534C5131; // 'SLQ1'
    static constexpr uint32_t INIT_STATE_UNINITIALIZED = 0;
    static constexpr uint32_t INIT_STATE_LEGACY = 1;
    static constexpr uint32_t INIT_STATE_INITIALIZING = 2;
    static constexpr uint32_t INIT_STATE_READY = 3;

    static constexpr bool is_power_of_two(uint32_t value) noexcept {
        return value != 0 && ((value & (value - 1)) == 0);
    }

public:
    /**
     * @brief Construct a new SlickQueue object
     * 
     * @param size The size of the queue, must be a power of 2.
     * @param shm_name The name of the shared memory segment. If nullptr, the queue will use local memory.
     * 
     * @throws std::runtime_error if shared memory allocation fails.
     * @throws std::invalid_argument if size is not a power of 2.
     */
    SlickQueue(uint32_t size, const char* const shm_name = nullptr)
        : size_(size)
        , mask_(size ? size - 1 : 0)
        , own_(shm_name == nullptr)
        , use_shm_(shm_name != nullptr)
    {
        if (!is_power_of_two(size_)) {
            throw std::invalid_argument("size must power of 2");
        }
        if (shm_name) {
            allocate_shm_data(shm_name, false);
        } else {
            reserved_ = &reserved_local_;
            reserved_->store(0, std::memory_order_relaxed);
            last_published_ = &last_published_local_;
            last_published_->store(kInvalidIndex, std::memory_order_relaxed);
            last_published_valid_ = true;
            data_ = new T[size_];
            control_ = new slot[size_];
        }
    }

    /**
     * @brief Open an existing SlickQueue in shared memory
     * 
     * @param shm_name The name of the shared memory segment.
     * 
     * @throws std::runtime_error if shared memory allocation fails or the segment does not exist.
     */
    SlickQueue(const char* const shm_name)
        : size_(0)
        , mask_(0)
        , own_(false)
        , use_shm_(true)
    {
        allocate_shm_data(shm_name, true);
    }

    virtual ~SlickQueue() noexcept {
        if (use_shm_) {
            // slick-shm RAII handles unmapping and closing automatically
            // Only need to explicitly remove on POSIX if we're the owner
#if !defined(_MSC_VER)
            if (own_ && shm_.is_valid() && !shm_name_.empty()) {
                slick::shm::shared_memory::remove(shm_name_.c_str());
            }
#endif
            // shm_ destructor unmaps and closes handle automatically
        } else {
            delete[] data_;
            data_ = nullptr;
            delete[] control_;
            control_ = nullptr;
        }
    }

    /**
     * @brief Check if the queue owns the memory buffer
     * @return true if the queue owns the memory buffer, false otherwise
     */
    bool own_buffer() const noexcept { return own_; }

    /**
     * @brief Check if the queue uses shared memory
     * @return true if the queue uses shared memory, false otherwise
     */
    bool use_shm() const noexcept { return use_shm_; }

    /**
     * @brief Get the size of the queue
     * @return Size of the queue
     */
    constexpr uint32_t size() const noexcept { return size_; }

    
    /**
     * @brief Get the number of items skipped due to overwrite (debug-only if enabled).
     * @return Count of skipped items observed by this queue instance.
     */
    uint64_t loss_count() const noexcept {
#if SLICK_QUEUE_ENABLE_LOSS_DETECTION
        return loss_count_.load(std::memory_order_relaxed);
#else
        return 0;
#endif
    }

    /**
     * @brief Get the initial reading index, which is 0 if the queue is newly created or the current writing index if opened existing 
     * @return Initial reading index
     */
    uint64_t initial_reading_index() const noexcept {
        return get_index(reserved_->load(std::memory_order_relaxed));
    }

    /**
     * @brief Reserve space in the queue for writing
     * @param n Number of slots to reserve, default is 1
     * @return The starting index of the reserved space
     */
    uint64_t reserve(uint32_t n = 1) {
        if (n == 0) [[unlikely]] {
            throw std::invalid_argument("required size must be > 0");
        }
        if (n > size_) [[unlikely]] {
            throw std::runtime_error("required size " + std::to_string(n) + " > queue size " + std::to_string(size_));
        }
        if (n == 1) {
            constexpr reserved_info step = (1ULL << 16);
            auto prev = reserved_->fetch_add(step, std::memory_order_release);
            auto index = get_index(prev);
            auto prev_size = get_size(prev);
            if (prev_size != 1) {
                auto expected = make_reserved_info(index + 1, prev_size);
                reserved_->compare_exchange_strong(expected, make_reserved_info(index + 1, 1),
                    std::memory_order_release, std::memory_order_relaxed);
            }
            return index;
        }
        auto reserved = reserved_->load(std::memory_order_relaxed);
        uint64_t next = 0;
        uint64_t index = 0;
        bool buffer_wrapped = false;
        for (;;) {
            buffer_wrapped = false;
            index = get_index(reserved);
            auto idx = index & mask_;
            if ((idx + n) > size_) {
                // if there is no enough buffer left, start from the beginning
                index += size_ - idx;
                next = make_reserved_info(index + n, n);
                buffer_wrapped = true;
            }
            else {
                next = make_reserved_info(index + n, n);
            }
            if (reserved_->compare_exchange_weak(reserved, next, std::memory_order_release, std::memory_order_relaxed)) {
                break;
            }
            cpu_relax();
        }
        if (buffer_wrapped) {
            // queue wrapped, set current slock.data_index to the reserved index to let the reader
            // know the next available data is in different slot.
            auto& slot = control_[get_index(reserved) & mask_];
            slot.size = n;
            slot.data_index.store(index, std::memory_order_release);
        }
        return index;
    }

    /**
     * @brief Access the reserved space for writing
     * @param index The index returned by reserve()
     * @return Pointer to the reserved space
     */
    T* operator[] (uint64_t index) noexcept {
        return &data_[index & mask_];
    }

    /**
     * @brief Access the reserved space for writing (const version)
     * @param index The index returned by reserve()
     * @return Pointer to the reserved space
     */
    const T* operator[] (uint64_t index) const noexcept {
        return &data_[index & mask_];
    }

    /**
     * @brief Publish the data written in the reserved space
     * @param index The index returned by reserve()
     * @param n Number of slots to publish, default is 1
     */
    void publish(uint64_t index, uint32_t n = 1) noexcept {
        assert(n > 0);
        auto& slot = control_[index & mask_];
        slot.size = n;
        slot.data_index.store(index, std::memory_order_release);

        if (last_published_valid_) {
            auto current = last_published_->load(std::memory_order_relaxed);
            while ((current == kInvalidIndex || current < index) &&
                   !last_published_->compare_exchange_weak(
                       current, index, std::memory_order_release, std::memory_order_relaxed)) {
            }
        }
    }

    /**
     * @brief Read data from the queue
     * @param read_index Reference to the reading index, will be updated to the next index after reading
     * @return Pair of pointer to the data and the size of the data, or nullptr and 0 if no data is available
     */
    std::pair<T*, uint32_t> read(uint64_t& read_index) noexcept {
        uint64_t index;
        slot* current_slot;
        while (true) {
            auto idx = read_index & mask_;
            current_slot = &control_[idx];
            index = current_slot->data_index.load(std::memory_order_acquire);
            if (index != std::numeric_limits<uint64_t>::max() && get_index(reserved_->load(std::memory_order_relaxed)) < index) [[unlikely]] {
                // queue has been reset
                read_index = 0;
            }

#if SLICK_QUEUE_ENABLE_LOSS_DETECTION
            if (index != std::numeric_limits<uint64_t>::max() && index > read_index && ((index & mask_) == idx)) {
                loss_count_.fetch_add(index - read_index, std::memory_order_relaxed);
            }
#endif

            if (index == std::numeric_limits<uint64_t>::max() || index < read_index) {
                // data not ready yet
                return std::make_pair(nullptr, 0);
            }
            else if (index > read_index && ((index & mask_) != idx)) {
                // queue wrapped, skip the unused slots
                read_index = index;
                continue;
            }
            break;
        }

        auto& data = data_[read_index & mask_];
        read_index = index + current_slot->size;
        return std::make_pair(&data, current_slot->size);
    }

    /**
     * @brief Read data from the queue using a shared atomic cursor
     * @param read_index Reference to the atomic reading index, will be atomically updated after reading
     * @return Pair of pointer to the data and the size of the data, or nullptr and 0 if no data is available
     *
     * This overload allows multiple consumers to share a single atomic cursor for load-balancing/work-stealing patterns.
     * Each consumer atomically claims the next item to process.
     */
    std::pair<T*, uint32_t> read(std::atomic<uint64_t>& read_index) noexcept {
        while (true) {
            uint64_t current_index = read_index.load(std::memory_order_relaxed);
            auto idx = current_index & mask_;
            slot* current_slot = &control_[idx];
            uint64_t index = current_slot->data_index.load(std::memory_order_acquire);

            if (index != std::numeric_limits<uint64_t>::max() && get_index(reserved_->load(std::memory_order_relaxed)) < index) [[unlikely]] {
                // queue has been reset
                read_index.store(0, std::memory_order_relaxed);
                continue;
            }

            if (index == std::numeric_limits<uint64_t>::max() || index < current_index) {
                // data not ready yet
                return std::make_pair(nullptr, 0);
            }

#if SLICK_QUEUE_ENABLE_LOSS_DETECTION
            uint64_t overrun = 0;
            if (index > current_index && ((index & mask_) == idx)) {
                overrun = index - current_index;
            }
#endif

            if (index > current_index && ((index & mask_) != idx)) {
                // queue wrapped, skip the unused slots
                read_index.compare_exchange_weak(current_index, index, std::memory_order_relaxed, std::memory_order_relaxed);
                continue;
            }

            // Try to atomically claim this item
            uint64_t next_index = index + current_slot->size;
            if (read_index.compare_exchange_weak(current_index, next_index, std::memory_order_relaxed, std::memory_order_relaxed)) {
#if SLICK_QUEUE_ENABLE_LOSS_DETECTION
                if (overrun != 0) {
                    loss_count_.fetch_add(overrun, std::memory_order_relaxed);
                }
#endif
                // Successfully claimed the item
                return std::make_pair(&data_[current_index & mask_], current_slot->size);
            }
            cpu_relax();
            // CAS failed, another consumer claimed it, retry
        }
    }

    /**
    * @brief Read the last published data in the queue
    * @return Pointer to the last published data, or nullptr if no data is available
    */
    std::pair<T*, uint32_t> read_last() noexcept {
        if (last_published_valid_) {
            auto last_index = last_published_->load(std::memory_order_acquire);
            if (last_index == kInvalidIndex) {
                return std::make_pair(nullptr, 0);
            }
            slot &slot = control_[last_index];
            return std::make_pair(&data_[last_index & mask_], slot.size);
        }

        // legacy
        auto reserved = reserved_->load(std::memory_order_relaxed);
        auto index = get_index(reserved);
        if (index == 0) {
            return std::make_pair(nullptr, 0);
        }
        auto sz = get_size(reserved);
        auto last_index = index - sz;
        return std::make_pair(&data_[last_index & mask_], sz);
    }

    /**
     * @brief Reset the queue, invalidating all existing data
     * 
     * Note: This function is not thread-safe and should be called when no other threads are accessing the queue.
     */
    void reset() noexcept {
        if (use_shm_) {
            control_ = new ((uint8_t*)lpvMem_ + HEADER_SIZE) slot[size_];
        } else {
            delete [] control_;
            control_ = new slot[size_];
        }
        reserved_->store(0, std::memory_order_release);
        last_published_->store(kInvalidIndex, std::memory_order_relaxed);
#if SLICK_QUEUE_ENABLE_LOSS_DETECTION
        loss_count_.store(0, std::memory_order_relaxed);
#endif
    }

private:
    // Helper functions for packing/unpacking reserved_info (16-bit size, 48-bit index)
    static constexpr uint64_t make_reserved_info(uint64_t index, uint32_t size) noexcept {
        return ((index & 0xFFFFFFFFFFFFULL) << 16) | (size & 0xFFFF);
    }

    static constexpr uint64_t get_index(uint64_t reserved) noexcept {
        return reserved >> 16;
    }

    static constexpr uint32_t get_size(uint64_t reserved) noexcept {
        return static_cast<uint32_t>(reserved & 0xFFFF);
    }

    static inline void cpu_relax() noexcept {
#if SLICK_QUEUE_ENABLE_CPU_RELAX
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
        _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield" ::: "memory");
#else
        std::this_thread::yield();
#endif
#else
        (void)0;
#endif
    }


    bool wait_for_shared_memory_ready(uint8_t* base, std::atomic<uint32_t>* init_state) const noexcept {
        constexpr int kMaxWaitMs = 2000;
        constexpr int kLegacyGraceMs = 5;

        for (int i = 0; i < kMaxWaitMs; ++i) {
            uint32_t state = init_state->load(std::memory_order_acquire);
            if (state == INIT_STATE_READY) {
                return true;
            }

            if (state == INIT_STATE_LEGACY && i >= kLegacyGraceMs) {
                uint32_t size = *reinterpret_cast<uint32_t*>(
                    base + sizeof(std::atomic<reserved_info>));
                uint32_t element_size = *reinterpret_cast<uint32_t*>(
                    base + sizeof(std::atomic<reserved_info>) + sizeof(uint32_t));
                if (size != 0 && element_size != 0) {
                    return true;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return false;
    }



    void allocate_shm_data(const char* const shm_name, bool open_only) {
        shm_name_ = shm_name;  // Store for destructor cleanup

        if (open_only) {
            // Opener constructor - open existing only
            try {
                shm_ = slick::shm::shared_memory(
                    shm_name,
                    slick::shm::open_existing,
                    slick::shm::access_mode::read_write
                );
            } catch (const slick::shm::shared_memory_error& e) {
                throw std::runtime_error(std::string("Failed to open shared memory: ") + e.what());
            }

            lpvMem_ = shm_.data();
            if (!lpvMem_) {
                throw std::runtime_error("Failed to map shared memory");
            }

            auto* base = reinterpret_cast<uint8_t*>(lpvMem_);
            auto* init_state = reinterpret_cast<std::atomic<uint32_t>*>(base + INIT_STATE_OFFSET);
            if (!wait_for_shared_memory_ready(base, init_state)) {
                throw std::runtime_error("Timed out waiting for shared memory initialization");
            }

            uint32_t state = init_state->load(std::memory_order_acquire);
            last_published_valid_ = false;
            if (state == INIT_STATE_READY) {
                auto* header_magic = reinterpret_cast<std::atomic<uint32_t>*>(base + HEADER_MAGIC_OFFSET);
                uint32_t magic = header_magic->load(std::memory_order_acquire);
                last_published_valid_ = (magic == HEADER_MAGIC);
            }
            last_published_ = reinterpret_cast<std::atomic<uint64_t>*>(base + LAST_PUBLISHED_OFFSET);

            // Read size from header
            size_ = *reinterpret_cast<uint32_t*>(
                base + sizeof(std::atomic<reserved_info>));
            uint32_t element_size = *reinterpret_cast<uint32_t*>(
                base + sizeof(std::atomic<reserved_info>) + sizeof(uint32_t));

            // Validate
            if (!is_power_of_two(size_)) {
                throw std::runtime_error("Shared memory size must be power of 2. Got " + std::to_string(size_));
            }
            if (element_size != sizeof(T)) {
                throw std::runtime_error("Shared memory element size mismatch. Expected " +
                    std::to_string(sizeof(T)) + " but got " + std::to_string(element_size));
            }

            mask_ = size_ - 1;

            // Map to existing structures
            reserved_ = reinterpret_cast<std::atomic<reserved_info>*>(base);
            control_ = reinterpret_cast<slot*>(base + HEADER_SIZE);
            data_ = reinterpret_cast<T*>(base + HEADER_SIZE + sizeof(slot) * size_);

        } else {
            // Creator constructor - create or open
            const size_t total_size = HEADER_SIZE + sizeof(slot) * size_ + sizeof(T) * size_;

            try {
                shm_ = slick::shm::shared_memory(
                    shm_name,
                    total_size,
                    slick::shm::open_or_create,
                    slick::shm::access_mode::read_write
                );
            } catch (const slick::shm::shared_memory_error& e) {
                throw std::runtime_error(std::string("Failed to create/open shared memory: ") + e.what());
            }

            lpvMem_ = shm_.data();
            if (!lpvMem_) {
                throw std::runtime_error("Failed to map shared memory");
            }

            auto* base = reinterpret_cast<uint8_t*>(lpvMem_);
            auto* init_state = reinterpret_cast<std::atomic<uint32_t>*>(base + INIT_STATE_OFFSET);

            uint32_t expected = INIT_STATE_UNINITIALIZED;
            bool we_are_creator = init_state->compare_exchange_strong(
                expected, INIT_STATE_INITIALIZING, std::memory_order_acq_rel);

            if (we_are_creator) {
                // Initialize as creator
                own_ = true;

                auto* header_magic = new (base + HEADER_MAGIC_OFFSET) std::atomic<uint32_t>();
                header_magic->store(HEADER_MAGIC, std::memory_order_release);

                // Initialize atomic header
                reserved_ = new (base) std::atomic<reserved_info>();
                reserved_->store(0, std::memory_order_relaxed);

                last_published_ = new (base + LAST_PUBLISHED_OFFSET) std::atomic<uint64_t>();
                last_published_->store(kInvalidIndex, std::memory_order_relaxed);
                last_published_valid_ = true;

                // Write metadata
                *reinterpret_cast<uint32_t*>(
                    base + sizeof(std::atomic<reserved_info>)) = size_;
                *reinterpret_cast<uint32_t*>(
                    base + sizeof(std::atomic<reserved_info>) + sizeof(uint32_t)) = sizeof(T);

                // Placement-new arrays
                control_ = new (base + HEADER_SIZE) slot[size_];
                data_ = new (base + HEADER_SIZE + sizeof(slot) * size_) T[size_];

                init_state->store(INIT_STATE_READY, std::memory_order_release);

            } else {
                // Opened existing - validate
                own_ = false;

                if (!wait_for_shared_memory_ready(base, init_state)) {
                    throw std::runtime_error("Timed out waiting for shared memory initialization");
                }

                uint32_t state = init_state->load(std::memory_order_acquire);
                last_published_valid_ = false;
                if (state == INIT_STATE_READY) {
                    auto* header_magic = reinterpret_cast<std::atomic<uint32_t>*>(base + HEADER_MAGIC_OFFSET);
                    uint32_t magic = header_magic->load(std::memory_order_acquire);
                    last_published_valid_ = (magic == HEADER_MAGIC);
                }
                last_published_ = reinterpret_cast<std::atomic<uint64_t>*>(base + LAST_PUBLISHED_OFFSET);

                // Read and validate metadata
                uint32_t shm_size = *reinterpret_cast<uint32_t*>(
                    base + sizeof(std::atomic<reserved_info>));
                uint32_t element_size = *reinterpret_cast<uint32_t*>(
                    base + sizeof(std::atomic<reserved_info>) + sizeof(uint32_t));

                if (shm_size != size_) {
                    throw std::runtime_error("Shared memory size mismatch. Expected " +
                        std::to_string(size_) + " but got " + std::to_string(shm_size));
                }
                if (element_size != sizeof(T)) {
                    throw std::runtime_error("Shared memory element size mismatch. Expected " +
                        std::to_string(sizeof(T)) + " but got " + std::to_string(element_size));
                }

                // Map to existing structures
                reserved_ = reinterpret_cast<std::atomic<reserved_info>*>(base);
                control_ = reinterpret_cast<slot*>(base + HEADER_SIZE);
                data_ = reinterpret_cast<T*>(base + HEADER_SIZE + sizeof(slot) * size_);
            }
        }
    }
};

/// Preferred snake_case spelling of SlickQueue, matching the slick::queue
/// CMake target and the Boost-style naming of companion types such as slick::dynamic_buffer.
/// SlickQueue remains available for backward compatibility.
template<typename T>
using queue = SlickQueue<T>;

}
