// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>

struct Ctx;

namespace nxrt::games::fefw::coremods {

// Rates are fifths: 5 means 1.0x. Growth bonus is in percentage points.
enum class Setting : uint8_t {
    GrowthMode, GrowthBonus, Experience, ClassMastery, Support, Mount, Weapon, Skill,
    Fragment, Money, MoveCost, ClassSuccessBonus, ClassLevelAdjustment, SpecialClassCount,
    ClassChapterUnlock,
    InfiniteWeapons, InfiniteMagic, InfiniteMedicine, InfiniteFood,
    QuickBird, QuickMeal, QuickTraining, QuickCapture, QuickService,
    RecruitLevelOne, PerfectExplore,
    UnlimitedWeeklyActivities, PreserveActivityFlames, RemoveWeaponRestrictions,
    DeploymentLimit, Count
};

enum class AdjustmentPage : uint8_t { Rates, Growth };

int get(Setting setting);
int configured(Setting setting); // Saved slider value, even while its page is disabled.
bool set(Setting setting, int value, std::string* error = nullptr);
bool adjustments_enabled(AdjustmentPage page);
bool set_adjustments_enabled(AdjustmentPage page, bool enabled, std::string* error = nullptr);
int minimum(Setting setting);
int maximum(Setting setting);
int initial(Setting setting);
const char* label(Setting setting);

struct GrowthCharacter { std::string pid; std::string label; };
struct GrowthSelection {
    bool available = false;
    bool loaded = false;
    bool rates_loaded = false;
    std::string pid;
    std::array<int, 9> bonus{}; // INT_MAX: inherit the global bonus
    std::array<int, 9> personal{};
    std::array<int, 9> job{}; // Includes class fallback when personal growth is zero.
};
std::vector<GrowthCharacter> growth_characters();
std::string growth_character_label(const std::string& pid);
GrowthSelection growth_selection();
void select_growth_character(const std::string& pid);
bool set_growth_bonus(const std::string& pid, int stat, int bonus);
bool reset_growth_bonuses(); // Queue a reset of all saved per-character bonuses in the current save.
int take_growth_reset_result(); // 0: none/pending, 1: complete, -1: failed.

// Guest hooks are registered once when play mode starts. The settings are
// global to this PC installation and read live by the hooks.
void install(uint64_t main_base);
void remove();

int scale_positive(int delta, int fifths);
int fragment_total(int current, int delta, int fifths);

// Renown belongs to the loaded game save. The F1 menu reads a guest-frame
// snapshot and queues changes for the next guest frame.
bool renown_available();
int renown_current();
int renown_cap();
bool set_renown(int value);
void renown_frame(::Ctx* context);

}  // namespace nxrt::games::fefw::coremods
