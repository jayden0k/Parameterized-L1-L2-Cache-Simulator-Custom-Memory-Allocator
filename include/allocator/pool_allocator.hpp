// ============================================================================
// pool_allocator.hpp
//
// Memory::PoolAllocator — a custom user-space allocator that:
//   * Reserves one large arena directly from the OS via mmap (POSIX) or
//     VirtualAlloc (Windows). No libc malloc/free and no `new`/`delete` are
//     used anywhere in the implementation of the allocator itself.
//   * Sub-allocates from that arena using an explicit intrusive free list
//     (first-fit) combined with a doubly linked *physical* block list that
//     enables O(1) coalescing of adjacent free blocks on free().
//   * Supports arbitrary power-of-two alignment requests. Every block is
//     naturally aligned to `kDefaultAlignment` (64 bytes, one x86/ARM cache
//     line) by construction; alignments above that are satisfied by an
//     over-allocation + back-pointer scheme (see pool_allocator.cpp).
//   * Tracks fragmentation and metadata-overhead metrics.
//
// Thread-safety: a single mutex serializes allocate()/deallocate(). This is
// intentionally simple (a production allocator would shard by thread/size
// class) but keeps the reference implementation easy to reason about and
// safe to use from benchmarks/tests that spawn multiple threads.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>

namespace cas::memory {

// Thrown when an alignment argument is not a power of two, or is zero.
class InvalidAlignmentError final : public std::invalid_argument {
public:
    explicit InvalidAlignmentError(std::string what) : std::invalid_argument(std::move(what)) {}
};

// Thrown when the arena has insufficient contiguous free space to satisfy
// a request (the pool never grows — it is a fixed-size arena by design).
class OutOfMemoryError final : public std::bad_alloc {
public:
    explicit OutOfMemoryError(std::string what) : message_(std::move(what)) {}
    [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

// Thrown by deallocate() when the pointer does not correspond to a live
// allocation made by this allocator instance (corruption / double-free /
// foreign pointer detection via embedded header magic numbers).
class InvalidPointerError final : public std::invalid_argument {
public:
    explicit InvalidPointerError(std::string what) : std::invalid_argument(std::move(what)) {}
};

struct AllocatorStats {
    std::size_t total_pool_bytes = 0;
    std::size_t used_payload_bytes = 0;   // sum of bytes currently handed out to callers
    std::size_t free_bytes = 0;           // sum of payload capacity in free blocks
    std::size_t header_overhead_bytes = 0;// sum of BlockHeader bytes across ALL blocks (used+free)
    std::size_t largest_free_block_bytes = 0;
    std::size_t num_free_blocks = 0;
    std::uint64_t num_allocations = 0;    // lifetime count of successful allocate() calls
    std::uint64_t num_frees = 0;          // lifetime count of successful deallocate() calls
    std::uint64_t num_active_allocations = 0;

    // External fragmentation: 1 - (largest free block / total free bytes).
    // 0%  => all free memory is in one contiguous block (best case).
    // ~100% => free memory is scattered into many small, mostly-unusable blocks.
    [[nodiscard]] double external_fragmentation_pct() const noexcept;

    // Internal / metadata overhead: header bytes as a percentage of the
    // total pool size — the cost of bookkeeping itself.
    [[nodiscard]] double metadata_overhead_pct() const noexcept;

    void print(std::ostream& os) const;
};

// ----------------------------------------------------------------------------
// PoolAllocator
// ----------------------------------------------------------------------------
class PoolAllocator {
public:
    static constexpr std::size_t kDefaultAlignment = 64; // one hardware cache line

    // Reserves `pool_size_bytes` (rounded up to the OS page size) directly
    // from the operating system. Throws OutOfMemoryError if the OS mapping
    // call fails.
    explicit PoolAllocator(std::size_t pool_size_bytes);
    ~PoolAllocator();

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
    PoolAllocator(PoolAllocator&&) = delete;
    PoolAllocator& operator=(PoolAllocator&&) = delete;

    // Allocates `size` bytes aligned to `alignment` (must be a power of two;
    // defaults to the 64-byte cache-line alignment). Throws
    // InvalidAlignmentError or OutOfMemoryError on failure; never returns
    // nullptr, so callers can use the result unconditionally.
    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment = kDefaultAlignment);

    // Convenience wrapper returning a type-erased, bounds-known view.
    [[nodiscard]] std::span<std::byte> allocate_span(std::size_t size, std::size_t alignment = kDefaultAlignment);

    // Frees a pointer previously returned by allocate()/allocate_span().
    // Passing nullptr is a no-op (mirrors std::free semantics). Throws
    // InvalidPointerError if the pointer cannot be validated as a live
    // allocation from this arena.
    void deallocate(void* ptr);

    [[nodiscard]] AllocatorStats stats() const;

    // Walks the physical block list and free list checking invariants
    // (ordering, magic numbers, size bookkeeping, no overlapping blocks).
    // Intended for tests/debugging; returns false (does not throw) on the
    // first violation found.
    [[nodiscard]] bool validate_heap() const;

    [[nodiscard]] std::uintptr_t arena_base_address() const noexcept;
    [[nodiscard]] std::size_t arena_size_bytes() const noexcept { return arena_size_; }

    // Forward-declared here (public) so that free functions in the .cpp
    // translation unit can take `sizeof(BlockHeader)` etc.; the struct's
    // members are still only ever touched by PoolAllocator's own member
    // functions, and the full definition lives in pool_allocator.cpp.
    struct BlockHeader;

private:
    void* arena_base_ = nullptr;
    std::size_t arena_size_ = 0;
    BlockHeader* physical_head_ = nullptr; // first block in address order
    BlockHeader* free_list_head_ = nullptr;// intrusive singly-headed doubly linked free list
    mutable std::mutex mutex_;

    mutable std::uint64_t lifetime_allocations_ = 0;
    mutable std::uint64_t lifetime_frees_ = 0;
    mutable std::uint64_t active_allocations_ = 0;

    static void* map_arena(std::size_t size);
    static void unmap_arena(void* ptr, std::size_t size) noexcept;

    [[nodiscard]] bool owns(const void* ptr) const noexcept;
    BlockHeader* find_first_fit(std::size_t total_span_needed) noexcept;
    void split_block_if_worthwhile(BlockHeader* block, std::size_t used_extent) noexcept;
    BlockHeader* coalesce_with_neighbors(BlockHeader* block) noexcept;
    void free_list_insert_front(BlockHeader* block) noexcept;
    void free_list_remove(BlockHeader* block) noexcept;
};

} // namespace cas::memory
