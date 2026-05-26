#include "allocator/pool_allocator.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <unordered_set>
#include <vector>

using namespace cas::memory;

namespace {
[[nodiscard]] bool is_aligned(const void* p, std::size_t alignment) noexcept {
    return (reinterpret_cast<std::uintptr_t>(p) % alignment) == 0;
}
} // namespace

// ============================================================================
// Basic allocate/free correctness
// ============================================================================
TEST(PoolAllocatorBasic, AllocateReturnsUsablyWritableMemory) {
    PoolAllocator alloc(1 << 20); // 1 MiB
    void* p = alloc.allocate(128);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xAB, 128);
    EXPECT_EQ(static_cast<std::byte*>(p)[0], std::byte{0xAB});
    EXPECT_EQ(static_cast<std::byte*>(p)[127], std::byte{0xAB});
    alloc.deallocate(p);
}

TEST(PoolAllocatorBasic, DefaultAlignmentIsSixtyFourBytes) {
    PoolAllocator alloc(1 << 20);
    for (int i = 0; i < 50; ++i) {
        void* p = alloc.allocate(static_cast<std::size_t>(1 + i));
        EXPECT_TRUE(is_aligned(p, PoolAllocator::kDefaultAlignment)) << "iteration " << i;
    }
}

TEST(PoolAllocatorBasic, ExplicitPowerOfTwoAlignmentsAreHonored) {
    PoolAllocator alloc(4 << 20); // 4 MiB
    for (std::size_t align : {std::size_t{8}, std::size_t{16}, std::size_t{32}, std::size_t{64},
                               std::size_t{128}, std::size_t{256}, std::size_t{512}, std::size_t{4096}}) {
        void* p = alloc.allocate(64, align);
        EXPECT_TRUE(is_aligned(p, align)) << "alignment=" << align;
        alloc.deallocate(p);
    }
}

TEST(PoolAllocatorBasic, RejectsNonPowerOfTwoAlignment) {
    PoolAllocator alloc(1 << 20);
    EXPECT_THROW(alloc.allocate(64, 48), InvalidAlignmentError);
    EXPECT_THROW(alloc.allocate(64, 0), InvalidAlignmentError);
}

TEST(PoolAllocatorBasic, ZeroSizeAllocationReturnsUniqueValidPointer) {
    PoolAllocator alloc(1 << 20);
    void* a = alloc.allocate(0);
    void* b = alloc.allocate(0);
    EXPECT_NE(a, nullptr);
    EXPECT_NE(b, nullptr);
    EXPECT_NE(a, b);
    alloc.deallocate(a);
    alloc.deallocate(b);
}

TEST(PoolAllocatorBasic, NullptrDeallocateIsNoOp) {
    PoolAllocator alloc(1 << 20);
    EXPECT_NO_THROW(alloc.deallocate(nullptr));
}

TEST(PoolAllocatorBasic, DeallocatingForeignPointerThrows) {
    PoolAllocator alloc(1 << 20);
    int stack_var = 0;
    EXPECT_THROW(alloc.deallocate(&stack_var), InvalidPointerError);
}

TEST(PoolAllocatorBasic, DoubleFreeIsDetected) {
    PoolAllocator alloc(1 << 20);
    void* p = alloc.allocate(64);
    alloc.deallocate(p);
    EXPECT_THROW(alloc.deallocate(p), InvalidPointerError);
}

// ============================================================================
// Out-of-memory behavior
// ============================================================================
TEST(PoolAllocatorBasic, ThrowsOutOfMemoryWhenArenaExhausted) {
    PoolAllocator alloc(4096); // small, page-sized arena
    EXPECT_THROW(alloc.allocate(1 << 20), OutOfMemoryError);
}

// ============================================================================
// Coalescing
// ============================================================================
TEST(PoolAllocatorCoalescing, AdjacentFreedBlocksMergeIntoOne) {
    PoolAllocator alloc(1 << 16); // 64 KiB, small enough that fragmentation is observable

    void* a = alloc.allocate(256);
    void* b = alloc.allocate(256);
    void* c = alloc.allocate(256);
    (void)a; (void)c;

    const auto stats_before = alloc.stats();
    const std::size_t largest_before = stats_before.largest_free_block_bytes;

    alloc.deallocate(b); // free the middle block -- can't fully coalesce yet (neighbors still live)
    alloc.deallocate(a); // now a+b should merge
    alloc.deallocate(c); // and then c should merge into the (a+b) run too

    const auto stats_after = alloc.stats();
    EXPECT_GT(stats_after.largest_free_block_bytes, largest_before);
    EXPECT_TRUE(alloc.validate_heap());

    // With every allocation freed, the whole arena minus one remaining
    // block header should be reclaimed into a single free block again.
    EXPECT_EQ(stats_after.num_free_blocks, 1u);
}

TEST(PoolAllocatorCoalescing, FreeingInReverseOrderAlsoFullyCoalesces) {
    PoolAllocator alloc(1 << 16);

    std::vector<void*> blocks;
    for (int i = 0; i < 8; ++i) {
        blocks.push_back(alloc.allocate(128));
    }
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        alloc.deallocate(*it);
    }

    EXPECT_TRUE(alloc.validate_heap());
    EXPECT_EQ(alloc.stats().num_free_blocks, 1u);
    EXPECT_EQ(alloc.stats().num_active_allocations, 0u);
}

// ============================================================================
// Fragmentation & overhead metrics
// ============================================================================
TEST(PoolAllocatorMetrics, ExternalFragmentationIsZeroForFreshArena) {
    PoolAllocator alloc(1 << 16);
    EXPECT_DOUBLE_EQ(alloc.stats().external_fragmentation_pct(), 0.0);
}

TEST(PoolAllocatorMetrics, ExternalFragmentationRisesWithScatteredFreeBlocks) {
    PoolAllocator alloc(1 << 16);

    std::vector<void*> blocks;
    for (int i = 0; i < 16; ++i) {
        blocks.push_back(alloc.allocate(256));
    }
    // Free every other block, leaving many small, non-adjacent free holes.
    for (std::size_t i = 0; i < blocks.size(); i += 2) {
        alloc.deallocate(blocks[i]);
    }

    const auto stats = alloc.stats();
    EXPECT_GT(stats.num_free_blocks, 1u);
    EXPECT_GT(stats.external_fragmentation_pct(), 0.0);

    // Clean up remaining allocations.
    for (std::size_t i = 1; i < blocks.size(); i += 2) {
        alloc.deallocate(blocks[i]);
    }
}

TEST(PoolAllocatorMetrics, MetadataOverheadIsPositiveAndBounded) {
    PoolAllocator alloc(1 << 16);
    void* p = alloc.allocate(64);
    const auto stats = alloc.stats();
    EXPECT_GT(stats.metadata_overhead_pct(), 0.0);
    EXPECT_LT(stats.metadata_overhead_pct(), 100.0);
    alloc.deallocate(p);
}

// ============================================================================
// Heap integrity under randomized allocation churn ("no leaks / no corruption")
// ============================================================================
TEST(PoolAllocatorStress, RandomizedAllocDeallocChurnKeepsHeapValid) {
    PoolAllocator alloc(2 << 20); // 2 MiB
    std::vector<std::pair<void*, std::size_t>> live;
    std::mt19937 rng(7);
    std::uniform_int_distribution<std::size_t> size_dist(1, 512);
    std::uniform_int_distribution<int> action_dist(0, 1);

    for (int iter = 0; iter < 5000; ++iter) {
        if (live.empty() || action_dist(rng) == 0) {
            const std::size_t size = size_dist(rng);
            void* p = alloc.allocate(size);
            std::memset(p, static_cast<int>(size & 0xFF), size); // fill to catch corruption
            live.emplace_back(p, size);
        } else {
            std::uniform_int_distribution<std::size_t> idx_dist(0, live.size() - 1);
            std::size_t idx = idx_dist(rng);
            auto [ptr, size] = live[idx];
            // Verify our own data wasn't corrupted by neighboring metadata.
            EXPECT_EQ(static_cast<std::byte*>(ptr)[0], std::byte(size & 0xFF));
            alloc.deallocate(ptr);
            live[idx] = live.back();
            live.pop_back();
        }
    }
    for (auto& [ptr, size] : live) {
        (void)size;
        alloc.deallocate(ptr);
    }

    EXPECT_TRUE(alloc.validate_heap());
    EXPECT_EQ(alloc.stats().num_active_allocations, 0u);
}

TEST(PoolAllocatorStress, NoOverlapBetweenConcurrentLiveAllocations) {
    PoolAllocator alloc(1 << 20);
    std::vector<std::pair<std::uintptr_t, std::size_t>> ranges;

    for (int i = 0; i < 200; ++i) {
        std::size_t size = 32 + static_cast<std::size_t>(i % 40) * 8;
        void* p = alloc.allocate(size, 64);
        auto addr = reinterpret_cast<std::uintptr_t>(p);
        for (auto& [existing_addr, existing_size] : ranges) {
            const bool disjoint = (addr + size <= existing_addr) || (existing_addr + existing_size <= addr);
            EXPECT_TRUE(disjoint) << "overlap detected between new block and existing block";
        }
        ranges.emplace_back(addr, size);
    }
}
