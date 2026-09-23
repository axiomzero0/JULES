#include "core/codegen/target.h"

#include <string_view>

namespace jules {

TargetRegistry& TargetRegistry::instance() {
    static TargetRegistry reg;
    return reg;
}

void TargetRegistry::add(MachineTarget* t) { targets_.push_back(t); }

const MachineTarget* TargetRegistry::find(const char* name) const {
    for (MachineTarget* t : targets_)
        if (name && t->name() && std::string_view(t->name()) == name) return t;
    return nullptr;
}

} // namespace jules
