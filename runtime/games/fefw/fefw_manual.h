// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace nxrt::games::fefw::manual {
void install(uint64_t main_base);
void remove();
bool enabled();
bool difficulty_switch_enabled();
}
