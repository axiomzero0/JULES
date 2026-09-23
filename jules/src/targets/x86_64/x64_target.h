// x86-64 machine target: register-file facts for the shared allocator core.
//
// The register enums in core/codegen/linear.h are the MIR-side spelling of
// these ids; this file is the single source of truth for what the
// allocator may use. Per-function trimming (argument registers the emitted
// stream never writes join the caller bank; the isel FP constant pool
// trims the XMM bank from the top) is applied by x64_ra.cpp on copies.
#pragma once

#include "core/codegen/target.h"

namespace jules {

class X64MachineTarget : public MachineTarget {
public:
    X64MachineTarget();
    const char* name() const override { return "x86_64"; }
};

} // namespace jules
