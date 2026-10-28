#pragma once
#include <cstdint>

// DesertStormFix's simulation revision, shared by the host's simulation fingerprint (features/onlinecompat.cpp) and the
// fingerprint every split-out dsfix.<feature> plugin declares (plugins/kit, PSEPlugin_SimFingerprint): online peers must
// agree on it. Bump when simulation code changes without a DS_VERSION bump or a patch-table change - in dsfix.dll or in
// any of DesertStormFix's own plugin DLLs.
namespace dsfix {
constexpr uint32_t kSimRevision = 5;   // 2: co-op mission scripts (coopscripts.cpp); 3: owner-mode records 'L' 10 (held item); 4: inputs 'L' 11 (mission start), pick-ups, skip vote; 5: destroyed vehicles (EA VX), vehicle heading
}  // namespace dsfix
