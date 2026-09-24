// Tier-4 SMT verification entries (x86-64). Implementation: x64_super_smt.cpp.
#pragma once

#include "superopt/superopt.h"

namespace jules {

// True when a Z3 binary was discovered (the tier will actually run).
bool x64_super_smt_available();

// Tier 4: prove (or refute) the candidate equivalent to the original window
// on the live-out contract, over ALL inputs. Proven => the commit is
// certified; Refuted => the commit is rejected; Unknown => degrades to the
// Tier-2 sampling verdict (unless JULES_SUPEROPT_SMT=2 requires proof).
superopt::Verdict x64_super_tier4(const superopt::Tier4Query& q);

// Tier 3 cross-probe: re-evaluate the ORIGINAL on the batch-0 concrete
// inputs through the SMT encoder and require bit-exact agreement with the
// simulator on the contract (the differential lock between the two semantic
// sources). Aborts loudly on any disagreement.
bool x64_super_smt_selftest(const superopt::Tier4Query& q);

} // namespace jules
