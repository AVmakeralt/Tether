// diagnostics/diagnostics.hpp — source-located error reporting
//
// The DiagnosticEmitter collects errors and warnings during lexing,
// parsing, name resolution, type checking, and borrow checking. It
// never aborts compilation on the first error — it reports as many
// errors as it can find, so the user can fix several issues per build.
//
// Diagnostics are plain text for v0.1. A future version may emit
// structured JSON for IDE consumption.

#pragma once

#include "support/source.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tether {

enum class DiagnosticLevel : uint8_t {
    Error,
    Warning,
    Note,
};

struct Diagnostic {
    DiagnosticLevel level;
    SourceRange     range;
    std::string     message;
    // Optional notes — for example, "previous definition was here".
    std::vector<std::pair<SourceRange, std::string>> notes;
};

class DiagnosticEmitter {
public:
    DiagnosticEmitter() = default;

    void error(SourceRange range, std::string message) {
        add(DiagnosticLevel::Error, range, std::move(message));
    }
    void warning(SourceRange range, std::string message) {
        add(DiagnosticLevel::Warning, range, std::move(message));
    }
    void note(SourceRange range, std::string message) {
        add(DiagnosticLevel::Note, range, std::move(message));
    }

    void add(DiagnosticLevel level, SourceRange range, std::string message) {
        diagnostics_.push_back({level, range, std::move(message), {}});
    }

    void append_note(SourceRange range, std::string message) {
        if (!diagnostics_.empty()) {
            diagnostics_.back().notes.emplace_back(range, std::move(message));
        }
    }

    bool has_errors() const {
        for (const auto& d : diagnostics_) {
            if (d.level == DiagnosticLevel::Error) return true;
        }
        return false;
    }

    std::size_t error_count() const {
        std::size_t n = 0;
        for (const auto& d : diagnostics_) {
            if (d.level == DiagnosticLevel::Error) ++n;
        }
        return n;
    }

    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // Render all diagnostics as text, with file/line context.
    std::string render(const SourceManager& sm) const;

    // Clear all diagnostics.
    void clear() { diagnostics_.clear(); }

private:
    std::vector<Diagnostic> diagnostics_;
};

} // namespace tether
