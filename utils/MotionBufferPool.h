// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/*
 * ===========================================================================
 * MotionBufferPool — zero-copy consumer buffer pool for motion vectors
 * ===========================================================================
 *
 * The parser writes the dense 4x4 MV grid directly into a slot obtained from
 * this pool.  Slots are reused through ObjectPool and returned automatically
 * when the last MotionVectorBufferLease drops.
 *
 * A reference picture's lease is retained by the parser's MV-DPB, so the
 * consumer can safely use its own pool: a slot is not recycled while a
 * reference still points at it.  Non-reference pictures release their parser
 * lease immediately after the ready callback.
 *
 * This pool is optional.  A consumer can implement the two callbacks with
 * any allocator or pool; the parser only needs an aligned pointer, capacity,
 * and an optional lease.
 */

#include "../syntax/avc_motion_vector.hpp"
#include "ObjectPool.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

namespace bs {
namespace utils {

class MotionBufferPool {
   public:
    struct Config {
        std::size_t minimumRetained;
        std::size_t maxBuffers;
        std::size_t maxBytes;
        std::size_t alignment;

        Config(std::size_t minimum_retained = 4,
               std::size_t max_buffers = 32,
               std::size_t max_bytes = 256u << 20,
               std::size_t buffer_alignment = 64)
            : minimumRetained(minimum_retained),
              maxBuffers(max_buffers),
              maxBytes(max_bytes),
              alignment(buffer_alignment) {}
    };

    struct Stats {
        std::size_t buffersInUse = 0;
        std::size_t bytesInUse = 0;
        std::size_t poolLive = 0;
        std::size_t poolIdle = 0;
        std::size_t allocations = 0;
        std::size_t reuses = 0;
    };

    explicit MotionBufferPool(const Config& config = Config())
        : config_(config),
          counters_(std::make_shared<Counters>()),
          pool_(SlotPolicy{},
                ObjectPool<Slot, SlotPolicy>::Config{
                        config.minimumRetained,
                        config.maxBuffers,
                        config.maxBuffers}) {}

    ~MotionBufferPool() = default;

    MotionBufferPool(const MotionBufferPool&) = delete;
    MotionBufferPool& operator=(const MotionBufferPool&) = delete;
    MotionBufferPool(MotionBufferPool&&) = delete;
    MotionBufferPool& operator=(MotionBufferPool&&) = delete;

    [[nodiscard]] avc::MotionVectorBuffer acquire(
            const avc::MotionVectorBufferRequest& request) {
        avc::MotionVectorBuffer result{};

        auto lease = pool_.tryAcquire();
        if (!lease) {
            return result;
        }

        Slot* slot = lease.get();
        if (slot == nullptr) {
            return result;
        }

        const std::size_t requested_alignment =
                std::max(config_.alignment, static_cast<std::size_t>(64));
        const std::size_t alignment = std::bit_ceil(requested_alignment);

        if (slot->data == nullptr || slot->capacity < request.required_bytes ||
            slot->alignment != alignment) {
            if (!slot->allocate(request.required_bytes, alignment)) {
                return result; /* lease returns the slot to the pool */
            }
        }

        const std::size_t bytes = slot->capacity;
        const std::size_t current = counters_->bytesInUse.load(std::memory_order_acquire);
        if (config_.maxBytes != 0 && current + bytes > config_.maxBytes) {
            return result;
        }

        counters_->bytesInUse.fetch_add(bytes, std::memory_order_acq_rel);
        counters_->buffersInUse.fetch_add(1, std::memory_order_acq_rel);

        /*
         * The shared_ptr deleter owns the ObjectPool lease.  When the parser
         * and every consumer copy are gone, the deleter returns the slot to
         * the pool and updates the byte counters.
         */
        auto counters = counters_;
        std::shared_ptr<void> lifetime(
                slot->data,
                [lease = std::move(lease), counters, bytes](void*) mutable {
                    counters->bytesInUse.fetch_sub(bytes, std::memory_order_acq_rel);
                    counters->buffersInUse.fetch_sub(1, std::memory_order_acq_rel);
                    lease.reset();
                });

        result.data = slot->data;
        result.capacity = slot->capacity;
        result.lease = std::move(lifetime);
        return result;
    }

    /*
     * Convenience for callers installing the callbacks directly.
     *
     *     handlers.motion_vectors.ctx = &pool;
     *     handlers.motion_vectors.acquire =
     *         &MotionBufferPool::acquireThunk;
     */
    static avc::MotionVectorBuffer acquireThunk(
            void* ctx,
            const avc::MotionVectorBufferRequest& request) {
        return static_cast<MotionBufferPool*>(ctx)->acquire(request);
    }

    [[nodiscard]] Stats stats() const noexcept {
        Stats result{};
        result.buffersInUse = counters_->buffersInUse.load(std::memory_order_acquire);
        result.bytesInUse = counters_->bytesInUse.load(std::memory_order_acquire);
        const auto pool = pool_.stats();
        result.poolLive = pool.live;
        result.poolIdle = pool.idle;
        result.allocations = pool.allocations;
        result.reuses = pool.reuses;
        return result;
    }

    void trim() noexcept { pool_.trim(); }
    void trimAll() noexcept { pool_.trimAll(); }

   private:
    struct Slot {
        void* data = nullptr;
        std::size_t capacity = 0;
        std::size_t alignment = 0;

        Slot() = default;
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

        ~Slot() { releaseStorage(); }

        [[nodiscard]] bool allocate(std::size_t bytes, std::size_t new_alignment) {
            releaseStorage();
            if (bytes == 0 || new_alignment == 0) {
                return false;
            }
            try {
                data = ::operator new(bytes, std::align_val_t{new_alignment});
            } catch (...) {
                data = nullptr;
                return false;
            }
            capacity = bytes;
            alignment = new_alignment;
            return true;
        }

        void releaseStorage() noexcept {
            if (data != nullptr) {
                ::operator delete(data, std::align_val_t{alignment});
                data = nullptr;
            }
            capacity = 0;
            alignment = 0;
        }

        void reset() noexcept {
            /* Keep the storage: the next picture usually has the same size. */
        }
    };

    struct SlotPolicy {
        [[nodiscard]] Slot* create() { return new Slot; }
        void reset(Slot* slot) noexcept { slot->reset(); }
        void destroy(Slot* slot) noexcept { delete slot; }
    };

    struct Counters {
        std::atomic<std::size_t> buffersInUse{0};
        std::atomic<std::size_t> bytesInUse{0};
    };

    Config config_;
    std::shared_ptr<Counters> counters_;
    ObjectPool<Slot, SlotPolicy> pool_;
};

}  // namespace utils
}  // namespace bs
