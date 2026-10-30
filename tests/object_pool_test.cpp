// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#include "utils/ObjectPool.h"

#include <cstddef>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

struct Item {
    std::vector<int> values;
    void reset() { values.clear(); }
};

struct ItemPolicy {
    Item* create() { return new Item; }
    void reset(Item* item) { item->reset(); }
    void destroy(Item* item) { delete item; }
};

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

}  // namespace

int main() {
    using Pool = bs::utils::ObjectPool<Item, ItemPolicy>;

    {
        Pool pool(ItemPolicy{}, Pool::Config{2, 4, 4});
        auto first = pool.tryAcquire();
        expect(static_cast<bool>(first), "first acquire succeeds");
        first->values.push_back(7);

        auto second = first;
        expect(second.useCount() == 2, "lease copy retains the slot");
        first.reset();
        expect(second.useCount() == 1, "one lease remains");
        expect(second->values.size() == 1, "object survives until last lease");

        auto third = pool.tryAcquire();
        expect(static_cast<bool>(third), "second concurrent acquire");
        auto fourth = pool.tryAcquire();
        expect(static_cast<bool>(fourth), "third concurrent acquire");
        auto fifth = pool.tryAcquire();
        expect(static_cast<bool>(fifth), "fourth concurrent acquire");
        auto sixth = pool.tryAcquire();
        expect(!sixth, "maximumRetained is enforced");

        third.reset();
        fourth.reset();
        fifth.reset();
        second.reset();

        const auto stats = pool.stats();
        expect(stats.live <= 4, "live objects stay under cap");
        expect(stats.reuses >= 1, "released objects are reused");
    }

    {
        Pool pool(ItemPolicy{}, Pool::Config{0, 8, 0});
        auto lease = pool.tryAcquire();
        expect(static_cast<bool>(lease), "pool acquires without preallocation");
        lease->values.assign(32, 3);
        lease.reset();
        auto again = pool.tryAcquire();
        expect(static_cast<bool>(again), "pool reuses after reset");
        expect(again->values.empty(), "policy reset clears the object");
        again.reset();
        pool.trimAll();
        expect(pool.stats().live == 0, "trimAll destroys idle objects");
    }

    {
        auto pool = std::make_unique<Pool>(ItemPolicy{}, Pool::Config{1, 4, 4});
        auto lease = pool->tryAcquire();
        expect(static_cast<bool>(lease), "lease before pool destruction");
        pool.reset();
        expect(lease->values.empty(), "outstanding lease keeps the object alive");
        lease.reset();
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d object-pool failure(s)\n", failures);
        return 1;
    }

    std::puts("object pool: ok");
    return 0;
}
