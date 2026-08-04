// diagnostics/diagnostics.cpp — rendering

#include "diagnostics.hpp"

#include <sstream>
#include <string>

namespace tether {

namespace {

const char* level_prefix(DiagnosticLevel l) {
    switch (l) {
        case DiagnosticLevel::Error:   return "error";
        case DiagnosticLevel::Warning: return "warning";
        case DiagnosticLevel::Note:    return "note";
    }
    return "diagnostic";
}

} // namespace

std::string DiagnosticEmitter::render(const SourceManager& sm) const {
    std::ostringstream out;
    for (const Diagnostic& d : diagnostics_) {
        const SourceFile& f = sm.file(d.range.start.file_id);
        auto [line, col] = f.line_col(d.range.start.offset);
        out << f.name() << ':' << line << ':' << col << ": "
            << level_prefix(d.level) << ": " << d.message << '\n';

        // Print the offending line with a caret.
        std::string_view text = f.line_text(line);
        if (!text.empty()) {
            out << "    | " << text << '\n';
            out << "    | ";
            for (uint32_t i = 1; i < col; ++i) out << ' ';
            out << '^';
            uint32_t end_offset = d.range.end.offset;
            auto end_lc = f.line_col(end_offset);
            if (end_lc.line == line && end_offset > d.range.start.offset) {
                uint32_t span = end_offset - d.range.start.offset;
                for (uint32_t i = 1; i < span; ++i) out << '~';
            }
            out << '\n';
        }

        for (const auto& [range, msg] : d.notes) {
            const SourceFile& nf = sm.file(range.start.file_id);
            auto [nline, ncol] = nf.line_col(range.start.offset);
            out << "    " << nf.name() << ':' << nline << ':' << ncol
                << ": " << msg << '\n';
        }
    }
    return out.str();
}

} // namespace tether
