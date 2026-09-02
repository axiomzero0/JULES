// Symbol interning: SymbolId is an index; hot paths never touch strings.
// Backing string storage lives in a bulk-freed Arena (see project laws).
#pragma once

#include "core/support/common.h"

namespace jules {

using SymbolId = u32;
inline constexpr SymbolId kNoSymbol = 0xFFFFFFFFu;

class SymbolTable {
public:
    SymbolTable() {
        // id 0 reserved for the empty symbol so kNoSymbol never collides.
        intern("");
    }
    SymbolId intern(std::string_view name) {
        if (auto* p = map_.find(name)) return *p;
        char* buf = arena_.create<char>(name.size() + 1);
        std::memcpy(buf, name.data(), name.size());
        buf[name.size()] = '\0';
        SymbolId id = static_cast<SymbolId>(names_.size());
        names_.push_back(buf);
        map_.insert(name, id);
        return id;
    }
    SymbolId find(std::string_view name) const {
        const SymbolId* p = map_.find(name);
        return p ? *p : kNoSymbol;
    }
    std::string_view name(SymbolId id) const {
        assert(id < names_.size());
        return names_[id];
    }
    size_t size() const { return names_.size(); }

private:
    // Key view into arena-backed storage; comparator on string_view.
    struct SvLess {
        bool operator()(const std::pair<std::string_view, SymbolId>& a,
                        const std::pair<std::string_view, SymbolId>& b) const {
            return a.first < b.first;
        }
    };
    Arena arena_;
    std::vector<std::string_view> names_;
    FlatMap<std::string_view, SymbolId> map_;
};

} // namespace jules
