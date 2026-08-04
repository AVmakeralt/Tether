// support/source.hpp — source file management and locations
//
// Every AST node and diagnostic carries a SourceRange. The
// SourceManager owns the bytes of every source file loaded into the
// compiler and translates offsets into (line, column) pairs.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tether {

struct SourceLoc {
    uint32_t file_id = 0;
    uint32_t offset  = 0;

    bool valid() const { return file_id != 0; }
};

struct SourceRange {
    SourceLoc start;
    SourceLoc end;

    static SourceRange single(SourceLoc loc) {
        SourceRange r;
        r.start = loc;
        r.end   = loc;
        return r;
    }
};

class SourceFile {
public:
    SourceFile(uint32_t file_id, std::string name, std::string content);

    uint32_t file_id() const { return file_id_; }
    const std::string& name() const { return name_; }
    std::string_view content() const { return content_; }

    // Translate an offset into a (line, column) pair. Lines and columns
    // are 1-indexed, matching compiler convention.
    struct LineCol {
        uint32_t line;
        uint32_t column;
    };
    LineCol line_col(uint32_t offset) const;

    // Return the text of line N (1-indexed). The returned view excludes
    // the trailing newline.
    std::string_view line_text(uint32_t line) const;

    // Number of lines in the file.
    std::size_t line_count() const { return line_starts_.size(); }

private:
    uint32_t              file_id_;
    std::string           name_;
    std::string           content_;
    std::vector<uint32_t> line_starts_; // offset of start of each line

    void compute_line_starts();
};

class SourceManager {
public:
    SourceManager();

    // Load a file from disk. Returns the file_id. Throws on I/O error.
    uint32_t load_file(const std::string& path);

    // Load an in-memory buffer with a synthetic name. Used for tests
    // and REPL input.
    uint32_t load_buffer(std::string name, std::string content);

    const SourceFile& file(uint32_t file_id) const;
    std::size_t file_count() const { return files_.size(); }

private:
    std::vector<SourceFile> files_;
};

} // namespace tether
