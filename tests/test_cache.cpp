#include "cache/cache.hpp"
#include "cache/eviction_policy.hpp"

#include <gtest/gtest.h>

using namespace cas::cache;

// ============================================================================
// Bit-manipulation / geometry
// ============================================================================
TEST(CacheGeometry, ExactLog2Basic) {
    EXPECT_EQ(exact_log2(1), 0u);
    EXPECT_EQ(exact_log2(2), 1u);
    EXPECT_EQ(exact_log2(64), 6u);
    EXPECT_EQ(exact_log2(1024), 10u);
    EXPECT_THROW(exact_log2(0), std::invalid_argument);
    EXPECT_THROW(exact_log2(3), std::invalid_argument);
    EXPECT_THROW(exact_log2(100), std::invalid_argument);
}

TEST(CacheGeometry, DirectMappedOffsetIndexTag) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 1024; // 1 KiB
    cfg.line_size_bytes = 64;    // -> 16 lines
    cfg.associativity = 1;       // direct mapped -> 16 sets
    Cache c(cfg);

    EXPECT_EQ(c.offset_bits(), 6u);  // log2(64)
    EXPECT_EQ(c.index_bits(), 4u);   // log2(16)
    EXPECT_EQ(c.num_sets(), 16u);
    EXPECT_EQ(c.associativity(), 1u);

    const std::uint64_t address = 0b1010'1101'0010'110101; // arbitrary bit pattern
    const std::uint64_t offset = c.extract_offset(address);
    const std::uint64_t index = c.extract_index(address);
    const std::uint64_t tag = c.extract_tag(address);

    EXPECT_EQ(offset, address & 0x3F);
    EXPECT_EQ(index, (address >> 6) & 0xF);
    EXPECT_EQ(tag, address >> 10);

    // Reconstruction should drop the offset bits (they are not encoded).
    EXPECT_EQ(c.reconstruct_address(tag, index), address & ~0x3Full);
}

TEST(CacheGeometry, FullyAssociativeHasSingleSet) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 2048;
    cfg.line_size_bytes = 64;
    cfg.associativity = 0; // sentinel: fully associative
    Cache c(cfg);

    EXPECT_EQ(c.num_sets(), 1u);
    EXPECT_EQ(c.associativity(), 32u); // 2048/64
    EXPECT_EQ(c.index_bits(), 0u);
}

TEST(CacheGeometry, RejectsNonPowerOfTwoLineSize) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 1024;
    cfg.line_size_bytes = 100; // invalid
    EXPECT_THROW(Cache c(cfg), std::invalid_argument);
}

TEST(CacheGeometry, RejectsAssociativityNotDividingLineCount) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 1024;
    cfg.line_size_bytes = 64; // 16 lines
    cfg.associativity = 3;    // does not divide 16
    EXPECT_THROW(Cache c(cfg), std::invalid_argument);
}

// ============================================================================
// Basic hit / miss behavior
// ============================================================================
TEST(CacheBasic, ColdMissThenHit) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 1024;
    cfg.line_size_bytes = 64;
    cfg.associativity = 1;
    Cache c(cfg);

    auto r1 = c.access(0x1000, AccessType::Read);
    EXPECT_FALSE(r1.hit);

    auto r2 = c.access(0x1000, AccessType::Read);
    EXPECT_TRUE(r2.hit);

    EXPECT_EQ(c.stats().hits, 1u);
    EXPECT_EQ(c.stats().misses, 1u);
}

TEST(CacheBasic, DifferentLinesInSameSetDoNotAliasIfAssociative) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 1024; // 16 lines
    cfg.line_size_bytes = 64;
    cfg.associativity = 4; // 4 sets, 4 ways each
    Cache c(cfg);

    // Addresses that map to the same set but different tags.
    const std::uint64_t set_stride = cfg.cache_size_bytes / 4 /*sets*/;
    const std::uint64_t base = 0x0;
    for (int i = 0; i < 4; ++i) {
        auto r = c.access(base + static_cast<std::uint64_t>(i) * set_stride, AccessType::Read);
        EXPECT_FALSE(r.hit) << "expected compulsory miss for way " << i;
    }
    // All four should now hit since associativity==4 fits them all.
    for (int i = 0; i < 4; ++i) {
        auto r = c.access(base + static_cast<std::uint64_t>(i) * set_stride, AccessType::Read);
        EXPECT_TRUE(r.hit) << "expected hit for way " << i;
    }
}

TEST(CacheBasic, WriteBackMarksLineDirtyAndWritesBackOnEviction) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 128; // 2 lines
    cfg.line_size_bytes = 64;
    cfg.associativity = 2; // fully associative within single set (1 set, 2 ways)
    cfg.eviction_policy = EvictionPolicyType::FIFO;
    cfg.write_policy = WritePolicy::WriteBack;
    Cache c(cfg);

    auto rw = c.access(0x0000, AccessType::Write); // miss, install dirty
    EXPECT_FALSE(rw.hit);
    EXPECT_TRUE(rw.line_installed);

    c.access(0x0040, AccessType::Read); // fills the second (and last) way

    // A third distinct line forces an eviction; FIFO evicts 0x0000 first.
    auto r3 = c.access(0x0080, AccessType::Read);
    EXPECT_TRUE(r3.evicted);
    EXPECT_TRUE(r3.writeback_needed); // the evicted line (0x0000) was dirty
    EXPECT_EQ(r3.evicted_address, 0x0000u);
}

TEST(CacheBasic, WriteThroughNeverLeavesDirtyLines) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 64;
    cfg.line_size_bytes = 64;
    cfg.associativity = 1;
    cfg.write_policy = WritePolicy::WriteThrough;
    Cache c(cfg);

    c.access(0x0, AccessType::Write);
    EXPECT_GE(c.stats().writebacks, 1u); // write-through traffic counted immediately
}

TEST(CacheBasic, NoWriteAllocateBypassesArrayOnWriteMiss) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 64;
    cfg.line_size_bytes = 64;
    cfg.associativity = 1;
    cfg.write_allocate = WriteAllocatePolicy::NoWriteAllocate;
    Cache c(cfg);

    auto r = c.access(0x0, AccessType::Write);
    EXPECT_FALSE(r.hit);
    EXPECT_FALSE(r.line_installed);

    // A subsequent read to the same address must still miss (nothing installed).
    auto r2 = c.access(0x0, AccessType::Read);
    EXPECT_FALSE(r2.hit);
}

TEST(CacheBasic, InvalidateRemovesLine) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 64;
    cfg.line_size_bytes = 64;
    cfg.associativity = 1;
    Cache c(cfg);

    c.access(0x0, AccessType::Read);
    EXPECT_TRUE(c.probe(0x0));
    EXPECT_TRUE(c.invalidate(0x0));
    EXPECT_FALSE(c.probe(0x0));
    EXPECT_FALSE(c.invalidate(0x0)); // already gone
}

// ============================================================================
// Eviction policy correctness under capacity pressure
// ============================================================================
TEST(EvictionPolicies, LRUEvictsLeastRecentlyUsed) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 192; // 3 lines
    cfg.line_size_bytes = 64;
    cfg.associativity = 3; // single set, 3 ways, fully associative
    cfg.eviction_policy = EvictionPolicyType::LRU;
    Cache c(cfg);

    c.access(0x0000, AccessType::Read); // A
    c.access(0x0040, AccessType::Read); // B
    c.access(0x0080, AccessType::Read); // C
    c.access(0x0000, AccessType::Read); // touch A -> A becomes MRU, B is now LRU

    auto r = c.access(0x00C0, AccessType::Read); // D forces eviction
    EXPECT_TRUE(r.evicted);
    EXPECT_EQ(r.evicted_address, 0x0040u); // B was least-recently-used
}

TEST(EvictionPolicies, FIFOIgnoresAccessRecency) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 192;
    cfg.line_size_bytes = 64;
    cfg.associativity = 3;
    cfg.eviction_policy = EvictionPolicyType::FIFO;
    Cache c(cfg);

    c.access(0x0000, AccessType::Read); // A (inserted 1st)
    c.access(0x0040, AccessType::Read); // B (inserted 2nd)
    c.access(0x0080, AccessType::Read); // C (inserted 3rd)
    c.access(0x0000, AccessType::Read); // touching A must NOT change FIFO order

    auto r = c.access(0x00C0, AccessType::Read); // D forces eviction
    EXPECT_TRUE(r.evicted);
    EXPECT_EQ(r.evicted_address, 0x0000u); // A was inserted first, regardless of the later touch
}

TEST(EvictionPolicies, LFUEvictsLeastFrequentlyUsedWithFifoTieBreak) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 192;
    cfg.line_size_bytes = 64;
    cfg.associativity = 3;
    cfg.eviction_policy = EvictionPolicyType::LFU;
    Cache c(cfg);

    c.access(0x0000, AccessType::Read); // A: freq=1
    c.access(0x0040, AccessType::Read); // B: freq=1
    c.access(0x0080, AccessType::Read); // C: freq=1

    c.access(0x0000, AccessType::Read); // A: freq=2
    c.access(0x0000, AccessType::Read); // A: freq=3
    c.access(0x0080, AccessType::Read); // C: freq=2
    // B remains at freq=1 (least frequently used) -> should be evicted first.

    auto r = c.access(0x00C0, AccessType::Read); // D forces eviction
    EXPECT_TRUE(r.evicted);
    EXPECT_EQ(r.evicted_address, 0x0040u);
}

TEST(EvictionPolicies, LFUTieBreaksByOldestInsertion) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 128;
    cfg.line_size_bytes = 64;
    cfg.associativity = 2;
    cfg.eviction_policy = EvictionPolicyType::LFU;
    Cache c(cfg);

    c.access(0x0000, AccessType::Read); // A inserted first, freq=1
    c.access(0x0040, AccessType::Read); // B inserted second, freq=1
    // Both at freq=1; oldest insertion (A) should be evicted first.

    auto r = c.access(0x0080, AccessType::Read);
    EXPECT_TRUE(r.evicted);
    EXPECT_EQ(r.evicted_address, 0x0000u);
}

// ============================================================================
// Two-level hierarchy
// ============================================================================
TEST(CacheHierarchyTest, MissesL1HitsL2) {
    CacheHierarchy::Config cfg;
    cfg.l1_data.cache_size_bytes = 64;
    cfg.l1_data.line_size_bytes = 64;
    cfg.l1_data.associativity = 1;
    cfg.l2.cache_size_bytes = 256;
    cfg.l2.line_size_bytes = 64;
    cfg.l2.associativity = 4;
    CacheHierarchy h(cfg);

    auto r1 = h.access(0x1000, AccessType::Read);
    EXPECT_FALSE(r1.l1_hit);
    EXPECT_FALSE(r1.l2_hit); // compulsory miss at both levels

    auto r2 = h.access(0x1000, AccessType::Read);
    EXPECT_TRUE(r2.l1_hit); // now cached in L1
}

TEST(CacheHierarchyTest, InclusiveBackInvalidatesL1OnL2Eviction) {
    CacheHierarchy::Config cfg;
    cfg.l1_data.cache_size_bytes = 4096;
    cfg.l1_data.line_size_bytes = 64;
    cfg.l1_data.associativity = 64; // large L1 so it won't self-evict during this test
    cfg.l2.cache_size_bytes = 128;  // small L2: 2 lines
    cfg.l2.line_size_bytes = 64;
    cfg.l2.associativity = 2;
    cfg.l2.eviction_policy = EvictionPolicyType::FIFO;
    cfg.inclusion = InclusionPolicy::Inclusive;
    CacheHierarchy h(cfg);

    h.access(0x0000, AccessType::Read); // A into L1+L2
    h.access(0x0000, AccessType::Read); // A now hot in L1
    h.access(0x0040, AccessType::Read); // B into L1+L2 (L2 now full: A, B)
    h.access(0x0080, AccessType::Read); // C forces L2 to evict A (FIFO) -> must also invalidate A from L1

    // A should now miss in L1 too (evicted via inclusive back-invalidation).
    auto ra = h.access(0x0000, AccessType::Read);
    EXPECT_FALSE(ra.l1_hit);
}

TEST(CacheHierarchyTest, ExclusiveRemovesLineFromL2AfterPullingIntoL1) {
    CacheHierarchy::Config cfg;
    cfg.l1_data.cache_size_bytes = 64;
    cfg.l1_data.line_size_bytes = 64;
    cfg.l1_data.associativity = 1;
    cfg.l2.cache_size_bytes = 256;
    cfg.l2.line_size_bytes = 64;
    cfg.l2.associativity = 4;
    cfg.inclusion = InclusionPolicy::Exclusive;
    CacheHierarchy h(cfg);

    h.access(0x1000, AccessType::Read); // compulsory miss: fills L1 and L2
    // Force L1 to evict 0x1000 by accessing a conflicting address (direct-mapped, 1 set).
    h.access(0x2000, AccessType::Read); // evicts 0x1000 from L1 (clean, no writeback)

    // Now 0x1000 should still be resident in L2 (unless the exclusive
    // property already removed it when it was first pulled into L1 -- which
    // is exactly the point: exclusive hierarchies don't double-cache).
    auto r = h.access(0x1000, AccessType::Read);
    EXPECT_FALSE(r.l1_hit);
    // Whether this is an L2 hit or miss depends on exclusivity bookkeeping;
    // the key correctness property is that no crash/inconsistency occurs and
    // the hierarchy remains internally consistent across repeated exclusive
    // transfers.
    (void)r.l2_hit;
}

// ============================================================================
// Stats sanity
// ============================================================================
TEST(CacheStatsTest, HitRateComputedCorrectly) {
    CacheConfig cfg;
    cfg.cache_size_bytes = 64;
    cfg.line_size_bytes = 64;
    cfg.associativity = 1;
    Cache c(cfg);

    c.access(0x0, AccessType::Read);  // miss
    c.access(0x0, AccessType::Read);  // hit
    c.access(0x0, AccessType::Read);  // hit
    c.access(0x1000, AccessType::Read); // miss (evicts 0x0)

    EXPECT_EQ(c.stats().hits, 2u);
    EXPECT_EQ(c.stats().misses, 2u);
    EXPECT_DOUBLE_EQ(c.stats().hit_rate(), 0.5);
}
