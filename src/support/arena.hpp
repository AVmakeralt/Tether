// support/arena.hpp — bump allocator with destructor tracking
//
// Design rationale:
//   The Tether spec mandates that "allocators own memory, not objects".
//   AST and IR nodes are arena-allocated; freeing the arena frees every
//   node in one shot.
//
//   Two kinds of node are placed in arenas:
//
//   1. Trivially-destructible nodes (e.g. the planned SSA IR, where
//      vectors use an arena-aware allocator and so own no external
//      storage). For these, no destructor is registered; the arena
//      just bulk-frees its chunks.
//
//   2. Non-trivially-destructible nodes (e.g. AST nodes that own
//      std::vector / std::string fields). For these, the arena records
//      a (pointer, destructor-function) pair and calls them in reverse
//      construction order when the arena is destroyed.
//
//   This is the same pattern LLVM's BumpPtrAllocator uses. The
//   "million-object free loop" the spec warns against is about
//   *recursive* destructors walking a tree — flat destruction of N
//   independent vectors is O(N) but not recursive, and only happens
//   once per arena, not per pass.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace tether {

class Arena {
public:
    Arena() = default;
    ~Arena();

    Arena(const Arena&)            = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&)                 = default;
    Arena& operator=(Arena&&)      = default;

    // Allocate raw bytes with alignment.
    void* alloc(std::size_t size, std::size_t align);

    // Construct an object in the arena. Returns a pointer that is
    // valid until the Arena is destroyed. If T has a non-trivial
    // destructor, the destructor is registered and called when the
    // Arena is destroyed (in reverse construction order).
    template <typename T, typename... Args>
    T* construct(Args&&... args) {
        void* p = alloc(sizeof(T), alignof(T));
        new (p) T(std::forward<Args>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            destructors_.push_back({p, &destructor_fn<T>});
        }
        return static_cast<T*>(p);
    }

    // Construct an array of trivially-constructible objects.
    // (Non-trivially-destructible arrays are not supported — use a
    // std::vector with an arena-aware allocator if you need that.)
    template <typename T>
    T* construct_array(std::size_t count) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "Arena-allocated arrays must be trivially destructible.");
        void* p = alloc(sizeof(T) * count, alignof(T));
        return new (p) T[count]{};
    }

    std::size_t bytes_allocated() const;
    std::size_t chunk_count() const { return chunks_.size(); }
    std::size_t destructor_count() const { return destructors_.size(); }

private:
    struct Chunk {
        std::unique_ptr<char[]> data;
        std::size_t size = 0;
        std::size_t used = 0;
    };

    struct Destructor {
        void*  ptr;
        void (*fn)(void*);
    };

    template <typename T>
    static void destructor_fn(void* p) {
        static_cast<T*>(p)->~T();
    }

    std::vector<Chunk>      chunks_;
    std::vector<Destructor> destructors_;
    std::size_t             current_chunk_ = 0;

    static constexpr std::size_t kDefaultChunkSize = 64 * 1024;

    void grow(std::size_t needed);
};

} // namespace tether
