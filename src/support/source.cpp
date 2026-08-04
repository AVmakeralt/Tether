// support/source.cpp — source file management implementation

#include "source.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace tether {

SourceFile::SourceFile(uint32_t file_id, std::string name, std::string content)
    : file_id_(file_id), name_(std::move(name)), content_(std::move(content)) {
    compute_line_starts();
}

void SourceFile::compute_line_starts() {
    line_starts_.clear();
    line_starts_.push_back(0);
    for (std::size_t i = 0; i < content_.size(); ++i) {
        if (content_[i] == '\n') {
            line_starts_.push_back(static_cast<uint32_t>(i + 1));
        }
    }
}

SourceFile::LineCol SourceFile::line_col(uint32_t offset) const {
    // Binary search for the largest line_start <= offset.
    if (line_starts_.empty()) {
        return {1, 1};
    }
    std::size_t lo = 0;
    std::size_t hi = line_starts_.size();
    while (lo + 1 < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        if (line_starts_[mid] <= offset) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    uint32_t line   = static_cast<uint32_t>(lo + 1); // 1-indexed
    uint32_t column = offset - line_starts_[lo] + 1; // 1-indexed
    return {line, column};
}

std::string_view SourceFile::line_text(uint32_t line) const {
    if (line == 0 || line > line_starts_.size()) {
        return {};
    }
    uint32_t start = line_starts_[line - 1];
    uint32_t end   = (line < line_starts_.size())
                         ? line_starts_[line]
                         : static_cast<uint32_t>(content_.size());
    // Trim trailing \r and \n.
    while (end > start &&
           (content_[end - 1] == '\n' || content_[end - 1] == '\r')) {
        --end;
    }
    return std::string_view(content_.data() + start, end - start);
}

SourceManager::SourceManager() {
    // Reserve file_id 0 for "no file" / synthetic / unknown.
    files_.emplace_back(0, "<none>", std::string{});
}

uint32_t SourceManager::load_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::string msg = "tether: cannot open '";
        msg += path;
        msg += "'";
        throw std::runtime_error(msg);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return load_buffer(path, ss.str());
}

uint32_t SourceManager::load_buffer(std::string name, std::string content) {
    uint32_t id = static_cast<uint32_t>(files_.size());
    files_.emplace_back(id, std::move(name), std::move(content));
    return id;
}

const SourceFile& SourceManager::file(uint32_t file_id) const {
    if (file_id >= files_.size()) {
        throw std::runtime_error("tether: invalid file_id");
    }
    return files_[file_id];
}

} // namespace tether
