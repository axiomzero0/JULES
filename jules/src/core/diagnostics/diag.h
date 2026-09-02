// Diagnostics: structured error/warning reporting with source rendering.
// No exceptions; errors flow out through return codes + this collector.
#pragma once

#include "core/support/common.h"
#include <cstdio>

namespace jules {

enum class Severity : u8 { Info, Warning, Error };

struct SourcePos {
    u32 line = 1;   // 1-based
    u32 col = 1;    // 1-based
    bool operator==(const SourcePos& o) const { return line == o.line && col == o.col; }
};

struct Diagnostic {
    Severity sev = Severity::Info;
    SourcePos pos;
    std::string msg;
};

class Diagnostics {
public:
    void error(SourcePos pos, std::string msg) { add(Severity::Error, pos, std::move(msg)); }
    void warn(SourcePos pos, std::string msg) { add(Severity::Warning, pos, std::move(msg)); }
    void info(SourcePos pos, std::string msg) { add(Severity::Info, pos, std::move(msg)); }

    bool has_errors() const { return errors_ > 0; }
    u32 error_count() const { return errors_; }
    u32 warning_count() const { return warnings_; }
    const std::vector<Diagnostic>& all() const { return diags_; }
    void clear() { diags_.clear(); errors_ = warnings_ = 0; }

    // Render all diagnostics with a source excerpt, deterministic order.
    void render(std::FILE* out, std::string_view source) const;

private:
    void add(Severity sev, SourcePos pos, std::string msg) {
        if (sev == Severity::Error) ++errors_;
        else if (sev == Severity::Warning) ++warnings_;
        diags_.push_back(Diagnostic{sev, pos, std::move(msg)});
    }
    std::vector<Diagnostic> diags_;
    u32 errors_ = 0;
    u32 warnings_ = 0;
};

} // namespace jules
