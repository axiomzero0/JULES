// x86-64 machine target (see x64_target.h). Register facts mirror the
// predicates in core/codegen/linear.h:
//   * GPR caller-saved allocatable: r10, r11 (isel never writes them;
//     rax/rcx/rdx are isel scratch contracts, rsi/rdi/r8/r9 are argument
//     registers the RA may adopt per-function when the stream is clean)
//   * GPR callee-saved: rbx, r12-r15 (survive SysV calls; pushed by the
//     allocator's prologue)
//   * XMM: xmm2-xmm13 allocatable (xmm0/1 isel scratch, xmm8+ capped by
//     the FP constant pool per function, all caller-saved in SysV)
#include "targets/x86_64/x64_target.h"

#include "core/codegen/linear.h"

namespace jules {

X64MachineTarget::X64MachineTarget() {
    TargetRegClassDesc gpr;
    gpr.name = "GPR";
    {
        TargetBankDesc caller;
        caller.name = "caller";
        caller.regs = {static_cast<u16>(R::R10), static_cast<u16>(R::R11)};
        gpr.banks.push_back(caller);
    }
    {
        TargetBankDesc callee;
        callee.name = "callee";
        callee.regs = {static_cast<u16>(R::Rbx), static_cast<u16>(R::R12),
                       static_cast<u16>(R::R13), static_cast<u16>(R::R14),
                       static_cast<u16>(R::R15)};
        callee.callee_saved = true;
        gpr.banks.push_back(callee);
    }
    classes_.push_back(gpr);

    TargetRegClassDesc xmm;
    xmm.name = "XMM";
    {
        TargetBankDesc caller;
        caller.name = "caller";
        for (int r = static_cast<int>(R::Xmm2); r <= static_cast<int>(R::Xmm13); ++r)
            caller.regs.push_back(static_cast<u16>(r));
        xmm.banks.push_back(caller);
    }
    classes_.push_back(xmm);
}

JULES_REGISTER_TARGET(X64MachineTarget)

} // namespace jules
