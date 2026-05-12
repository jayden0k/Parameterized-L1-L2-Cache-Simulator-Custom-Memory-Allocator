#include "allocator/pool_allocator.hpp"

#include <bit>
#include <cstring>
#include <format>
#include <memory>
#include <ostream>

#if defined(_WIN32)
#  define CAS_PLATFORM_WINDOWS 1
#  include <windows.h>
#else
#  define CAS_PLATFORM_POSIX 1
#  include <sys/mman.h>
#  include <unistd.h>
#endif

namespace cas::memory {

// ============================================================================
// AllocatorStats
// ============================================================================
double AllocatorStats::external_fragmentation_pct() const noexcept {
    if (free_bytes == 0) return 0.0;
    return (1.0 - static_cast<double>(largest_free_block_bytes) / static_cast<double>(free_bytes)) * 100.0;
}

double AllocatorStats::metadata_overhead_pct() const noexcept {
    if (total_pool_bytes == 0) return 0.0;
    return static_cast<double>(header_overhead_bytes) / static_cast<double>(total_pool_bytes) * 100.0;
}

void AllocatorStats::print(std::ostream& os) const {
    os << "--- PoolAllocator stats ---\n"
       << "  total_pool_bytes=" << total_pool_bytes << " used_payload_bytes=" << used_payload_bytes
       << " free_bytes=" << free_bytes << "\n"
       << "  header_overhead_bytes=" << header_overhead_bytes << " num_free_blocks=" << num_free_blocks
       << " largest_free_block_bytes=" << largest_free_block_bytes << "\n"
       << std::format("  external_fragmentation={:.2f}% metadata_overhead={:.2f}%\n",
                       external_fragmentation_pct(), metadata_overhead_pct())
       << "  lifetime_allocations=" << num_allocations << " lifetime_frees=" << num_frees
       << " active_allocations=" << num_active_allocations << "\n";
}

// ============================================================================
// BlockHeader
//
// Embedded, in-band header placed immediately before every block's payload
// region (whether the block is free or in use). alignas(64) guarantees:
//   (a) sizeof(BlockHeader) is itself a multiple of 64, so
//   (b) if a block's starting address is 64-byte aligned, the payload
//       address immediately following its header is ALSO 64-byte aligned
//       "for free" — this is how the allocator satisfies the default
//       hardware-cache-line alignment guarantee without any extra padding
//       logic on the common path.
//
// Two disjoint linked structures live in the same header:
//   * prev_phys/next_phys — the *physical* (address-order) block list,
//     spanning the entire arena, used for O(1) coalescing: to merge a freed
//     block with its neighbors we simply follow these pointers rather than
//     searching.
//   * prev_free/next_free — the *free list*, a doubly linked list threading
//     together only the currently-free blocks (LIFO insertion), used to
//     make find-first-fit allocation touch only free blocks instead of the
//     entire arena.
//
// `magic` is a corruption / double-free / foreign-pointer guard: on
// deallocate() we refuse to trust a candidate header unless its magic
// value matches the expected "in use" sentinel.
// ============================================================================
struct alignas(64) PoolAllocator::BlockHeader {
    static constexpr std::uint64_t kMagicUsed = 0xCA5EA110'C0FFEEULL;
    static constexpr std::uint64_t kMagicFree = 0xF4EE0B10'CACA0000ULL;

    std::size_t size = 0;   // usable payload capacity in bytes (NOT including this header)
    bool is_free = true;
    bool is_aligned_alloc = false; // true => allocated via the over-alignment back-pointer path
    BlockHeader* prev_phys = nullptr;
    BlockHeader* next_phys = nullptr;
    BlockHeader* prev_free = nullptr;
    BlockHeader* next_free = nullptr;
    std::uint64_t magic = kMagicFree;

    [[nodiscard]] std::byte* payload() noexcept {
        return reinterpret_cast<std::byte*>(this) + sizeof(BlockHeader);
    }
};

namespace {
constexpr std::size_t kHeaderSize = sizeof(PoolAllocator::BlockHeader);
static_assert(kHeaderSize % 64 == 0, "BlockHeader must remain a multiple of the cache-line size");
constexpr std::size_t kMinSplitPayload = 32; // don't split off slivers smaller than this

[[nodiscard]] constexpr bool is_power_of_two(std::size_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

[[nodiscard]] constexpr std::uintptr_t align_up(std::uintptr_t value, std::size_t alignment) noexcept {
    const std::uintptr_t mask = static_cast<std::uintptr_t>(alignment) - 1;
    return (value + mask) & ~mask;
}
} // namespace

// ============================================================================
// OS arena mapping
// ============================================================================
void* PoolAllocator::map_arena(std::size_t size) {
#if defined(CAS_PLATFORM_POSIX)
    void* mem = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        throw OutOfMemoryError(std::format("mmap failed to reserve {} bytes (errno={})", size, errno));
    }
    return mem;
#elif defined(CAS_PLATFORM_WINDOWS)
    void* mem = ::VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (mem == nullptr) {
        throw OutOfMemoryError(std::format("VirtualAlloc failed to reserve {} bytes (GetLastError={})",
                                            size, static_cast<unsigned long>(GetLastError())));
    }
    return mem;
#else
#  error "Unsupported platform: no OS arena-mapping primitive available"
#endif
}

void PoolAllocator::unmap_arena(void* ptr, std::size_t size) noexcept {
    if (ptr == nullptr) return;
#if defined(CAS_PLATFORM_POSIX)
    ::munmap(ptr, size);
#elif defined(CAS_PLATFORM_WINDOWS)
    (void)size;
    ::VirtualFree(ptr, 0, MEM_RELEASE);
#endif
}

namespace {
[[nodiscard]] std::size_t query_page_size() noexcept {
#if defined(CAS_PLATFORM_POSIX)
    long ps = ::sysconf(_SC_PAGESIZE);
    return ps > 0 ? static_cast<std::size_t>(ps) : 4096;
#else
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#endif
}
} // namespace

// ============================================================================
// Construction / destruction
// ============================================================================
PoolAllocator::PoolAllocator(std::size_t pool_size_bytes) {
    if (pool_size_bytes == 0) {
        throw std::invalid_argument("PoolAllocator: pool_size_bytes must be > 0");
    }

    const std::size_t page_size = query_page_size();
    arena_size_ = align_up(pool_size_bytes, page_size);
    if (arena_size_ < kHeaderSize) {
        arena_size_ = align_up(kHeaderSize, page_size);
    }

    arena_base_ = map_arena(arena_size_);

    // Carve the entire arena into a single free block spanning it.
    auto* block = static_cast<BlockHeader*>(arena_base_);
    // Placement-construct the header in place. This is NOT a heap
    // allocation (`new`): the storage already exists inside the mmap'd
    // arena, we are only running the constructor over it, which is the
    // idiomatic, allocation-free way to turn raw OS memory into a typed
    // object per the C++ object model.
    ::new (block) BlockHeader{};
    block->size = arena_size_ - kHeaderSize;
    block->is_free = true;
    block->is_aligned_alloc = false;
    block->prev_phys = nullptr;
    block->next_phys = nullptr;
    block->magic = BlockHeader::kMagicFree;

    physical_head_ = block;
    free_list_head_ = nullptr;
    free_list_insert_front(block);
}

PoolAllocator::~PoolAllocator() { unmap_arena(arena_base_, arena_size_); }

std::uintptr_t PoolAllocator::arena_base_address() const noexcept {
    return reinterpret_cast<std::uintptr_t>(arena_base_);
}

bool PoolAllocator::owns(const void* ptr) const noexcept {
    const auto p = reinterpret_cast<std::uintptr_t>(ptr);
    const auto base = reinterpret_cast<std::uintptr_t>(arena_base_);
    return p >= base && p < base + arena_size_;
}

// ============================================================================
// Free-list management
// ============================================================================
void PoolAllocator::free_list_insert_front(BlockHeader* block) noexcept {
    block->prev_free = nullptr;
    block->next_free = free_list_head_;
    if (free_list_head_ != nullptr) {
        free_list_head_->prev_free = block;
    }
    free_list_head_ = block;
}

void PoolAllocator::free_list_remove(BlockHeader* block) noexcept {
    if (block->prev_free != nullptr) {
        block->prev_free->next_free = block->next_free;
    } else {
        free_list_head_ = block->next_free;
    }
    if (block->next_free != nullptr) {
        block->next_free->prev_free = block->prev_free;
    }
    block->prev_free = nullptr;
    block->next_free = nullptr;
}

PoolAllocator::BlockHeader* PoolAllocator::find_first_fit(std::size_t total_span_needed) noexcept {
    for (BlockHeader* b = free_list_head_; b != nullptr; b = b->next_free) {
        if (b->size >= total_span_needed) {
            return b;
        }
    }
    return nullptr;
}

// Splits `block` (already removed from the free list by the caller) so that
// only `used_extent` bytes of its payload remain assigned to it; if the
// remainder is large enough to be useful, a brand-new free block header is
// carved out of the tail and linked into both the physical and free lists.
//
// INVARIANT: every BlockHeader in the arena starts at an address that is a
// multiple of kDefaultAlignment (64). This is what lets the default-
// alignment fast path in allocate() work "for free" for every block,
// including ones created by splitting. To preserve it, the split point
// itself (i.e. `used_extent`, measured from the start of the payload) is
// always rounded UP to a multiple of kDefaultAlignment before a new block
// boundary is introduced — since block->payload() is already 64-aligned by
// this same invariant, block->payload() + round_up(used_extent, 64) is too.
void PoolAllocator::split_block_if_worthwhile(BlockHeader* block, std::size_t used_extent) noexcept {
    if (block->size < used_extent) {
        used_extent = block->size; // defensive clamp; should not happen
    }

    const std::size_t aligned_used_extent =
        static_cast<std::size_t>(align_up(used_extent, kDefaultAlignment));
    if (aligned_used_extent >= block->size) {
        return; // rounding to preserve the alignment invariant leaves no useful remainder
    }
    used_extent = aligned_used_extent;

    const std::size_t remainder = block->size - used_extent;
    if (remainder < kHeaderSize + kMinSplitPayload) {
        return; // remainder too small to be worth the header overhead
    }

    auto* new_block = reinterpret_cast<BlockHeader*>(block->payload() + used_extent);
    ::new (new_block) BlockHeader{};
    new_block->size = remainder - kHeaderSize;
    new_block->is_free = true;
    new_block->is_aligned_alloc = false;
    new_block->magic = BlockHeader::kMagicFree;

    // Splice new_block into the physical list right after `block`.
    new_block->prev_phys = block;
    new_block->next_phys = block->next_phys;
    if (block->next_phys != nullptr) {
        block->next_phys->prev_phys = new_block;
    }
    block->next_phys = new_block;

    block->size = used_extent;

    free_list_insert_front(new_block);
}

// Merges `block` with its physically-adjacent neighbors if they are also
// free. O(1) because prev_phys/next_phys give direct access without any
// search. Returns the (possibly merged) surviving block.
PoolAllocator::BlockHeader* PoolAllocator::coalesce_with_neighbors(BlockHeader* block) noexcept {
    // Merge with next neighbor first (absorb its header+payload into us).
    if (BlockHeader* nxt = block->next_phys; nxt != nullptr && nxt->is_free) {
        free_list_remove(nxt);
        block->size += kHeaderSize + nxt->size;
        block->next_phys = nxt->next_phys;
        if (nxt->next_phys != nullptr) {
            nxt->next_phys->prev_phys = block;
        }
        // nxt's header memory is now silently absorbed as part of block's
        // payload; no destructor call is needed since BlockHeader is
        // trivially destructible (no owning resources beyond raw pointers
        // into the same arena).
    }

    // Merge with previous neighbor (we get absorbed into it).
    if (BlockHeader* prv = block->prev_phys; prv != nullptr && prv->is_free) {
        free_list_remove(prv);
        prv->size += kHeaderSize + block->size;
        prv->next_phys = block->next_phys;
        if (block->next_phys != nullptr) {
            block->next_phys->prev_phys = prv;
        }
        block = prv;
    }

    return block;
}

// ============================================================================
// allocate()
// ============================================================================
void* PoolAllocator::allocate(std::size_t size, std::size_t alignment) {
    if (!is_power_of_two(alignment)) {
        throw InvalidAlignmentError(std::format("PoolAllocator::allocate: alignment {} is not a power of two",
                                                 alignment));
    }
    if (size == 0) {
        size = 1; // never hand out a zero-sized region; mirrors malloc(0) returning a unique valid pointer
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // --- Fast path: alignment satisfied automatically by the 64B-aligned
    //     header/payload invariant (covers 1,2,4,8,16,32,64-byte requests).
    if (alignment <= kDefaultAlignment) {
        const std::size_t needed = align_up(size, alignof(std::max_align_t));

        BlockHeader* block = find_first_fit(needed);
        if (block == nullptr) {
            throw OutOfMemoryError(
                std::format("PoolAllocator: out of memory allocating {} bytes (align {})", size, alignment));
        }

        free_list_remove(block);
        split_block_if_worthwhile(block, needed);

        block->is_free = false;
        block->is_aligned_alloc = false;
        block->magic = BlockHeader::kMagicUsed;

        ++lifetime_allocations_;
        ++active_allocations_;
        return block->payload();
    }

    // --- Slow path: over-alignment beyond the default cache-line guarantee.
    //     We reserve extra headroom for (a) the worst-case alignment slack
    //     and (b) an 8-byte back-pointer slot placed immediately before the
    //     aligned payload address so deallocate() can recover the true
    //     BlockHeader*, which does not sit directly before the pointer we
    //     hand back to the caller in this path.
    //
    //     The actual alignment computation is delegated to std::align
    //     (<memory>) rather than hand-rolled bit-masking: given a mutable
    //     (pointer, remaining-space) pair it advances the pointer to the
    //     next address satisfying `alignment` and shrinks `space`
    //     accordingly, failing (returning nullptr) if it would not fit —
    //     which we treat as an internal invariant violation since
    //     `span_needed` was already sized to guarantee a fit.
    const std::size_t span_needed = size + alignment - 1 + sizeof(void*);

    BlockHeader* block = find_first_fit(span_needed);
    if (block == nullptr) {
        throw OutOfMemoryError(std::format(
            "PoolAllocator: out of memory allocating {} bytes (align {})", size, alignment));
    }
    free_list_remove(block);

    std::byte* raw_payload = block->payload();
    void* align_cursor = raw_payload + sizeof(void*); // reserve room for the back-pointer slot first
    std::size_t space_available = block->size - sizeof(void*);

    void* aligned_result = std::align(alignment, size, align_cursor, space_available);
    if (aligned_result == nullptr) {
        // Should be unreachable given span_needed's sizing; fail loudly
        // rather than silently corrupting the heap if it ever is.
        throw OutOfMemoryError(std::format(
            "PoolAllocator: std::align could not satisfy alignment {} for {} bytes "
            "within a block sized for it (internal invariant violation)",
            alignment, size));
    }
    auto* aligned_payload = static_cast<std::byte*>(aligned_result);

    const std::size_t used_extent =
        static_cast<std::size_t>((aligned_payload + size) - raw_payload);

    split_block_if_worthwhile(block, used_extent);

    // Store the back-pointer to the real header immediately before the
    // aligned pointer we are about to return.
    std::memcpy(aligned_payload - sizeof(void*), &block, sizeof(void*));

    block->is_free = false;
    block->is_aligned_alloc = true;
    block->magic = BlockHeader::kMagicUsed;

    ++lifetime_allocations_;
    ++active_allocations_;
    return aligned_payload;
}

std::span<std::byte> PoolAllocator::allocate_span(std::size_t size, std::size_t alignment) {
    void* ptr = allocate(size, alignment);
    return std::span<std::byte>(static_cast<std::byte*>(ptr), size);
}

// ============================================================================
// deallocate()
// ============================================================================
void PoolAllocator::deallocate(void* ptr) {
    if (ptr == nullptr) return;

    std::lock_guard<std::mutex> lock(mutex_);

    if (!owns(ptr)) {
        throw InvalidPointerError("PoolAllocator::deallocate: pointer does not belong to this arena");
    }

    BlockHeader* header = nullptr;

    // Candidate 1: standard (non-over-aligned) path — header sits exactly
    // kHeaderSize bytes before the payload.
    auto* std_candidate =
        reinterpret_cast<BlockHeader*>(reinterpret_cast<std::byte*>(ptr) - kHeaderSize);
    if (owns(std_candidate) &&
        std_candidate->magic == BlockHeader::kMagicUsed &&
        !std_candidate->is_aligned_alloc) {
        header = std_candidate;
    } else {
        // Candidate 2: over-aligned path — a back-pointer to the real
        // header is stored in the 8 bytes immediately preceding `ptr`.
        auto* backptr_slot = reinterpret_cast<std::byte*>(ptr) - sizeof(void*);
        if (owns(backptr_slot) && owns(backptr_slot + sizeof(void*) - 1)) {
            BlockHeader* candidate = nullptr;
            std::memcpy(&candidate, backptr_slot, sizeof(void*));
            if (owns(candidate) && candidate->magic == BlockHeader::kMagicUsed && candidate->is_aligned_alloc) {
                header = candidate;
            }
        }
    }

    if (header == nullptr) {
        throw InvalidPointerError(
            "PoolAllocator::deallocate: pointer is not a live allocation (corrupted, foreign, or double-freed)");
    }

    header->is_free = true;
    header->magic = BlockHeader::kMagicFree;
    header = coalesce_with_neighbors(header);
    free_list_insert_front(header);

    ++lifetime_frees_;
    --active_allocations_;
}

// ============================================================================
// Introspection
// ============================================================================
AllocatorStats PoolAllocator::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    AllocatorStats s;
    s.total_pool_bytes = arena_size_;
    s.num_allocations = lifetime_allocations_;
    s.num_frees = lifetime_frees_;
    s.num_active_allocations = active_allocations_;

    for (const BlockHeader* b = physical_head_; b != nullptr; b = b->next_phys) {
        s.header_overhead_bytes += kHeaderSize;
        if (b->is_free) {
            s.free_bytes += b->size;
            s.num_free_blocks += 1;
            s.largest_free_block_bytes = std::max(s.largest_free_block_bytes, b->size);
        } else {
            s.used_payload_bytes += b->size;
        }
    }
    return s;
}

bool PoolAllocator::validate_heap() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::size_t walked_bytes = 0;
    const BlockHeader* prev = nullptr;
    for (const BlockHeader* b = physical_head_; b != nullptr; b = b->next_phys) {
        if (!owns(b)) return false;
        if (b->prev_phys != prev) return false;
        if (b->magic != (b->is_free ? BlockHeader::kMagicFree : BlockHeader::kMagicUsed)) return false;
        // No two adjacent free blocks should ever survive uncoalesced.
        if (prev != nullptr && prev->is_free && b->is_free) return false;

        walked_bytes += kHeaderSize + b->size;
        prev = b;
    }
    return walked_bytes == arena_size_;
}

} // namespace cas::memory
