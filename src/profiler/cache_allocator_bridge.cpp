#include "profiler/cache_allocator_bridge.hpp"

#include <format>

namespace cas::profiler {

double BridgeSummary::l1_hit_rate() const noexcept {
    return total_line_touches == 0 ? 0.0 : static_cast<double>(l1_hits) / static_cast<double>(total_line_touches);
}

double BridgeSummary::overall_hit_rate() const noexcept {
    if (total_line_touches == 0) return 0.0;
    return static_cast<double>(l1_hits + l2_hits) / static_cast<double>(total_line_touches);
}

void BridgeSummary::print(std::ostream& os) const {
    os << "==== CacheAllocatorBridge Summary ====\n"
       << "  allocation_calls=" << total_allocation_calls << " free_calls=" << total_free_calls << "\n"
       << "  line_touches=" << total_line_touches << " l1_hits=" << l1_hits << " l2_hits=" << l2_hits
       << " full_misses=" << full_misses << "\n"
       << std::format("  l1_hit_rate={:.2f}% overall_hit_rate={:.2f}%\n", l1_hit_rate() * 100.0,
                       overall_hit_rate() * 100.0);
}

CacheAllocatorBridge::CacheAllocatorBridge(memory::PoolAllocator& allocator, cache::CacheHierarchy& hierarchy)
    : allocator_(allocator), hierarchy_(hierarchy) {}

void CacheAllocatorBridge::touch_lines(std::uint64_t base_address, std::size_t size, bool is_allocation_event,
                                        cache::AccessType type) {
    if (size == 0) size = 1;

    // Discover the L1 line size directly from the hierarchy so we touch
    // exactly one simulated access per cache line spanned by [base, base+size).
    const std::size_t line_size = hierarchy_.l1_data().config().line_size_bytes;
    const std::uint64_t line_mask = ~(static_cast<std::uint64_t>(line_size) - 1);

    std::uint64_t addr = base_address & line_mask;
    const std::uint64_t end = base_address + size;

    while (addr < end) {
        cache::CacheHierarchy::HierarchyAccessResult result = hierarchy_.access(addr, type, line_size);
        history_.push_back(BridgeLineTouch{
            .address = addr, .is_allocation_event = is_allocation_event, .access_type = type, .result = result});
        addr += line_size;
    }
}

void* CacheAllocatorBridge::allocate(std::size_t size, std::size_t alignment) {
    void* ptr = allocator_.allocate(size, alignment);
    ++allocation_calls_;
    touch_lines(reinterpret_cast<std::uint64_t>(ptr), size, /*is_allocation_event=*/true, cache::AccessType::Write);
    return ptr;
}

void CacheAllocatorBridge::deallocate(void* ptr, std::size_t size_hint) {
    if (ptr == nullptr) return;
    ++free_calls_;
    const std::size_t effective_size = size_hint == 0 ? 1 : size_hint;
    touch_lines(reinterpret_cast<std::uint64_t>(ptr), effective_size, /*is_allocation_event=*/false,
                cache::AccessType::Read);
    allocator_.deallocate(ptr);
}

BridgeSummary CacheAllocatorBridge::summary() const {
    BridgeSummary s;
    s.total_allocation_calls = allocation_calls_;
    s.total_free_calls = free_calls_;

    for (const auto& touch : history_) {
        ++s.total_line_touches;
        if (touch.result.l1_hit) {
            ++s.l1_hits;
        } else if (touch.result.l2_hit) {
            ++s.l2_hits;
        } else {
            ++s.full_misses;
        }
    }
    return s;
}

} // namespace cas::profiler
