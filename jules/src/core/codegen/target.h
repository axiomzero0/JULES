// Machine target description — the per-architecture contract.
//
// Backend split: the register allocation ALGORITHM is written once and is
// target-neutral (core/codegen/ralloc.h). Each machine target contributes:
//   1. its instruction selection + MIR (e.g. targets/x86_64/x64_emit.cpp),
//   2. this description: the register-file facts the shared core and the
//      driver consume (register classes, banks, callee-savedness).
// Adding an architecture means writing a new isel and implementing
// MachineTarget — the allocator does not change.
//
// Bank semantics for the RA core: `banks` lists register banks in the
// preference order for ranges that do NOT cross calls; a range that does
// cross a call is eligible only for callee-saved banks (the ABI preserves
// those). Per-function trimming (registers the emitted stream happens to
// never clobber, isel-reserved registers) is applied by the target's RA
// input builder on copies of these facts.
#pragma once

#include "core/support/common.h"

#include <vector>

namespace jules {

struct TargetBankDesc {
    const char* name = "";
    std::vector<u16> regs; // opaque register ids (the MIR's register enum)
    bool callee_saved = false;
};

struct TargetRegClassDesc {
    const char* name = "";
    std::vector<TargetBankDesc> banks;
};

class MachineTarget {
public:
    virtual ~MachineTarget() = default;
    virtual const char* name() const = 0;
    const std::vector<TargetRegClassDesc>& reg_classes() const { return classes_; }

protected:
    std::vector<TargetRegClassDesc> classes_;
};

// Registry (static-registration pattern, like the pass registry).
class TargetRegistry {
public:
    static TargetRegistry& instance();
    void add(MachineTarget* t);
    const MachineTarget* find(const char* name) const;
    const std::vector<MachineTarget*>& all_targets() const { return targets_; }

private:
    std::vector<MachineTarget*> targets_;
};

#define JULES_REGISTER_TARGET(CLASS)                                        \
    static const bool jules_target_registered_##CLASS = [] {                 \
        jules::TargetRegistry::instance().add(new CLASS());                  \
        return true;                                                         \
    }();

} // namespace jules
