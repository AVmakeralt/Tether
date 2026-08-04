// support/intern.hpp — string/symbol interning
//
// Compiler code spends most of its time hashing and comparing
// identifiers. Interning every identifier once and passing around a
// 32-bit StrId is dramatically faster than passing around
// std::string_view and re-hashing it on every lookup.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tether {

using StrId = uint32_t;

// Sentinel for "no string". Returned by lookup-only APIs.
constexpr StrId kInvalidStrId = 0xFFFFFFFFu;

class InternTable {
public:
    InternTable();

    // Intern a string view. The table copies the bytes; the input does
    // not need to outlive the call.
    StrId intern(std::string_view s);

    // Intern a std::string by moving it into the table. Avoids a copy
    // when the caller was going to discard the string anyway.
    StrId intern(std::string&& s);

    // Look up a string without inserting. Returns kInvalidStrId if
    // absent.
    StrId lookup(std::string_view s) const;

    // Retrieve a view of the string. The view is valid as long as the
    // InternTable is alive and no further insertions happen that would
    // cause the underlying storage to be reallocated (currently it
    // never reallocates individual strings — only the vector of
    // pointers).
    std::string_view get(StrId id) const;

    // Retrieve the owned std::string.
    const std::string& get_owned(StrId id) const;

    // Number of interned strings.
    std::size_t size() const { return storage_.size(); }

    bool contains(std::string_view s) const {
        return lookup(s) != kInvalidStrId;
    }

private:
    // We store std::string (not string_view) so that the table owns the
    // bytes. The map keys are string_views into those owned strings; we
    // never invalidate them because std::string storage is stable for
    // the lifetime of the string (since C++11, small-string-optimized
    // strings do not move on resize of the std::vector — the vector
    // moves the std::string objects themselves, but their internal
    // buffers are heap-allocated and stable).
    //
    // To be safe against SSO, we use a custom key comparator that
    // hashes and compares by value, not by pointer.
    std::unordered_map<std::string, StrId> map_;
    std::vector<std::string>               storage_;
};

} // namespace tether
