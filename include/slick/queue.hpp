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
#include <type_traits>
#include <concepts>

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <immintrin.h>
#endif

// The SLICK_QUEUE_ENABLE_* macros were replaced by the Traits template parameter
// (see slick::queue_traits). A macro cannot express per-instantiation configuration,
// and because these changed the bodies - and the layout - of a header-only class
// template, two translation units compiled with different values silently violated
// the ODR. Warn rather than error so upgrading does not break a build that still
// passes -D; the macro itself has no effect any more.
#define SLICK_QUEUE_STR_(x) #x
#define SLICK_QUEUE_STR(x) SLICK_QUEUE_STR_(x)
#if defined(_MSC_VER)
// The __FILE__(__LINE__): prefix makes the warning clickable in the VS problem list.
#define SLICK_QUEUE_DEPRECATED_MACRO(msg) \
    __pragma(message(__FILE__ "(" SLICK_QUEUE_STR(__LINE__) "): warning: " msg))
#else
#define SLICK_QUEUE_DEPRECATED_MACRO(msg) _Pragma(SLICK_QUEUE_STR(GCC warning msg))
#endif

#ifdef SLICK_QUEUE_ENABLE_LOSS_DETECTION
SLICK_QUEUE_DEPRECATED_MACRO("SLICK_QUEUE_ENABLE_LOSS_DETECTION is ignored; it was replaced by "
                             "slick::queue_traits::enable_loss_detection. See README.")
#endif
#ifdef SLICK_QUEUE_ENABLE_CPU_RELAX
SLICK_QUEUE_DEPRECATED_MACRO("SLICK_QUEUE_ENABLE_CPU_RELAX is ignored; it was replaced by "
                             "slick::queue_traits::enable_cpu_relax. See README.")
#endif

// MSVC keeps [[no_unique_address]] a no-op for ABI reasons and spells the working one
// [[msvc::no_unique_address]]. This varies by compiler, not by user configuration, so
// unlike the macros above it carries no ODR risk.
#if defined(_MSC_VER)
#define SLICK_QUEUE_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define SLICK_QUEUE_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

namespace slick {

/**
 * @brief Compile-time feature configuration for SlickQueue.
 *
 * Derive and override only what you need:
 * @code
 *     struct my_traits : slick::queue_traits {
 *         static constexpr bool enable_reset_check = true;
 *     };
 *     slick::queue<int, my_traits> q(1024);
 * @endcode
 *
 * Because the configuration is a template parameter, two differently configured
 * queues are different types and can coexist in one program.
 */
struct queue_traits {
    /// Enable read_last(). Tracks the last published index.
    /// Cost: one CAS per publish(), one cacheline per instance.
    static constexpr bool enable_read_last = true;

    /// Detect a concurrent reset() during read() and rewind the cursor once.
    /// Cost: one relaxed load of the producer's reservation counter per read().
    static constexpr bool enable_reset_check = false;

    /// Per-instance counter of items skipped when a producer overruns a reader.
    /// Cost: one cacheline per instance, one relaxed fetch_add per lossy read().
    static constexpr bool enable_loss_detection = false;

    /// pause/yield backoff on contended CAS loops.
    static constexpr bool enable_cpu_relax = true;
};

/// queue_traits with the skipped-item counter enabled.
struct debug_queue_traits : queue_traits {
    static constexpr bool enable_loss_detection = true;
};

// Selecting between two fixed traits *types* - rather than making a queue_traits member
// depend on NDEBUG - keeps each struct a single definition everywhere. A debug/release
// mismatch across translation units then differs in the template-id, so it surfaces as an
// ordinary unresolved-symbol link error instead of a silent ODR violation.
#ifdef NDEBUG
using default_queue_traits = queue_traits;
#else
using default_queue_traits = debug_queue_traits;
#endif

/**
 * @brief Requirements on the Traits parameter of SlickQueue.
 *
 * Requiring exactly bool (not merely convertible to bool) rejects a wrong type at the
 * point of use with a readable constraint failure. Note it cannot catch a misspelled
 * override in a derived traits struct: the inherited member stays visible and silently
 * keeps the base value.
 */
template<typename Traits>
concept queue_traits_type = requires {
    requires std::same_as<std::remove_cv_t<decltype(Traits::enable_read_last)>, bool>;
    requires std::same_as<std::remove_cv_t<decltype(Traits::enable_reset_check)>, bool>;
    requires std::same_as<std::remove_cv_t<decltype(Traits::enable_loss_detection)>, bool>;
    requires std::same_as<std::remove_cv_t<decltype(Traits::enable_cpu_relax)>, bool>;
};

namespace detail {

// Deliberately not std::hardware_destructive_interference_size. Its value tracks -mtune
// and -mcpu, so two translation units built with different flags would disagree about
// alignof(SlickQueue<T>) and its member offsets - an ODR violation in a header-only
// library, where every consumer compiles this fresh with whatever flags it uses. That is
// exactly what GCC's -Winterference-size reports, and its advice is to define the
// constant yourself; a warning raised in a header is also the consumer's problem, not
// only ours, so suppressing it rather than fixing it would push it downstream into
// builds that may use -Werror.
//
// This is a local-padding constant, not part of the shared-memory contract: HEADER_SIZE
// and sizeof(slot) fix that layout independently, so the value may differ per platform
// without breaking a segment shared between them.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t cacheline_size = 128;  // Apple silicon reports 128
#else
inline constexpr std::size_t cacheline_size = 64;
#endif

// if constexpr cannot elide a member declaration, so the two optional counters live in
// these storage helpers, held as SLICK_QUEUE_NO_UNIQUE_ADDRESS members declared in the
// positions the raw members used to occupy. An enabled feature reproduces the previous
// object layout byte for byte; a disabled one takes no space.
//
// The alignas belongs on the *member declaration* in SlickQueue, not here: sizeof always
// rounds up to alignof, so a wrapper carrying alignas(cacheline_size) would itself be a
// full cacheline rather than the 8 bytes the raw member occupied.
//
// The two helpers must be distinct templates - [[no_unique_address]] members of the same
// empty type cannot both collapse to zero size.
template<bool Enable>
struct last_published_storage {
    std::atomic<uint64_t> value{ std::numeric_limits<uint64_t>::max() };
};
template<>
struct last_published_storage<false> {};

template<bool Enable>
struct loss_counter_storage {
    std::atomic<uint64_t> value{ 0 };
};
template<>
struct loss_counter_storage<false> {};

}  // namespace detail

/**
 * @brief A lock-free multi-producer multi-consumer queue with optional shared memory support.
 * 
 * This queue allows a multiple producer thread to write data and a multiple consumer thread to read data concurrently without locks.
 * It can optionally use shared memory for inter-process communication.
 * This queue is lossy: if producers outrun consumers, older data may be overwritten.
 * 
 * @tparam T The type of elements stored in the queue.
 * @tparam Traits Compile-time feature configuration, see slick::queue_traits.
 */
template<typename T, queue_traits_type Traits = default_queue_traits>
class SlickQueue {
    static constexpr uint64_t kInvalidIndex = std::numeric_limits<uint64_t>::max();

    struct slot {
        std::atomic_uint_fast64_t data_index{ kInvalidIndex };
        // Atomic, and always read through a single relaxed load bracketed by the
        // data_index checks around it. A wrapping producer recycles a slot while a
        // reader is inside that bracket, so a plain uint32_t here was a data race: the
        // acquire fence the readers use orders atomic accesses only, which left the
        // seqlock it implements without the guarantee it was written for, and left the
        // compiler free to re-load the field outside the bracket. A relaxed 32-bit
        // load or store is the same instruction as the plain access on every supported
        // target, so being correct here costs nothing.
        //
        // What remains is the formal gap every seqlock carries. The acquire fence gives
        // the load-load ordering the pattern needs, and gives it on every target this
        // runs on, but the standard states fence guarantees as synchronizes-with edges
        // and none of them forbids the relaxed size load from having returned a newer
        // generation's value when the re-check still observes the older one. Two changes
        // that look like they would close it do not. Making the re-check an acquire load
        // orders that load against what follows it, not against the size load before it,
        // so it is strictly weaker here than the fence already in place. And nothing
        // depends on the two fields sharing a cacheline: the fence does the ordering, and
        // the pattern holds just as well with them apart, so a future layout change is
        // free to separate them. The gap is theoretical, unlike the data race it
        // replaced, and closing it properly needs guarantees the language does not offer.
        std::atomic<uint32_t> size{ 1 };
    };
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
        "slot::size must be lock-free - the control array is shared between processes, "
        "and a lock-based atomic would embed a per-slot lock in the segment");
    static_assert(sizeof(slot) == 16 && alignof(slot) == 8,
        "slot layout is part of the shared-memory contract: the control array is indexed "
        "by sizeof(slot) and the data array starts after it");

    using reserved_info = uint64_t;

    static constexpr std::size_t cacheline_size = detail::cacheline_size;

    uint32_t size_;
    uint32_t mask_;
    T* data_ = nullptr;
    slot* control_ = nullptr;
    std::atomic<reserved_info>* reserved_ = nullptr;
    std::atomic<uint64_t>* last_published_ = nullptr;
    // Read-only after construction, so they belong in this line with the pointers above
    // rather than after the counters below, where they would share a cacheline with
    // whichever atomic sits last - reserved_local_, last_published_local_ or loss_count_
    // depending on Traits - and be invalidated by every producer CAS. They fit in the
    // padding this line already carried ahead of reserved_local_'s alignas, so keeping
    // them here costs no space in any configuration.
    bool own_ = false;
    bool use_shm_ = false;
    alignas(cacheline_size) std::atomic<reserved_info> reserved_local_{0};
    // Declared where the raw members used to be, and aligned here rather than inside the
    // helper, so an enabled feature lands on exactly its previous offset. The alignment
    // is cacheline_size when the feature is on and 1 when it is off, so a disabled member
    // cannot force padding around a zero-size object.
    static constexpr std::size_t last_published_align =
        Traits::enable_read_last ? cacheline_size : 1;
    static constexpr std::size_t loss_count_align =
        Traits::enable_loss_detection ? cacheline_size : 1;

    SLICK_QUEUE_NO_UNIQUE_ADDRESS alignas(last_published_align)
    detail::last_published_storage<Traits::enable_read_last> last_published_local_;
    SLICK_QUEUE_NO_UNIQUE_ADDRESS alignas(loss_count_align)
    detail::loss_counter_storage<Traits::enable_loss_detection> loss_count_;
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
    //   Offset 24-27 (4 bytes):  header_magic - 'SLQ' layout marker + feature nibble
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
    // Layout marker: the bytes 'SLQ' followed by an ASCII digit whose low nibble carries
    // the features the creator was built with. Only options that change the shared header
    // protocol get a bit; the purely local ones (reset check, loss detection, cpu relax)
    // cost nothing to mix on one segment and deliberately have none.
    //   bit 0    - the last-published index at LAST_PUBLISHED_OFFSET is maintained
    //   bits 1-3 - reserved for future shared-layout features, must be 0
    // Traits never reach the segment, so this word is the only cross-process signal of
    // what a peer agrees to maintain; every attacher matches it against its own - see
    // validate_header_magic().
    static constexpr uint32_t HEADER_MAGIC = 0x534C5131;               // 'SLQ1'
    static constexpr uint32_t HEADER_MAGIC_FEATURE_MASK = 0x0000000Fu; // feature nibble
    static constexpr uint32_t HEADER_MAGIC_READ_LAST = 0x1u;           // feature bit 0
    static constexpr uint32_t HEADER_MAGIC_EXPECTED =
        Traits::enable_read_last ? HEADER_MAGIC : (HEADER_MAGIC & ~HEADER_MAGIC_READ_LAST);
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
            if constexpr (Traits::enable_read_last) {
                last_published_ = &last_published_local_.value;
                last_published_->store(kInvalidIndex, std::memory_order_relaxed);
            }
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
     * @brief Get the number of items skipped due to overwrite.
     * @return Count of skipped items observed by this queue instance, or 0 when
     *         Traits::enable_loss_detection is false.
     */
    uint64_t loss_count() const noexcept {
        if constexpr (Traits::enable_loss_detection) {
            return loss_count_.value.load(std::memory_order_relaxed);
        } else {
            return 0;
        }
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
            slot.size.store(n, std::memory_order_relaxed);
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
        // Relaxed: the release store below is what publishes it, and a reader that
        // acquires data_index therefore sees this size.
        slot.size.store(n, std::memory_order_relaxed);
        slot.data_index.store(index, std::memory_order_release);

        if constexpr (Traits::enable_read_last) {
            // Unconditional: a shared segment whose creator does not maintain this index
            // is rejected at attach time, so reaching here means every peer maintains it.
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
        while (true) {
            if constexpr (Traits::enable_reset_check) {
                if (was_reset(read_index)) [[unlikely]] {
                    // The queue was reset out from under this cursor; restart from the
                    // beginning of the new generation. Terminates after one retry:
                    // was_reset(0) is `0 > counter`, which is never true.
                    read_index = 0;
                    continue;
                }
            }

            auto idx = read_index & mask_;
            slot* current_slot = &control_[idx];
            uint64_t index = current_slot->data_index.load(std::memory_order_acquire);

            // Recorded, not yet applied - the read below can still be sent round the
            // loop by a recycled slot, and this cursor is not advanced when that
            // happens, so charging the counter here would count the same overrun again
            // on the next pass. The shared-cursor overload defers it for the same
            // reason.
            [[maybe_unused]] uint64_t overrun = 0;
            if constexpr (Traits::enable_loss_detection) {
                if (index != kInvalidIndex && index > read_index && ((index & mask_) == idx)) {
                    overrun = index - read_index;
                }
            }

            if (index == kInvalidIndex || index < read_index) {
                // data not ready yet
                return std::make_pair(nullptr, 0);
            }
            else if (index > read_index && ((index & mask_) != idx)) {
                // queue wrapped, skip the unused slots
                read_index = index;
                continue;
            }

            // One load, then re-validate the slot (seqlock-style, as read_last() does).
            // A wrapping producer writes size before it advances data_index, so reading
            // the field twice - once to advance the cursor and once to return - could
            // take the two values from different generations and leave the cursor
            // describing a different record than the caller was handed. Retry rather
            // than report "no data": the next pass sees the new index and either reads
            // it or skips the lap, and the producer must complete another whole lap to
            // trigger this again, so it cannot spin.
            auto sz = current_slot->size.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (current_slot->data_index.load(std::memory_order_relaxed) != index) [[unlikely]] {
                continue;
            }

            if constexpr (Traits::enable_loss_detection) {
                if (overrun != 0) {
                    loss_count_.value.fetch_add(overrun, std::memory_order_relaxed);
                }
            }

            // index and read_index select the same slot here: they are either equal, or
            // index ran ahead within this same slot, which the branch above required.
            read_index = index + sz;
            return std::make_pair(&data_[idx], sz);
        }
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

            if constexpr (Traits::enable_reset_check) {
                if (was_reset(current_index)) [[unlikely]] {
                    // The queue was reset out from under this cursor. Rewind through a
                    // CAS so a peer consumer that already moved it on is not clobbered;
                    // either way the next iteration observes a cursor at or below the
                    // reservation counter, so this cannot spin.
                    read_index.compare_exchange_weak(current_index, 0,
                        std::memory_order_relaxed, std::memory_order_relaxed);
                    continue;
                }
            }

            auto idx = current_index & mask_;
            slot* current_slot = &control_[idx];
            uint64_t index = current_slot->data_index.load(std::memory_order_acquire);

            if (index == kInvalidIndex || index < current_index) {
                // data not ready yet
                return std::make_pair(nullptr, 0);
            }

            [[maybe_unused]] uint64_t overrun = 0;
            if constexpr (Traits::enable_loss_detection) {
                if (index > current_index && ((index & mask_) == idx)) {
                    overrun = index - current_index;
                }
            }

            if (index > current_index && ((index & mask_) != idx)) {
                // queue wrapped, skip the unused slots
                read_index.compare_exchange_weak(current_index, index, std::memory_order_relaxed, std::memory_order_relaxed);
                continue;
            }

            // One load, validated before the claim, so the cursor advances by exactly
            // the size handed back to the caller. Validating after the CAS instead would
            // be wrong: the claim would already have been published to the other
            // consumers, and refusing to return the item would drop it for all of them.
            auto sz = current_slot->size.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (current_slot->data_index.load(std::memory_order_relaxed) != index) [[unlikely]] {
                continue;
            }

            // Try to atomically claim this item
            uint64_t next_index = index + sz;
            if (read_index.compare_exchange_weak(current_index, next_index, std::memory_order_relaxed, std::memory_order_relaxed)) {
                if constexpr (Traits::enable_loss_detection) {
                    if (overrun != 0) {
                        loss_count_.value.fetch_add(overrun, std::memory_order_relaxed);
                    }
                }
                // Successfully claimed the item
                return std::make_pair(&data_[current_index & mask_], sz);
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
        static_assert(Traits::enable_read_last,
            "read_last() requires Traits::enable_read_last. There is no fallback: without "
            "the feature nothing maintains the last published index, and the reserved "
            "cursor is not a substitute because it reports reservations that were never "
            "published and truncates sizes above 65535.");

        auto last_index = last_published_->load(std::memory_order_acquire);
        if (last_index == kInvalidIndex) {
            return std::make_pair(nullptr, 0);
        }
        auto idx = last_index & mask_;
        slot& s = control_[idx];
        // The slot may already have been recycled by a wrapping producer, and publish()
        // writes slot.size before it advances last_published_. Validate the slot before
        // and after reading size (seqlock-style) so the returned {pointer, size} pair
        // always describes one and the same record.
        if (s.data_index.load(std::memory_order_acquire) != last_index) {
            return std::make_pair(nullptr, 0);
        }
        auto sz = s.size.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.data_index.load(std::memory_order_relaxed) != last_index) {
            return std::make_pair(nullptr, 0);
        }
        return std::make_pair(&data_[idx], sz);
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
        if constexpr (Traits::enable_read_last) {
            last_published_->store(kInvalidIndex, std::memory_order_relaxed);
        }
        if constexpr (Traits::enable_loss_detection) {
            loss_count_.value.store(0, std::memory_order_relaxed);
        }
    }

private:
    /**
     * @brief Detect that a concurrent reset() rewound the reservation counter below
     *        the absolute index still recorded in the slot being read.
     *
     * Shared by both read() overloads so the two recovery sites cannot drift apart.
     */
    bool was_reset(uint64_t read_index) const noexcept {
        // The test is on the *reader's cursor*, not on the slot it happens to land on.
        // A cursor beyond the reservation counter is asking for an index that was never
        // reserved, which can only happen if the counter was rewound by reset().
        //
        // Testing the slot's index instead would miss every case that matters: reset()
        // clears the control array before rewinding the counter, so the slot a stale
        // reader lands on holds either kInvalidIndex or a fresh low index - neither of
        // which is ahead of the counter, leaving the reader stuck returning nullptr and
        // eventually skipping the start of the new generation.
        //
        // No false positives: read_index is only ever advanced to index + slot.size, and
        // reserve(n) sets the counter to index + n, so read_index <= the counter always
        // holds in normal operation, with equality when the reader is caught up.
        return read_index > get_index(reserved_->load(std::memory_order_relaxed));
    }

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
        if constexpr (Traits::enable_cpu_relax) {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
            _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
            __asm__ __volatile__("yield" ::: "memory");
#else
            std::this_thread::yield();
#endif
        }
    }

    bool wait_for_shared_memory_ready(uint8_t* base, std::atomic<uint32_t>* init_state) const noexcept {
        constexpr int kMaxWaitMs = 2000;
        constexpr int kLegacyGraceMs = 5;

        for (int i = 0; i < kMaxWaitMs; ++i) {
            uint32_t state = init_state->load(std::memory_order_acquire);
            if (state == INIT_STATE_READY) {
                return true;
            }

            // A pre-v1.4.0 segment left its creator flag here rather than an init state.
            // Such a segment carries no layout marker and validate_header_magic() will
            // reject it; recognising it is what turns a 2s timeout into that precise
            // error, so the grace path stays even though the layout is unsupported.
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

    static std::string to_hex(uint32_t value) {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string out("0x00000000");
        for (int i = 0; i < 8; ++i) {
            out[9 - i] = digits[(value >> (i * 4)) & 0xF];
        }
        return out;
    }

    /**
     * @brief Reject an existing segment whose creator disagrees about the header protocol.
     *
     * Traits are a compile-time property of each peer and never reach the segment, so the
     * feature nibble in the marker is the only place a disagreement can be caught - and
     * attach time is the only moment at which it is still cheap. Both directions are
     * fatal: a queue that expects the last-published index would read a counter nobody
     * writes, and one that does not maintain it would silently freeze read_last() for
     * every peer that does.
     */
    void validate_header_magic(const uint8_t* base) const {
        uint32_t magic = reinterpret_cast<const std::atomic<uint32_t>*>(
            base + HEADER_MAGIC_OFFSET)->load(std::memory_order_acquire);
        if (magic == HEADER_MAGIC_EXPECTED) {
            return;
        }

        if ((magic & ~HEADER_MAGIC_FEATURE_MASK) != (HEADER_MAGIC & ~HEADER_MAGIC_FEATURE_MASK)) {
            throw std::runtime_error(
                "Shared memory does not carry a slick-queue layout marker. Expected " +
                to_hex(HEADER_MAGIC_EXPECTED) + " but got " + to_hex(magic) +
                "; segments created before v1.4.0 carry no marker and are not supported");
        }

        if ((magic & HEADER_MAGIC_FEATURE_MASK & ~HEADER_MAGIC_READ_LAST) != 0) {
            throw std::runtime_error(
                "Shared memory was created with unknown layout features. Marker " +
                to_hex(magic) + " is newer than this build understands (" +
                to_hex(HEADER_MAGIC_EXPECTED) + ")");
        }

        throw std::runtime_error(
            std::string("Shared memory feature mismatch: the segment was created with "
                        "enable_read_last=") +
            ((magic & HEADER_MAGIC_READ_LAST) ? "true" : "false") +
            " but this queue has enable_read_last=" +
            (Traits::enable_read_last ? "true" : "false"));
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

            validate_header_magic(base);
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

                // Publish this queue's feature nibble in the marker. It is what every
                // attacher matches itself against, so a peer that maintains the
                // last-published index and one that does not can no longer end up sharing
                // a segment in either direction. Stored unconditionally so a recycled
                // segment cannot leave a stale marker behind.
                auto* header_magic = new (base + HEADER_MAGIC_OFFSET) std::atomic<uint32_t>();
                header_magic->store(HEADER_MAGIC_EXPECTED, std::memory_order_release);

                // Initialize atomic header
                reserved_ = new (base) std::atomic<reserved_info>();
                reserved_->store(0, std::memory_order_relaxed);

                last_published_ = new (base + LAST_PUBLISHED_OFFSET) std::atomic<uint64_t>();
                last_published_->store(kInvalidIndex, std::memory_order_relaxed);

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

                validate_header_magic(base);
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
template<typename T, queue_traits_type Traits = default_queue_traits>
using queue = SlickQueue<T, Traits>;

}
