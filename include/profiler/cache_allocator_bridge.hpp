// ============================================================================
// cache_allocator_bridge.hpp
//
// Profiler::CacheAllocatorBridge wires Memory::PoolAllocator allocation
// traffic directly into the Cache simulator's address trace input, so that
// the cache-friendliness of the allocator itself (header layout, alignment
// padding, coalescing-induced address reuse, etc.) can be observed under a
// real cache hierarchy in real time — no separate trace file required.
//
// Model:
//   * allocate(size, alignment) forwards to PoolAllocator::allocate(), then
//     simulates a WRITE burst covering every cache line touched by the
//     returned [ptr, ptr+size) region (the conservative assumption that a
//     freshly allocated object is written to before being read, mirroring
//     zero-initialization / constructor-run behavior).
//   * deallocate(ptr, size_hint) simulates a READ burst over the same
//     region (approximating destructor / bookkeeping touches) before
//     forwarding to PoolAllocator::deallocate(). `size_hint` lets the
//     caller supply the original allocation size for accurate line
//     coverage; if omitted, only the single line containing `ptr` is
//     touched.
//
// Every simulated line touch is recorded so callers can inspect hit/miss
// behavior per allocation event for analysis or visualization.
// ============================================================================
#pragma once

#include "allocator/pool_allocator.hpp"
#include "cache/cache.hpp"

#include <cstdint>
#include <ostream>
#include <vector>

namespace cas::profiler {

struct BridgeLineTouch {
    std::uint64_t address = 0;
    bool is_allocation_event = true; // true = from allocate(), false = from deallocate()
    cache::AccessType access_type = cache::AccessType::Write;
    cache::CacheHierarchy::HierarchyAccessResult result{};
};

struct BridgeSummary {
    std::uint64_t total_allocation_calls = 0;
    std::uint64_t total_free_calls = 0;
    std::uint64_t total_line_touches = 0;
    std::uint64_t l1_hits = 0;
    std::uint64_t l2_hits = 0;
    std::uint64_t full_misses = 0; // missed both L1 and L2
    [[nodiscard]] double l1_hit_rate() const noexcept;
    [[nodiscard]] double overall_hit_rate() const noexcept; // hit at L1 OR L2
    void print(std::ostream& os) const;
};

class CacheAllocatorBridge {
public:
    CacheAllocatorBridge(memory::PoolAllocator& allocator, cache::CacheHierarchy& hierarchy);

    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment = memory::PoolAllocator::kDefaultAlignment);
    void deallocate(void* ptr, std::size_t size_hint = 0);

    [[nodiscard]] const std::vector<BridgeLineTouch>& history() const noexcept { return history_; }
    [[nodiscard]] BridgeSummary summary() const;

private:
    memory::PoolAllocator& allocator_;
    cache::CacheHierarchy& hierarchy_;
    std::vector<BridgeLineTouch> history_;
    std::uint64_t allocation_calls_ = 0;
    std::uint64_t free_calls_ = 0;

    void touch_lines(std::uint64_t base_address, std::size_t size, bool is_allocation_event,
                      cache::AccessType type);
};

} // namespace cas::profiler
