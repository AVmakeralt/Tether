// support/intern.cpp — string interning implementation

#include "intern.hpp"

#include <utility>

namespace tether {

InternTable::InternTable() {
    // Reserve slot 0 for the empty string. This means StrId 0 is always
    // "" — convenient for default-initialization and for representing
    // "no name" without using kInvalidStrId.
    storage_.emplace_back();
    map_.emplace(std::string{}, StrId{0});
}

StrId InternTable::intern(std::string_view s) {
    if (auto it = map_.find(std::string{s}); it != map_.end()) {
        return it->second;
    }
    StrId id = static_cast<StrId>(storage_.size());
    std::string owned{s};
    // Take the address of the std::string inside storage_ before moving
    // it in, so the map key remains valid even after the move.
    storage_.push_back(std::move(owned));
    const std::string& stored = storage_.back();
    map_.emplace(stored, id);
    return id;
}

StrId InternTable::intern(std::string&& s) {
    if (auto it = map_.find(s); it != map_.end()) {
        return it->second;
    }
    StrId id = static_cast<StrId>(storage_.size());
    storage_.push_back(std::move(s));
    const std::string& stored = storage_.back();
    map_.emplace(stored, id);
    return id;
}

StrId InternTable::lookup(std::string_view s) const {
    auto it = map_.find(std::string{s});
    if (it == map_.end()) {
        return kInvalidStrId;
    }
    return it->second;
}

std::string_view InternTable::get(StrId id) const {
    if (id >= storage_.size()) {
        return {};
    }
    return storage_[id];
}

const std::string& InternTable::get_owned(StrId id) const {
    static const std::string empty;
    if (id >= storage_.size()) {
        return empty;
    }
    return storage_[id];
}

} // namespace tether
