#include "core/diagnostics/diag.h"

namespace jules {

namespace {
std::string_view nth_line(std::string_view src, u32 line) {
    u32 cur = 1;
    size_t start = 0;
    for (size_t i = 0; i <= src.size(); ++i) {
        if (cur == line) {
            if (i == src.size() || src[i] == '\n')
                return src.substr(start, i - start);
        } else if (i < src.size() && src[i] == '\n') {
            ++cur; start = i + 1;
        }
    }
    return {};
}
const char* sev_name(Severity s) {
    switch (s) {
        case Severity::Info:    return "note";
        case Severity::Warning: return "warning";
        case Severity::Error:   return "error";
    }
    return "error";
}
} // namespace

void Diagnostics::render(std::FILE* out, std::string_view source) const {
    for (const Diagnostic& d : diags_) {
        std::fprintf(out, "%u:%u: %s: %s\n", d.pos.line, d.pos.col, sev_name(d.sev), d.msg.c_str());
        std::string_view ln = nth_line(source, d.pos.line);
        if (!ln.empty()) {
            std::fprintf(out, "  |  %.*s\n", static_cast<int>(ln.size()), ln.data());
            if (d.pos.col > 0 && d.pos.col <= ln.size() + 1) {
                std::fprintf(out, "  |  ");
                for (u32 i = 1; i < d.pos.col; ++i) std::fputc(' ', out);
                std::fprintf(out, "^\n");
            }
        }
    }
    if (errors_) std::fprintf(out, "%u error(s), %u warning(s)\n", errors_, warnings_);
}

} // namespace jules
