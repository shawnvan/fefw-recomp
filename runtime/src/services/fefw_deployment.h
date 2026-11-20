// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace nxrt::services::fs { class RomfsImage; }

namespace nxrt::services {
// Build a per-launch RomFS overlay with extra deployment positions. Returns
// an empty path when the original limit is requested or no safe files match.
std::string fefw_deployment_overlay(const fs::RomfsImage& image,
                                     const std::string& romfs_dir,
                                     const std::vector<std::string>& existing_overlays,
                                     int target);
}
