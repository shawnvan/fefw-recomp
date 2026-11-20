// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/abi.h"
namespace nxrt {
#define NXRT_STR2(x) #x
#define NXRT_STR(x) NXRT_STR2(x)
const char* abi_source() { return "recomp/include/nxrecomp/ctx.h, ABI version " NXRT_STR(NXRECOMP_ABI_VERSION); }
int abi_version() { return NXRECOMP_ABI_VERSION; }
// The source tree this runtime was configured from (git commit, "-dirty" appended when the tree had changes; "unknown"
// outside a git checkout). Printed by the startup banner; public-release/build_package.py reads the marker out of
// fefw_rt.dll and refuses a payload whose commit is not the tree being packaged.
#ifndef NXRT_BUILD_ID
#define NXRT_BUILD_ID unknown
#endif
const char* build_id() { return "fefw-recomp-build:" NXRT_STR(NXRT_BUILD_ID); }
}  // namespace nxrt
