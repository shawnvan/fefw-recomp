// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace nxrt::games::fefw::weapon_scope {

// Job.bin +0x38 is the class role, +0x68 is the mount ID, and +0x08 is JID.
// Mounted classes retain their role in the dismounted record, so the JID
// suffix also excludes that record. Unknown roles are left at native rules.
inline bool infantry(std::string_view role, std::string_view jid, bool has_mount) {
    if (has_mount || jid.ends_with("下馬")) return false;
    return role == "攻勢" || role == "防衛" || role == "援護" ||
           role == "魔道" || role == "連携";
}

}  // namespace nxrt::games::fefw::weapon_scope
