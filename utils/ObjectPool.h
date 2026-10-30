// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/*
 * ===========================================================================
 * ObjectPool — deterministic object pool for the bitstream parser
 * ===========================================================================
 *
 * This is the parser-local replacement for the host's general-purpose
 * RAIIPool.  It keeps the useful parts (ref-counted leases, policy-based
 * create/reset/destroy, capacity-retaining reset) and deliberately drops the
 * parts a header-only bitstream parser must not depend on:
 *
 *   - no global eviction manager and no background thread;
 *   - no thread-local caches;
 *   - no idle-timeout guessing;
 *   - no dependency on the application's platform/log headers.
 *
 * Acquisition is bounded and non-throwing: `tryAcquire()` returns an empty
 * lease when the configured cap is reached or when the policy cannot create
 * an object.  The parser uses that to fall back instead of aborting a stream.
 *
 * Lifetime:
 *
 *   ObjectPool owns a shared PoolState.  Every Lease keeps that state alive,
 *   so destroying the pool while a lease is outstanding is safe: the object
 *   is reset/destroyed when the last lease drops.
 */

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace bs {
namespace utils {

template <typename T, typename Policy>
class ObjectPool {
   public:
    struct Config {
        /*
         * Objects kept warm.  The constructor preallocates them (best
         * effort) and trim() never drops below this count.
         */
        std::size_t minimumRetained = 0;

        /*
         * Hard cap on live objects (idle + in use).  Zero means unlimited.
         */
        std::size_t maximumRetained = 0;

        /*
         * Hard cap on idle objects.  Zero means unlimited.  When a lease is
         * released and the idle cap is exceeded, the object is destroyed
         * rather than retained.
         */
        std::size_t maximumIdle = 0;

        Config() = default;

        Config(std::size_t minimum, std::size_t maximum, std::size_t max_idle)
            : minimumRetained(minimum),
              maximumRetained(maximum),
              maximumIdle(max_idle) {}
    };

    struct Stats {
        std::size_t live = 0;
        std::size_t idle = 0;
        std::size_t inUse = 0;
        std::size_t allocations = 0;
        std::size_t reuses = 0;
        std::size_t peakLive = 0;
    };

   private:
    struct Slot {
        T* object = nullptr;
        std::atomic<std::uint32_t> refs{0};
        bool inUse = false;
    };

    struct PoolState {
        PoolState(Policy&& policy_value, const Config& config_value)
            : policy(std::move(policy_value)), config(config_value) {}

        ~PoolState() {
            for (auto& slot : slots) {
                destroyObject(slot->object);
                slot->object = nullptr;
            }
        }

        [[nodiscard]] bool canCreate() const noexcept {
            return config.maximumRetained == 0 || live < config.maximumRetained;
        }

        [[nodiscard]] T* createObject() noexcept {
            try {
                return policy.create();
            } catch (...) {
                return nullptr;
            }
        }

        void destroyObject(T* object) noexcept {
            if (object == nullptr) {
                return;
            }
            try {
                policy.destroy(object);
            } catch (...) {
                /* A policy destructor must not take the pool down. */
            }
        }

        [[nodiscard]] Slot* allocSlot() {
            auto slot = std::make_unique<Slot>();
            Slot* raw = slot.get();
            slots.push_back(std::move(slot));
            return raw;
        }

        /*
         * Create an object and put it in the idle list.  Returns false when
         * the policy refuses or the cap is reached.
         */
        bool preallocateOne() noexcept {
            std::lock_guard<std::mutex> lock(mutex);
            if (!canCreate()) {
                return false;
            }
            T* object = createObject();
            if (object == nullptr) {
                return false;
            }
            try {
                Slot* slot = allocSlot();
                slot->object = object;
                slot->refs.store(0, std::memory_order_relaxed);
                slot->inUse = false;
                idle.push_back(slot);
                ++live;
                ++idleCount;
                ++allocations;
                peakLive = std::max(peakLive, live);
                return true;
            } catch (...) {
                destroyObject(object);
                return false;
            }
        }

        /*
         * Acquire a slot with refs == 1.  The caller wraps it in a Lease.
         */
        [[nodiscard]] Slot* tryAcquireSlot() noexcept {
            std::lock_guard<std::mutex> lock(mutex);

            while (!idle.empty()) {
                Slot* slot = idle.back();
                idle.pop_back();
                --idleCount;

                if (slot->object == nullptr) {
                    slot->object = createObject();
                    if (slot->object == nullptr) {
                        continue; /* leave the vacant slot for later */
                    }
                    ++allocations;
                    ++live;
                    peakLive = std::max(peakLive, live);
                }

                slot->inUse = true;
                slot->refs.store(1, std::memory_order_release);
                ++inUseCount;
                ++reuses;
                return slot;
            }

            if (!canCreate()) {
                return nullptr;
            }

            T* object = createObject();
            if (object == nullptr) {
                return nullptr;
            }

            try {
                Slot* slot = allocSlot();
                slot->object = object;
                slot->inUse = true;
                slot->refs.store(1, std::memory_order_release);
                ++live;
                ++inUseCount;
                ++allocations;
                peakLive = std::max(peakLive, live);
                return slot;
            } catch (...) {
                destroyObject(object);
                return nullptr;
            }
        }

        void release(Slot* slot) noexcept {
            if (slot == nullptr) {
                return;
            }

            T* object = slot->object;
            bool resetOk = true;
            if (object != nullptr) {
                try {
                    policy.reset(object);
                } catch (...) {
                    resetOk = false;
                }
            }

            std::lock_guard<std::mutex> lock(mutex);

            slot->refs.store(0, std::memory_order_release);
            slot->inUse = false;
            if (inUseCount > 0) {
                --inUseCount;
            }

            if (!resetOk) {
                destroyObject(object);
                slot->object = nullptr;
                if (live > 0) {
                    --live;
                }
                return;
            }

            slot->object = object;
            idle.push_back(slot);
            ++idleCount;

            if (config.maximumIdle != 0 && idleCount > config.maximumIdle && live > config.minimumRetained) {
                Slot* victim = idle.back();
                idle.pop_back();
                --idleCount;
                destroyObject(victim->object);
                victim->object = nullptr;
                if (live > 0) {
                    --live;
                }
            }
        }

        void trim(std::size_t keep) noexcept {
            std::vector<T*> doomed;

            {
                std::lock_guard<std::mutex> lock(mutex);
                while (!idle.empty() && idle.size() > keep) {
                    Slot* slot = idle.back();
                    idle.pop_back();
                    --idleCount;
                    if (slot->object != nullptr) {
                        doomed.push_back(slot->object);
                        slot->object = nullptr;
                        if (live > 0) {
                            --live;
                        }
                    }
                }
            }

            for (T* object : doomed) {
                destroyObject(object);
            }
        }

        PoolState(const PoolState&) = delete;
        PoolState& operator=(const PoolState&) = delete;

        Policy policy;
        Config config;

        mutable std::mutex mutex;
        std::vector<std::unique_ptr<Slot>> slots;
        std::vector<Slot*> idle;

        std::size_t live = 0;
        std::size_t idleCount = 0;
        std::size_t inUseCount = 0;
        std::size_t allocations = 0;
        std::size_t reuses = 0;
        std::size_t peakLive = 0;
    };

   public:
    /*
     * A ref-counted borrow of one pooled object.  Copies are cheap; the
     * object returns to the pool when the last Lease drops.
     */
    class Lease {
       public:
        Lease() noexcept = default;

        Lease(const Lease& other) noexcept
            : state_(other.state_), slot_(other.slot_) {
            retain();
        }

        Lease(Lease&& other) noexcept
            : state_(std::move(other.state_)), slot_(other.slot_) {
            other.slot_ = nullptr;
        }

        ~Lease() { reset(); }

        Lease& operator=(const Lease& other) noexcept {
            if (this != &other) {
                reset();
                state_ = other.state_;
                slot_ = other.slot_;
                retain();
            }
            return *this;
        }

        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                reset();
                state_ = std::move(other.state_);
                slot_ = other.slot_;
                other.slot_ = nullptr;
            }
            return *this;
        }

        [[nodiscard]] T* get() const noexcept {
            return slot_ != nullptr ? slot_->object : nullptr;
        }

        [[nodiscard]] T* operator->() const noexcept { return get(); }
        [[nodiscard]] T& operator*() const noexcept { return *get(); }
        [[nodiscard]] explicit operator bool() const noexcept { return get() != nullptr; }

        void reset() noexcept {
            if (slot_ == nullptr) {
                state_.reset();
                return;
            }

            if (slot_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                if (state_) {
                    state_->release(slot_);
                }
            }

            slot_ = nullptr;
            state_.reset();
        }

        [[nodiscard]] std::uint32_t useCount() const noexcept {
            return slot_ != nullptr ? slot_->refs.load(std::memory_order_acquire) : 0;
        }

       private:
        friend class ObjectPool;

        Lease(std::shared_ptr<PoolState> state, Slot* slot) noexcept
            : state_(std::move(state)), slot_(slot) {}

        void retain() noexcept {
            if (slot_ != nullptr) {
                slot_->refs.fetch_add(1, std::memory_order_relaxed);
            }
        }

        std::shared_ptr<PoolState> state_;
        Slot* slot_ = nullptr;
    };

    explicit ObjectPool(Policy policy = {}, const Config& config = Config())
        : state_(std::make_shared<PoolState>(std::move(policy), config)) {
        for (std::size_t i = 0; i < config.minimumRetained; ++i) {
            if (!state_->preallocateOne()) {
                break;
            }
        }
    }

    ~ObjectPool() = default;

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    /*
     * Bounded, non-throwing acquisition.  An empty lease means the pool is at
     * its cap or the policy could not create an object.
     */
    [[nodiscard]] Lease tryAcquire() noexcept {
        if (!state_) {
            return Lease{};
        }
        try {
            Slot* slot = state_->tryAcquireSlot();
            if (slot == nullptr) {
                return Lease{};
            }
            return Lease(state_, slot);
        } catch (...) {
            return Lease{};
        }
    }

    /*
     * Alias for callers that do not want to distinguish try/acquire.  The
     * parser uses one bounded semantic everywhere.
     */
    [[nodiscard]] Lease acquire() noexcept { return tryAcquire(); }

    void trim() noexcept {
        if (state_) {
            state_->trim(state_->config.minimumRetained);
        }
    }

    void trimAll() noexcept {
        if (state_) {
            state_->trim(0);
        }
    }

    [[nodiscard]] Stats stats() const noexcept {
        Stats result{};
        if (!state_) {
            return result;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        result.live = state_->live;
        result.idle = state_->idleCount;
        result.inUse = state_->inUseCount;
        result.allocations = state_->allocations;
        result.reuses = state_->reuses;
        result.peakLive = state_->peakLive;
        return result;
    }

   private:
    std::shared_ptr<PoolState> state_;
};

}  // namespace utils
}  // namespace bs
