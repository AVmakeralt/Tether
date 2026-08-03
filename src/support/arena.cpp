// support/arena.cpp — bump allocator with destructor tracking

#include "arena.hpp"

#include <algorithm>
#include <cstdint>

namespace tether {

namespace {

constexpr std::size_t align_up(std::size_t value, std::size_t align) {
    return (value + align - 1) & ~(align - 1);
}

} // namespace

Arena::~Arena() {
    // Call registered destructors in reverse construction order. This
    // mirrors normal C++ semantics: later-constructed objects are
    // destroyed first.
    for (auto it = destructors_.rbegin(); it != destructors_.rend(); ++it) {
        it->fn(it->ptr);
    }
    // chunks_ freed automatically by unique_ptr.
}

void* Arena::alloc(std::size_t size, std::size_t align) {
    if (size == 0) {
        alignas(max_align_t) static char dummy;
        return &dummy;
    }

    for (std::size_t i = current_chunk_; i < chunks_.size(); ++i) {
        Chunk& c = chunks_[i];
        std::size_t aligned = align_up(c.used, align);
        if (aligned + size <= c.size) {
            void* p = c.data.get() + aligned;
            c.used  = aligned + size;
            current_chunk_ = i;
            return p;
        }
    }

    grow(size + align);
    Chunk& c = chunks_.back();
    std::size_t aligned = align_up(c.used, align);
    void* p = c.data.get() + aligned;
    c.used  = aligned + size;
    current_chunk_ = chunks_.size() - 1;
    return p;
}

void Arena::grow(std::size_t needed) {
    std::size_t chunk_size = kDefaultChunkSize;
    while (chunk_size < needed) {
        chunk_size *= 2;
    }
    Chunk c;
    c.data = std::unique_ptr<char[]>(new char[chunk_size]);
    c.size = chunk_size;
    c.used = 0;
    chunks_.push_back(std::move(c));
}

std::size_t Arena::bytes_allocated() const {
    std::size_t total = 0;
    for (const Chunk& c : chunks_) {
        total += c.used;
    }
    return total;
}

} // namespace tether
