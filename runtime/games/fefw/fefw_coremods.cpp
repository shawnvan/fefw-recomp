// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime controls for the Fortune's Weave progression patch. The original
// AArch64 hook addresses and arithmetic are documented in the Eden source
// package's hooks.json and *_adjust.c files (game update 131072 only).
#include "fefw_coremods.h"
#include "fefw_actions.h"
#include "fefw_weapon_scope.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::fefw::coremods {
namespace {

#include "fefw_growth_names.inc"

struct Spec { const char* key; const char* name; int lo, hi, def; };
constexpr std::array<Spec, static_cast<size_t>(Setting::Count)> kSpecs{{
    {"growth_mode", "成长方式", -1, 1, -1}, // -1: game's own setting
    {"growth_bonus", "成长补正", -100, 100, 0},
    {"experience_rate", "人物等级经验倍率", 0, 50, 5},
    {"class_mastery_rate", "职业精通经验倍率", 0, 50, 5},
    {"support_rate", "人物支援倍率", -50, 50, 5},
    {"mount_rate", "坐骑支援倍率", -50, 50, 5},
    {"weapon_rate", "武器／魔法熟练倍率", -50, 50, 5},
    {"skill_rate", "兵种技能熟练倍率", -50, 50, 5},
    {"fragment_rate", "因果碎片获得倍率", -50, 50, 5},
    {"money_rate", "金钱获得倍率", -50, 50, 5},
    {"move_cost_steps", "大地图移动回合消耗", 0, 200, 20}, // fifth-percent steps: 0..1000%, default 100%
    {"class_success_bonus", "转职成功率补正", -100, 100, 0},
    {"class_level_adjustment", "转职等级补正", -99, 99, 0},
    {"special_class_count", "解除特殊职业人数限制", 0, 1, 0},
    {"class_chapter_unlock", "解除转职章节限制", 0, 1, 0},
    {"infinite_weapons", "无限武器耐久", 0, 1, 0},
    {"infinite_magic", "无限魔法次数", 0, 1, 0},
    {"infinite_medicine", "无限药品次数", 0, 1, 0},
    {"infinite_food", "无限烹饪食材", 0, 1, 0},
    {"quick_bird", "逗鸟快捷", 0, 1, 0},
    {"quick_meal", "快捷用餐且必定 Perfect", 0, 1, 0},
    {"quick_training", "快捷训练且必定 Perfect", 0, 1, 0},
    {"quick_capture", "抓坐骑快捷", 0, 1, 0},
    {"quick_service", "快捷侍奉且必定 Perfect", 0, 1, 0},
    {"recruit_level_one", "新入队角色从 1 级培养", 0, 1, 0},
    {"perfect_explore", "野外探索必定 Perfect", 0, 1, 0},
    {"unlimited_weekly_activities", "每周活动次数不限", 0, 1, 0},
    {"preserve_activity_flames", "角色每周活动次数不消耗", 0, 1, 0},
    {"remove_weapon_restrictions", "解锁步兵系职业武器/战技限制", 0, 1, 0},
    {"deployment_limit", "普通战斗自选出击人数", 6, 25, 6},
}};

struct State {
    std::array<std::atomic<int>, kSpecs.size()> values{};
    std::atomic<bool> rates_enabled{true};
    std::atomic<bool> growth_enabled{true};
    std::array<uint32_t, 128> hooks{};
    size_t hook_count = 0;
    uint64_t base = mem::kMainBase;
    uint64_t fixed_value = 0;
    uint64_t difficulty_text = 0;
    uint64_t save_difficulty_text = 0;
    uint64_t class_lookup_id = 0;
    std::mutex class_lookup_mutex;
    std::atomic<uint64_t> renown_user{0};
    std::atomic<int> renown_value{0};
    std::atomic<int> renown_limit{0};
    std::mutex renown_mutex;
    uint64_t pending_user = 0;
    int pending_value = -1;
    bool loaded = false;
    uint64_t food_base = 0;
    uint64_t food_user = 0;
    std::array<uint16_t, 322> food_counts{};
    bool food_ready = false;
    unsigned food_probe_frames = 119;
    uint64_t move_storage = 0;
    bool move_key_ready = false;
    std::atomic<uint32_t> move_revision{0};
    uint32_t move_applied_revision = 0;
    bool move_warned = false;
    std::mutex growth_mutex;
    std::vector<GrowthCharacter> growth_roster;
    GrowthSelection growth_selected;
    uint64_t growth_user = 0;
    unsigned growth_frames = 0;
    struct GrowthChange { uint64_t user; std::string pid; int stat; int bonus; };
    std::vector<GrowthChange> growth_pending;
    bool growth_reset_pending = false;
    std::atomic<int> growth_reset_result{0};
    std::mutex growth_key_mutex;
    std::map<std::string, uint64_t> growth_keys;
};
State& state() { static State s; return s; }
thread_local uint64_t t_recruit_prepare = 0;
thread_local uint64_t t_recruit_job = 0;
size_t index(Setting s) { return static_cast<size_t>(s); }
int clamp(Setting s, int value) {
    const Spec& spec = kSpecs[index(s)];
    return std::clamp(value, spec.lo, spec.hi);
}
void load() {
    State& s = state();
    if (s.loaded) return;
    for (size_t i = 0; i < kSpecs.size(); ++i) {
        const Spec& spec = kSpecs[i];
        const int64_t value = config_get_int("coremods", spec.key, spec.def);
        s.values[i].store(static_cast<int>(std::clamp<int64_t>(value, spec.lo, spec.hi)));
    }
    s.rates_enabled.store(config_get_bool("coremods", "rate_adjustments_enabled", true));
    s.growth_enabled.store(config_get_bool("coremods", "growth_adjustments_enabled", true));
    s.loaded = true;
}
uint32_t w(const ::Ctx* c, int n) { return static_cast<uint32_t>(c->x[n]); }
int32_t sw(const ::Ctx* c, int n) { return static_cast<int32_t>(w(c, n)); }
void putw(::Ctx* c, int n, int32_t value) { c->x[n] = static_cast<uint32_t>(value); }

template <typename T> bool read(uint64_t addr, T* out) {
    if (!addr || !mem::is_mapped(addr, sizeof(T))) return false;
    std::memcpy(out, reinterpret_cast<const void*>(addr), sizeof(T));
    return true;
}
template <typename T> bool write(uint64_t addr, const T& value) {
    if (!addr || !mem::is_mapped(addr, sizeof(T))) return false;
    std::memcpy(reinterpret_cast<void*>(addr), &value, sizeof(T));
    return true;
}
bool player_unit(uint64_t unit) {
    uint64_t person = 0, force = 0;
    uint8_t kind = 0;
    return read(unit + 0x188, &person) && person &&
           read(unit + 0x1b8, &force) && force &&
           read(force + 0x10, &kind) && kind < 8 &&
           ((1u << kind) & 0xf1u);
}
uint64_t recruit_base_job() {
    // The symbol is a global Job* slot, not the Job object itself.
    uint64_t job = 0;
    uint64_t name = 0;
    constexpr char expected[] = "JID_無職";
    if (!read(state().base + 0x22099c8, &job) || !job ||
        !read(job + 8, &name) || !name || !mem::is_mapped(name, sizeof(expected))) {
        static std::atomic<bool> once{false};
        if (!once.exchange(true)) NXLOG_I("recruit", "base job missing job=%llx name=%llx", static_cast<unsigned long long>(job), static_cast<unsigned long long>(name));
        return 0;
    }
    if (std::memcmp(reinterpret_cast<const void*>(name), expected, sizeof(expected))) {
        static std::atomic<bool> once{false};
        if (!once.exchange(true)) NXLOG_I("recruit", "base job mismatch job=%llx name=%llx value=%.*s", static_cast<unsigned long long>(job), static_cast<unsigned long long>(name), 10, reinterpret_cast<const char*>(name));
        return 0;
    }
    return job;
}
uint64_t canonical_person(uint64_t person) {
    uint64_t extra = 0, base = 0;
    if (read(person + 0x18, &extra) && extra && read(extra + 0x6f8, &base) && base) return base;
    return person;
}
std::string person_pid(uint64_t person) {
    person = canonical_person(person);
    uint64_t name = 0;
    if (!person || !read(person + 8, &name) || !name || !mem::is_mapped(name, 160)) return {};
    const char* begin = reinterpret_cast<const char*>(name);
    const size_t length = strnlen(begin, 160);
    return length < 160 && length >= 5 && !std::memcmp(begin, "PID_", 4)
        ? std::string(begin, length) : std::string{};
}
bool recruit_level_one_excluded(uint64_t person) {
    person = canonical_person(person);
    uint64_t name = 0;
    if (!read(person + 8, &name) || !name || !mem::is_mapped(name, 48)) return false;
    const char* pid = reinterpret_cast<const char*>(name);
    // These story characters need their native level, job, or mount on entry.
    for (const char* excluded : {"PID_コウカ", "PID_アレキサンドラ", "PID_イオ"})
        if (std::strncmp(pid, excluded, std::strlen(excluded) + 1) == 0) return true;
    return false;
}
bool recruit_already_player(uint64_t person, uint64_t new_unit) {
    uint64_t manager = 0, units = 0;
    // Likewise, this global stores a UnitManager*.
    if (!person || !read(state().base + 0x220a350, &manager) || !manager ||
        !read(manager + 0x38, &units) || !units) {
        return true;
    }
    person = canonical_person(person);
    for (unsigned i = 0; i < 250; ++i) {
        const uint64_t unit = units + static_cast<uint64_t>(i) * 0x8b0;
        if (unit == new_unit) continue;
        uint64_t force = 0, candidate = 0;
        uint8_t kind = 9;
        if (!read(unit + 0x1b8, &force) || !force ||
            !read(force + 0x10, &kind) || kind >= 8 ||
            !((1u << kind) & 0xf1u)) continue;
        if (read(unit + 0x188, &candidate) && canonical_person(candidate) == person) {
            return true;
        }
    }
    return false;
}
void recruit_prepare(::Ctx* c, uint64_t address, void*) {
    t_recruit_prepare = 0;
    t_recruit_job = 0;
    if (!get(Setting::RecruitLevelOne)) return;
    const uint64_t arg = c->x[0], unit = c->x[19];
    uint64_t caller = 0, person = 0;
    if (!read(c->x[29] + 8, &caller) || !read(arg, &person) || !person) {
        return;
    }
    caller &= 0x0000ffffffffffffull; // Native return addresses may carry PAC bits.
    bool new_recruit = false;
    uint32_t force = 0;
    if (address == state().base + 0x6da024) {
        new_recruit = caller == state().base + 0x1231e7c &&
            read(c->x[31] + 0xe0, &force) && force == 4;
    } else {
        new_recruit = caller == state().base + 0x89a390 ||
            caller == state().base + 0x89c054 ||
            caller == state().base + 0xf89610 ||
            caller == state().base + 0xf89668;
    }
    const uint64_t job = new_recruit ? recruit_base_job() : 0;
    const bool already_player = recruit_already_player(person, unit);
    if (!job || already_player || recruit_level_one_excluded(person)) return;
    const int32_t level = 1, experience = 0;
    const float multiplier = 1.0f;
    const uint8_t mode = 0, class_mode = 6;
    if (!write(arg + 8, job) || !write(arg + 0x58, level) ||
        !write(arg + 0x5c, experience) || !write(arg + 0x60, multiplier) ||
        !write(arg + 0x68, mode) || !write(arg + 0x69, class_mode)) return;
    t_recruit_prepare = arg;
    t_recruit_job = job;
}
void recruit_prepared(::Ctx*, uint64_t, void*) {
    const uint64_t arg = std::exchange(t_recruit_prepare, 0ull);
    const uint64_t job = std::exchange(t_recruit_job, 0ull);
    if (!arg || !job) return;
    uint64_t begin = 0, end = 0;
    if (read(arg + 0x10, &begin) && read(arg + 0x18, &end) &&
        begin && end > begin && write(begin, job)) {
        write(arg + 0x18, begin + sizeof(uint64_t));
    }
}
void recruit_floor(::Ctx* c, uint64_t, void*) {
    if (!get(Setting::RecruitLevelOne) || !recruit_base_job()) return;
    uint64_t person = 0;
    if (read(c->x[19] + 0x188, &person) && person &&
        !recruit_level_one_excluded(person) &&
        !recruit_already_player(person, c->x[19])) putw(c, 0, 1);
}
// The manager's Unit array begins before each character's inventory record.
// Never address held or magic slots relative to the manager's Unit pointer:
// its +0x180 region contains live game pointers, not item IDs.
bool inventory_unit(uint64_t unit, unsigned index, uint64_t* out) {
    const uint64_t inventory = unit + 0x1ec;
    uint32_t header = 0, level = 0;
    uint16_t hp = 0;
    if (!read(inventory - 4, &header) || !read(inventory, &hp) ||
        !read(inventory + 4, &level) ||
        (header >> 16) != index + 1 || (header & 0xffffu) > 32 ||
        !hp || (level & 0xffffu) || !(level >> 16)) return false;
    *out = inventory;
    return true;
}

// Movement is committed through two Gmap branches. Keep its fractional turn
// remainder in a Type 1 game variable so saving/reloading a slot retains it.
// The global F1 percentage is encoded alongside the remainder: a different
// percentage resets an older save's fractional progress on its next move.
constexpr char kMoveStateName[] = "CW_PC_MOVE_STATE";
constexpr int kMoveStateMax = 100199;
bool move_key(::Ctx* context) {
    State& s = state();
    if (s.move_key_ready) return true;
    if (!s.move_storage || !fn_lookup(s.base + 0x7ac700)) return false;
    const uint64_t name = s.move_storage + 0x20;
    if (!mem::is_mapped(name, sizeof(kMoveStateName))) return false;
    std::memcpy(reinterpret_cast<void*>(name), kMoveStateName, sizeof(kMoveStateName));
    ::Ctx call = *context;
    call.x[0] = name;
    dispatch_call(&call, s.base + 0x7ac700);
    if (!write(s.move_storage, call.x[0]) || !write(s.move_storage + 8, name)) return false;
    s.move_key_ready = true;
    return true;
}
bool move_variable(::Ctx* context, uint64_t* user, uint64_t* entry, int* value) {
    State& s = state();
    uint64_t impl = 0;
    if (!move_key(context) || !fn_lookup(s.base + 0x65cd00) ||
        !read(s.base + 0x220ab10, user) || !*user ||
        !read(*user + 0x30, &impl) || !impl) return false;
    ::Ctx call = *context;
    call.x[0] = impl;
    call.x[1] = s.move_storage;
    dispatch_call(&call, s.base + 0x65cd00);
    *entry = call.x[0];
    *value = 0;
    return !*entry || read(*entry + 0x20, value);
}
bool move_variable_set(::Ctx* context, uint64_t user, uint64_t entry, int value) {
    State& s = state();
    ::Ctx call = *context;
    if (entry) {
        if (!fn_lookup(s.base + 0x719980)) return false;
        call.x[0] = s.move_storage;
        call.x[1] = static_cast<uint32_t>(value);
        dispatch_call(&call, s.base + 0x719980);
        return true;
    }
    if (!fn_lookup(s.base + 0x82c460)) return false;
    call.x[0] = user + 0x28;
    call.x[1] = 1;
    call.x[2] = s.move_storage;
    call.x[3] = static_cast<uint32_t>(value);
    call.x[4] = 0;
    call.x[5] = 0;
    call.x[6] = kMoveStateMax;
    call.x[7] = 0;
    dispatch_call(&call, s.base + 0x82c460);
    return call.x[0] != 0;
}

// Type 1 variables belong to the current scene and are discarded when a
// battle starts. Type 2 variables survive hub/battle transitions in the
// native save. Keep the old key only for migrating existing test saves.
uint64_t growth_key(::Ctx* context, const std::string& pid, int stat, bool legacy = false) {
    State& s = state();
    if (pid.empty() || pid.size() > 140 || stat < 0 || stat >= 9 || !fn_lookup(s.base + 0x7ac700)) return 0;
    const std::string name = (legacy ? "CW_PC_GROWTH_" : "CW_PC_GROWTH_V2_") +
        pid + "_" + std::to_string(stat);
    std::lock_guard<std::mutex> lock(s.growth_key_mutex);
    if (auto it = s.growth_keys.find(name); it != s.growth_keys.end()) return it->second;
    const uint64_t storage = mem::alloc_kernel(0x100);
    if (!storage || !mem::is_mapped(storage, 0x100) || name.size() + 1 > 0xe0) return 0;
    std::memcpy(reinterpret_cast<void*>(storage + 0x20), name.c_str(), name.size() + 1);
    ::Ctx call = *context;
    call.x[0] = storage + 0x20;
    dispatch_call(&call, s.base + 0x7ac700);
    write(storage, call.x[0]);
    write(storage + 8, storage + 0x20);
    s.growth_keys.emplace(name, storage);
    return storage;
}
bool growth_variable(::Ctx* context, uint64_t user, const std::string& pid, int stat,
                     uint64_t* storage, uint64_t* entry, int* packed, bool legacy = false) {
    State& s = state();
    uint64_t impl = 0;
    *storage = growth_key(context, pid, stat, legacy);
    if (!*storage || !user || !fn_lookup(s.base + 0x65cd00) ||
        !read(user + 0x30, &impl) || !impl) return false;
    ::Ctx call = *context;
    call.x[0] = impl;
    call.x[1] = *storage;
    dispatch_call(&call, s.base + 0x65cd00);
    *entry = call.x[0];
    *packed = 0;
    return !*entry || read(*entry + 0x20, packed);
}
bool growth_variable_set(::Ctx* context, uint64_t user, uint64_t storage,
                         uint64_t entry, int packed) {
    State& s = state();
    ::Ctx call = *context;
    if (entry) {
        if (!fn_lookup(s.base + 0x719980)) return false;
        call.x[0] = storage;
        call.x[1] = static_cast<uint32_t>(packed);
        dispatch_call(&call, s.base + 0x719980);
        return true;
    }
    if (!fn_lookup(s.base + 0x82c460)) return false;
    call.x[0] = user + 0x28;
    call.x[1] = 2;
    call.x[2] = storage;
    call.x[3] = static_cast<uint32_t>(packed);
    call.x[4] = 0;
    call.x[5] = 0;
    call.x[6] = 2001;
    call.x[7] = 0;
    dispatch_call(&call, s.base + 0x82c460);
    return call.x[0] != 0;
}
bool growth_value(::Ctx* context, uint64_t user, const std::string& pid, int stat,
                  int* bonus, bool migrate) {
    uint64_t storage = 0, entry = 0;
    int packed = 0;
    if (!growth_variable(context, user, pid, stat, &storage, &entry, &packed)) return false;
    if (entry) {
        *bonus = packed >= 1 && packed <= 2001 ? packed - 1001 : INT_MAX;
        return true; // A V2 entry set to zero explicitly restores inheritance.
    }
    uint64_t old_storage = 0, old_entry = 0;
    int old_packed = 0;
    if (!growth_variable(context, user, pid, stat, &old_storage, &old_entry,
                         &old_packed, true)) return false;
    *bonus = old_entry && old_packed >= 1 && old_packed <= 2001 ? old_packed - 1001 : INT_MAX;
    if (*bonus != INT_MAX && migrate &&
        !growth_variable_set(context, user, storage, 0, old_packed)) {
        NXLOG_W("coremods", "cannot migrate per-character growth modifier");
    }
    return true;
}
// Enumerate the native save's growth variables so the reset also covers
// characters that are not currently present in the battle roster.
bool reset_saved_growth_bonuses(::Ctx* context, uint64_t user) {
    uint64_t impl = 0;
    if (!read(user + 0x30, &impl) || !impl) return false;
    constexpr const char* prefixes[] = {"CW_PC_GROWTH_", "CW_PC_GROWTH_V2_"};
    for (int type = 1; type <= 2; ++type) {
        uint64_t first = 0, last = 0;
        const uint64_t vector = impl + 0x3c0000 + 8 + static_cast<uint64_t>(type) * 24;
        if (!read(vector, &first) || !read(vector + 8, &last) ||
            last < first || (last - first) % 8 || (last - first) / 8 > 10000) return false;
        for (uint64_t at = first; at < last; at += 8) {
            uint64_t entry = 0, name_ptr = 0;
            if (!read(at, &entry) || !entry || !read(entry + 8, &name_ptr) || !name_ptr) continue;
            std::string name;
            for (size_t i = 0; i < 180; ++i) {
                char ch = 0;
                if (!read(name_ptr + i, &ch)) break;
                if (!ch) break;
                name.push_back(ch);
            }
            const std::string prefix = prefixes[type - 1];
            if (name.compare(0, prefix.size(), prefix) != 0) continue;
            const size_t suffix = name.rfind('_');
            if (suffix == std::string::npos || suffix <= prefix.size() ||
                suffix + 2 != name.size() || name[suffix + 1] < '0' || name[suffix + 1] > '8') continue;
            const std::string pid = name.substr(prefix.size(), suffix - prefix.size());
            uint64_t storage = 0, found = 0;
            int packed = 0;
            if (!growth_variable(context, user, pid, name[suffix + 1] - '0',
                                 &storage, &found, &packed, type == 1) ||
                (found && packed && !growth_variable_set(context, user, storage, found, 0))) return false;
        }
    }
    return true;
}
void move_reset_frame(::Ctx* context) {
    State& s = state();
    const uint32_t revision = s.move_revision.load();
    if (revision == s.move_applied_revision) return;
    uint64_t user = 0, entry = 0;
    int packed = 0;
    if (!move_variable(context, &user, &entry, &packed)) return;
    const int percent = get(Setting::MoveCost) * 5;
    if (entry && !move_variable_set(context, user, entry, (percent + 1) * 100)) return;
    s.move_applied_revision = revision;
    NXLOG_I("coremods", "move remainder reset for loaded save at %d%%", percent);
}
int move_turns(::Ctx* context) {
    State& s = state();
    const int percent = get(Setting::MoveCost) * 5;
    uint64_t user = 0, entry = 0;
    int packed = 0;
    if (!move_variable(context, &user, &entry, &packed)) {
        if (!s.move_warned) NXLOG_W("coremods", "move state unavailable; using native one-turn cost");
        s.move_warned = true;
        return 1; // fail open
    }
    const uint32_t revision = s.move_revision.load();
    const bool reset = revision != s.move_applied_revision ||
        packed < 100 || packed > kMoveStateMax || packed / 100 - 1 != percent;
    const int previous = reset ? 0 : packed % 100;
    const int total = percent + previous;
    const int result = total / 100;
    const int updated = (percent + 1) * 100 + total % 100;
    if ((percent != 100 || entry) && (updated != packed || reset) &&
        !move_variable_set(context, user, entry, updated)) {
        if (!s.move_warned) NXLOG_W("coremods", "move state could not be saved; using native one-turn cost");
        s.move_warned = true;
        return 1;
    }
    s.move_applied_revision = revision;
    NXLOG_I("coremods", "committed map move: cost=%d%% previous=%d turns=%d remainder=%d",
            percent, previous, result, updated % 100);
    return result;
}
void move_date(::Ctx* context, uint64_t, void*) {
    State& s = state();
    const uint64_t original = s.base + 0x711650;
    const uint64_t source = context->x[0], deadline = context->x[1], out = context->x[8];
    context->x[30] = s.base + 0x14fa09c; // the original BL's link register
    std::array<uint16_t, 6> date{};
    if (!s.move_storage || !read(source, &date) || !mem::is_mapped(out, sizeof(date)) ||
        !mem::is_mapped(s.move_storage + 0x80, sizeof(date))) {
        dispatch_call(context, original);
        return;
    }
    const int turns = move_turns(context);
    if (turns == 1) {
        dispatch_call(context, original);
        return;
    }
    if (!turns) {
        write(out, date);
        return;
    }
    const uint64_t scratch = s.move_storage + 0x80;
    for (int i = 0; i < turns; ++i) {
        if (!write(scratch, date)) break;
        ::Ctx call = *context;
        call.x[0] = scratch;
        call.x[1] = deadline;
        call.x[8] = out;
        dispatch_call(&call, original);
        if (!read(out, &date)) break;
    }
}
void move_immediate(::Ctx* context, uint64_t, void*) {
    const uint64_t base = state().base;
    const uint64_t original = base + 0x871480;
    context->x[30] = base + 0x14fa038; // the original BL's link register
    const int turns = move_turns(context);
    if (turns == 1) {
        dispatch_call(context, original);
        return;
    }
    context->x[0] = 0;
    for (int i = 0; i < turns; ++i) {
        ::Ctx call = *context;
        dispatch_call(&call, original);
        if (!call.x[0]) break;
        context->x[0] = 1;
    }
}

// The game's held-item and magic slots are eight bytes: item ID at +0,
// consumed uses at +2. Only player-side units are touched. Special items,
// gifts and quest objects never enter the medicine whitelist.
void refill_uses() {
    const bool weapons = get(Setting::InfiniteWeapons) != 0;
    const bool magic = get(Setting::InfiniteMagic) != 0;
    const bool medicine = get(Setting::InfiniteMedicine) != 0;
    if (!weapons && !magic && !medicine) return;
    State& s = state();
    uint64_t manager = 0, units = 0, item_table = 0, rows = 0, row_end = 0;
    if (!read(s.base + 0x220a350, &manager) || !manager ||
        !read(manager + 0x38, &units) || !units) return;
    if (!read(s.base + 0x22099f8, &item_table) || !item_table ||
        !read(item_table + 0x40, &rows) ||
        !read(item_table + 0x48, &row_end) ||
        !rows || row_end <= rows || row_end - rows > 65536 * 8ull) {
        rows = row_end = 0;
    }
    for (unsigned i = 0; i < 250; ++i) {
        const uint64_t unit = units + static_cast<uint64_t>(i) * 0x8b0;
        if (!player_unit(unit)) continue;
        uint64_t inventory = 0;
        if (!inventory_unit(unit, i, &inventory)) continue;
        for (unsigned k = 0; k < 6 && (weapons || medicine); ++k) {
            const uint64_t slot = inventory + 0x150 + k * 8;
            uint16_t id = 0;
            uint8_t used = 0;
            if (!read(slot, &id) || !id || !read(slot + 2, &used) || !used) continue;
            if (!rows || id >= (row_end - rows) / 8) continue;
            uint64_t row = 0;
            uint8_t kind = 0;
            if (!read(rows + static_cast<uint64_t>(id) * 8, &row) || !row ||
                !read(row + 0x48, &kind)) continue;
            const bool ordinary_medicine = kind == 14 &&
                ((id >= 641 && id <= 644) || (id >= 647 && id <= 649));
            // Kinds 5-6 are spells and kind 7 is a monster/siege attack.
            const bool ordinary_weapon = kind <= 4;
            if ((medicine && ordinary_medicine) || (weapons && ordinary_weapon)) {
                const uint8_t zero = 0;
                write(slot + 2, zero);
            }
        }
        if (magic) for (unsigned k = 0; k < 9; ++k) {
            const uint64_t slot = inventory + 0x180 + k * 8;
            uint16_t id = 0;
            uint8_t used = 0;
            if (!read(slot, &id) || !id || !read(slot + 2, &used) || !used ||
                !rows || id >= (row_end - rows) / 8) continue;
            uint64_t row = 0;
            uint8_t kind = 0;
            if (!read(rows + static_cast<uint64_t>(id) * 8, &row) || !row ||
                !read(row + 0x48, &kind) || (kind != 5 && kind != 6)) continue;
            const uint8_t zero = 0;
            write(slot + 2, zero);
        }
    }
}

bool food_id(uint16_t id) {
    // The ordinary meat/fish/produce/spice entries in the game's material
    // table. Bait (1020), trial seed (1049), and all quest items are excluded.
    return id >= 1007 && id <= 1113 && id != 1020 && id != 1049;
}

bool valid_food_table(uint64_t base) {
    if (!mem::is_mapped(base, 322 * 12)) return false;
    uint16_t previous = 0;
    for (unsigned i = 0; i < 322; ++i) {
        uint16_t id = 0, count = 0;
        uint32_t marker = 0;
        const uint64_t cell = base + i * 12;
        if (!read(cell + 2, &id) || !read(cell, &count) ||
            !read(cell + 4, &marker) || marker != 0x10000 ||
            id < 988 || id > 1313 || id <= previous || count > 9999) return false;
        previous = id;
    }
    return true;
}

void protect_food() {
    State& s = state();
    if (!get(Setting::InfiniteFood)) {
        s.food_ready = false;
        s.food_base = s.food_user = 0;
        return;
    }
    uint64_t user = 0, manager = 0, units = 0;
    if (!read(s.base + 0x220ab10, &user) || !user ||
        !read(s.base + 0x220a350, &manager) || !manager ||
        !read(manager + 0x38, &units) || !units) {
        s.food_ready = false;
        s.food_base = s.food_user = 0;
        return;
    }
    if (s.food_user != user) {
        s.food_ready = false;
        s.food_base = 0;
        s.food_user = user;
    }
    if (!s.food_base) {
        if (++s.food_probe_frames < 120) return;
        s.food_probe_frames = 0;
        // The material block is near the unit arena, but its offset is relative
        // to a live unit record rather than the manager's array base. Search
        // only that nearby mapped region and require one complete table.
        uint64_t found = 0;
        const uint64_t scan_begin = units + 0x8ad4a - 0x2000;
        const uint64_t scan_end = units + 0x8ad4a + 0x90000;
        for (uint64_t page = scan_begin & ~0xfffull; page < scan_end; page += 0x1000) {
            if (!mem::is_mapped(page, 0x1000)) continue;
            const auto* bytes = reinterpret_cast<const uint8_t*>(page);
            for (unsigned off = 0; off + 8 <= 0x1000; off += 2) {
                // A material cell is: count:u16, ID:u16, marker:u32.
                if (bytes[off + 2] != 0xdc || bytes[off + 3] != 3 ||
                    bytes[off + 4] || bytes[off + 5] ||
                    bytes[off + 6] != 1 || bytes[off + 7]) continue;
                const uint64_t candidate = page + off;
                if (!valid_food_table(candidate)) continue;
                if (found && found != candidate) {
                    NXLOG_W("coremods", "food lock: ambiguous material tables; no writes");
                    return;
                }
                found = candidate;
            }
        }
        if (!found) return;
        s.food_base = found;
        NXLOG_I("coremods", "food lock: verified material table at 0x%llx",
                static_cast<unsigned long long>(found));
    }
    if (!valid_food_table(s.food_base)) {
        s.food_base = 0;
        s.food_ready = false;
        NXLOG_W("coremods", "food lock: material table changed; no writes");
        return;
    }
    if (!s.food_ready) {
        for (unsigned i = 0; i < 322; ++i)
            read(s.food_base + i * 12, &s.food_counts[i]);
        s.food_ready = true;
        return;
    }
    for (unsigned i = 0; i < 322; ++i) {
        const uint64_t cell = s.food_base + i * 12;
        uint16_t id = 0, count = 0;
        if (!read(cell + 2, &id) || !read(cell, &count)) return;
        if (!food_id(id)) continue;
        if (count < s.food_counts[i]) write(cell, s.food_counts[i]);
        else s.food_counts[i] = count;
    }
}

// These two native checks load the signed permission byte for the unit's
// current class and the selected weapon type. A negative value rejects the
// weapon. These hooks run after the permission load. Preserve the loaded value
// unless the option is enabled for an allied unit.
std::string_view job_text(uint64_t job, uint64_t offset) {
    uint64_t pointer = 0;
    if (!read(job + offset, &pointer) || !pointer) return {};
    for (size_t length = 0; length < 128; ++length) {
        char character = 0;
        if (!read(pointer + length, &character)) return {};
        if (!character)
            return {reinterpret_cast<const char*>(pointer), length};
    }
    return {};
}
bool infantry_weapon_job(uint64_t job) {
    uint64_t mount = 0;
    return job && read(job + 0x68, &mount) &&
           weapon_scope::infantry(job_text(job, 0x38), job_text(job, 8), mount != 0);
}
void class_weapon_equip(::Ctx* c, uint64_t, void*) {
    if (get(Setting::RemoveWeaponRestrictions) && sw(c, 8) < 0 &&
        player_unit(c->x[19]) && infantry_weapon_job(c->x[0]))
        putw(c, 8, 0);
}

void class_weapon_use(::Ctx* c, uint64_t, void*) {
    if (get(Setting::RemoveWeaponRestrictions) && sw(c, 9) < 0 &&
        player_unit(c->x[20]) && infantry_weapon_job(c->x[0]))
        putw(c, 9, 0);
}

void class_success(::Ctx* c, uint64_t, void*) {
    const int original = sw(c, 8);
    if (original < 0) return; // native invalid-input sentinel
    const int bonus = get(Setting::ClassSuccessBonus);
    if (bonus) putw(c, 8, std::clamp(original + bonus, 0, 100));
}

void class_level(::Ctx* c, uint64_t, void*) {
    const int adjustment = get(Setting::ClassLevelAdjustment);
    if (adjustment) putw(c, 0, std::max(0, sw(c, 0) + adjustment));
}

// The class list, facility entry, and preview actor have separate story-part
// gates. Keep the game's level, proficiency, certificate and exam result
// checks intact; this option only opens the story-part gates.
void class_chapter_rank(::Ctx* c, uint64_t, void*) {
    if (!get(Setting::ClassChapterUnlock)) return;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) NXLOG_I("class-chapter", "visible rank %u -> 5", w(c, 19));
    putw(c, 19, 5);
}
void class_chapter_menu(::Ctx* c, uint64_t, void*) {
    if (!get(Setting::ClassChapterUnlock)) return;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) NXLOG_I("class-chapter", "menu chapter %u -> 2", w(c, 9));
    putw(c, 9, 2);
}
void class_chapter_list(::Ctx* c, uint64_t, void*) {
    if (!get(Setting::ClassChapterUnlock)) return;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) NXLOG_I("class-chapter", "list chapter %u -> 2", w(c, 9));
    putw(c, 9, 2);
}
void class_chapter_exam(::Ctx* c, uint64_t, void*) {
    if (!get(Setting::ClassChapterUnlock) || !(w(c, 25) & 4u) ||
        !player_unit(c->x[21])) return;
    uint8_t rank = 0;
    if (!read(c->x[20] + 49, &rank) || (rank & 0xfeu) != 4u) return;
    // The native exam reason mask uses bit 2 for this story-part gate. At this
    // site all other rejection bits are already accumulated in w25.
    putw(c, 25, static_cast<int32_t>(w(c, 25) & ~4u));
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) NXLOG_I("class-chapter", "top-rank exam story gate cleared");
}
void class_chapter_actor(::Ctx* c, uint64_t, void*) {
    if (!get(Setting::ClassChapterUnlock)) return;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        NXLOG_I("class-chapter", "top-rank actor visibility enabled (chapter %u)", w(c, 8));
    c->Z = 1;
}

void growth_frame(::Ctx* context);

void growth_frame(::Ctx* context) {
    State& s = state();
    uint64_t user = 0;
    read(s.base + 0x220ab10, &user);
    std::string selected_pid;
    std::vector<State::GrowthChange> pending;
    bool reset = false;
    bool scan = false;
    {
        std::lock_guard<std::mutex> lock(s.growth_mutex);
        if (user != s.growth_user) {
            s.growth_user = user;
            s.growth_roster.clear();
            s.growth_selected = {};
            s.growth_pending.clear();
            s.growth_reset_pending = false;
            if (s.growth_reset_result.load() == 2) s.growth_reset_result.store(-1);
            s.growth_frames = 0;
        }
        selected_pid = s.growth_selected.pid;
        pending.swap(s.growth_pending);
        reset = std::exchange(s.growth_reset_pending, false);
        scan = user && ++s.growth_frames % 60 == 1;
        if (scan && !selected_pid.empty()) s.growth_selected.loaded = false;
    }
    if (!user) return;
    if (reset) {
        const bool success = reset_saved_growth_bonuses(context, user);
        if (!success) {
            NXLOG_W("coremods", "cannot reset all saved per-character growth modifiers");
        }
        s.growth_reset_result.store(success ? 1 : -1);
        std::lock_guard<std::mutex> lock(s.growth_mutex);
        if (s.growth_user == user) s.growth_selected.loaded = false;
    }
    for (const auto& change : pending) {
        if (change.user != user) continue;
        uint64_t storage = 0, entry = 0;
        int packed = 0;
        if (!growth_variable(context, user, change.pid, change.stat, &storage, &entry, &packed) ||
            !growth_variable_set(context, user, storage, entry,
                                 change.bonus == INT_MAX ? 0 : change.bonus + 1001)) {
            NXLOG_W("coremods", "cannot save per-character growth modifier");
        }
    }
    if (scan) {
        std::vector<GrowthCharacter> roster;
        uint64_t manager = 0, units = 0;
        if (read(s.base + 0x220a350, &manager) && manager &&
            read(manager + 0x38, &units) && units) {
            for (unsigned i = 0; i < 250; ++i) {
                const uint64_t unit = units + static_cast<uint64_t>(i) * 0x8b0;
                uint64_t person = 0;
                if (!player_unit(unit) || !read(unit + 0x188, &person)) continue;
                const std::string pid = person_pid(person);
                if (pid.empty()) continue;
                const auto existing = std::find_if(roster.begin(), roster.end(),
                    [&](const GrowthCharacter& item) { return item.pid == pid; });
                if (existing == roster.end()) {
                    roster.push_back({pid, growth_character_label(pid)});
                }
            }
        }
        // Migrate every currently recruited character before a scene change.
        // Legacy Type 1 values disappear as soon as the battle scene loads.
        for (const auto& character : roster)
            for (int stat = 0; stat < 9; ++stat) {
                int bonus = INT_MAX;
                growth_value(context, user, character.pid, stat, &bonus, true);
            }
        std::lock_guard<std::mutex> lock(s.growth_mutex);
        if (s.growth_user == user) s.growth_roster = std::move(roster);
    }
    if (selected_pid.empty()) return;
    bool load_selected = false;
    {
        std::lock_guard<std::mutex> lock(s.growth_mutex);
        load_selected = s.growth_user == user && s.growth_selected.pid == selected_pid &&
                        !s.growth_selected.loaded;
    }
    if (!load_selected) return;
    std::array<int, 9> bonuses{};
    bool success = true;
    for (int stat = 0; stat < 9; ++stat) {
        if (!growth_value(context, user, selected_pid, stat, &bonuses[stat], true)) {
            success = false;
            break;
        }
    }
    if (!success) return;
    std::array<int, 9> personal_rates{}, job_rates{};
    bool rates_loaded = false;
    uint64_t manager = 0, units = 0;
    if (read(s.base + 0x220a350, &manager) && manager &&
        read(manager + 0x38, &units) && units) {
        for (unsigned i = 0; i < 250; ++i) {
            const uint64_t unit = units + static_cast<uint64_t>(i) * 0x8b0;
            uint64_t person = 0, job = 0;
            if (!player_unit(unit) || !read(unit + 0x188, &person) ||
                !read(unit + 0x190, &job) || !person || !job ||
                person_pid(person) != selected_pid) continue;
            rates_loaded = true;
            for (int stat = 0; stat < 9; ++stat) {
                uint16_t personal = 0, job_fallback = 0;
                int16_t job_bonus = 0;
                if (!read(person + 0x21c + stat * 2, &personal) ||
                    !read(job + 0x162 + stat * 2, &job_fallback) ||
                    !read(job + 0x178 + stat * 2, &job_bonus)) {
                    rates_loaded = false;
                    break;
                }
                // The native getter uses the class base only when this
                // character has no personal growth for the stat.
                personal_rates[stat] = personal;
                job_rates[stat] = job_bonus + (personal ? 0 : job_fallback);
            }
            break;
        }
    }
    std::lock_guard<std::mutex> lock(s.growth_mutex);
    if (s.growth_user == user && s.growth_selected.pid == selected_pid) {
        s.growth_selected.bonus = bonuses;
        s.growth_selected.personal = personal_rates;
        s.growth_selected.job = job_rates;
        s.growth_selected.rates_loaded = rates_loaded;
        s.growth_selected.loaded = true;
        s.growth_selected.available = true;
    }
}

int renown_frame_hook(::Ctx* c, uint64_t, void*) {
    actions::direct_frame(c);
    renown_frame(c);
    move_reset_frame(c);
    growth_frame(c);
    refill_uses();
    protect_food();
    return 0; // retain the game's normal frame update
}

void growth(::Ctx* c, uint64_t, void*) {
    if (!adjustments_enabled(AdjustmentPage::Growth) || !player_unit(c->x[20])) return;
    uint8_t stat = 0;
    if (!read(c->x[19], &stat) || stat >= 9) return;
    int bonus = get(Setting::GrowthBonus);
    uint64_t user = 0, person = 0;
    bool overridden = false;
    if (read(state().base + 0x220ab10, &user) && user &&
        read(c->x[20] + 0x188, &person) && person) {
        const std::string pid = person_pid(person);
        int personal_bonus = INT_MAX;
        if (growth_value(c, user, pid, stat, &personal_bonus, false) &&
            personal_bonus != INT_MAX) {
            bonus = personal_bonus;
            overridden = true;
        }
    }
    if (!bonus) return;
    const int64_t adjusted = static_cast<int64_t>(sw(c, 9)) +
        bonus * (stat == 0 && !overridden ? 2 : 1);
    putw(c, 9, static_cast<int32_t>(std::clamp<int64_t>(adjusted, INT32_MIN, INT32_MAX)));
}

void growth_mode(::Ctx* c, uint64_t, void*) {
    const int mode = get(Setting::GrowthMode);
    if (mode < 0 || !player_unit(c->x[19])) return;
    // The next two native instructions test the returned fixed-growth
    // variable pointer and its value at +0x20. A mapped immutable stand-in
    // lets player units choose fixed growth without editing any save variable.
    c->x[0] = mode ? state().fixed_value : 0;
}

// The native difficulty value 2 is already implemented by the game. Its
// creation row and two message strings are absent from the retail menu.
void difficulty_third_row(::Ctx* c, uint64_t, void*) {
    State& s = state();
    uint64_t allocator = 0;
    if (fn_lookup(s.base + 0x652310) && fn_lookup(s.base + 0x1714cc0) &&
        read(c->x[23], &allocator) && allocator) {
        ::Ctx call = *c;
        call.x[0] = allocator;
        call.x[1] = 0x58;
        call.x[2] = 0x10;
        call.x[3] = 0;
        dispatch_call(&call, s.base + 0x652310);
        const uint64_t item = call.x[0];
        if (item && mem::is_mapped(item, 0x58)) {
            const uint16_t zero16 = 0;
            const uint32_t zero32 = 0, mode = 0x02010101;
            const uint64_t zero64 = 0;
            write(item, c->x[20]);
            write(item + 8, zero64);
            write(item + 0x10, zero16);
            write(item + 0x28, zero32);
            for (int offset : {0x30, 0x38, 0x40, 0x48}) write(item + offset, zero64);
            write(item + 0x50, mode);
            call = *c;
            call.x[0] = c->x[19];
            call.x[1] = item;
            dispatch_call(&call, s.base + 0x1714cc0);
        }
    }
    // Reproduce the displaced LDP before the following native CMP.
    read(c->x[19] + 0x100, &c->x[9]);
    read(c->x[19] + 0x108, &c->x[8]);
}
void difficulty_name(::Ctx* c, uint64_t, void*) {
    State& s = state();
    const bool lunatic = c->x[22] == s.base + 0x1b2875a && s.difficulty_text;
    const uint64_t target = s.base + (lunatic ? 0x1594ed0 : 0x1594de0);
    if (!fn_lookup(target)) return;
    ::Ctx call = *c;
    call.x[30] = s.base + 0xa1cc74;
    if (lunatic) call.x[2] = s.difficulty_text;
    dispatch_call(&call, target);
    c->x[0] = call.x[0];
}
void difficulty_help(::Ctx* c, uint64_t, void*) {
    State& s = state();
    if (c->x[22] == s.base + 0x1b2875a && s.difficulty_text) {
        c->x[0] = s.difficulty_text + 0x80;
        return;
    }
    const uint64_t target = s.base + 0xa60820;
    if (!fn_lookup(target)) return;
    ::Ctx call = *c;
    call.x[30] = s.base + 0xa1cd08;
    dispatch_call(&call, target);
    c->x[0] = call.x[0];
}

// The save preview uses the third native difficulty message ID, whose CNzh
// text is empty. Its data already contains difficulty 2; supply visible text.
void save_preview_difficulty(::Ctx* c, uint64_t, void*) {
    State& s = state();
    uint64_t user = 0;
    uint8_t difficulty = 0xff;
    const bool lunatic = read(s.base + 0x220ab10, &user) && user &&
                         read(user + 0x0d, &difficulty) && difficulty == 2 &&
                         s.save_difficulty_text;
    const uint64_t target = s.base + (lunatic ? 0x1594ed0 : 0x1594de0);
    if (!fn_lookup(target)) return;
    ::Ctx call = *c;
    call.x[30] = s.base + 0x16dbf4c;
    if (lunatic) call.x[2] = s.save_difficulty_text;
    dispatch_call(&call, target);
    c->x[0] = call.x[0];
}

// Only the game's return-exam jobs have a unique certificate. Ordinary exam
// tickets and all other job requirements remain under native control.
bool reusable_class(uint64_t job) {
    uint8_t kind = 0;
    uint64_t certificate = 0;
    return read(job + 0x1f9, &kind) && kind == 1 &&
           read(job + 0x1e0, &certificate) && certificate;
}

bool player_has_class_qualification(::Ctx* context, uint64_t job) {
    State& s = state();
    std::lock_guard<std::mutex> lookup_lock(s.class_lookup_mutex);
    uint64_t manager = 0, units = 0, extra = 0, base = 0;
    uint16_t index = 0;
    if (!s.class_lookup_id || !fn_lookup(s.base + 0x6ec380) ||
        !read(s.base + 0x220a350, &manager) || !manager ||
        !read(manager + 0x38, &units) || !units ||
        !read(job + 0x18, &extra) || !extra ||
        !read(extra + 0x450, &base) || !base ||
        !read(base + 0x10, &index)) return false;
    for (unsigned id = 1; id <= 255; ++id) {
        if (!write(s.class_lookup_id, static_cast<uint8_t>(id))) return false;
        ::Ctx call = *context;
        call.x[0] = s.class_lookup_id;
        dispatch_call(&call, s.base + 0x6ec380);
        const uint64_t unit = call.x[0];
        uint64_t person = 0, force = 0, begin = 0, end = 0;
        uint8_t kind = 0;
        uint16_t flags = 0;
        if (!read(unit + 0x188, &person) || !person ||
            !read(unit + 0x1b8, &force) || !force ||
            !read(force + 0x10, &kind) || kind > 7 || !((1u << kind) & 0xf1u) ||
            !read(unit + 0x230, &begin) || !begin ||
            !read(unit + 0x238, &end) || end < begin || end - begin > 8192 ||
            ((end - begin) & 1) || index >= (end - begin) / 2 ||
            !read(begin + index * 2, &flags)) continue;
        if (flags & 0x400u) return true;
    }
    return false;
}

void class_certificate_count(::Ctx* c, uint64_t, void*) {
    if (get(Setting::SpecialClassCount) && sw(c, 0) == 0 &&
        reusable_class(c->x[27]) && player_has_class_qualification(c, c->x[27]))
        putw(c, 0, 1);
}

void class_missing_ticket(::Ctx* c, uint64_t, void*) {
    if (get(Setting::SpecialClassCount) && (w(c, 25) & 8u) &&
        reusable_class(c->x[20]) && player_has_class_qualification(c, c->x[20]))
        putw(c, 25, static_cast<int32_t>(w(c, 25) & ~8u));
}

void class_preserve_certificate(::Ctx* c, uint64_t, void*) {
    if (!get(Setting::SpecialClassCount) || !c->x[1]) return;
    uint64_t unit = 0, job = 0;
    if (!read(c->x[19] + 0x98, &unit) ||
        !read(c->x[19] + 0xa0, &job) || !reusable_class(job)) return;
    ::Ctx call = *c;
    call.x[0] = unit;
    call.x[1] = job;
    dispatch_call(&call, state().base + 0x96ae50);
    if (static_cast<int32_t>(static_cast<uint32_t>(call.x[0])) > 0 ||
        player_has_class_qualification(c, job)) c->x[1] = 0;
}

void class_suppress_refund(::Ctx* c, uint64_t, void*) {
    if (!get(Setting::SpecialClassCount)) return;
    uint64_t job = 0;
    if (read(c->x[19] + 0xa0, &job) && reusable_class(job)) c->x[8] = 0;
}

void support(::Ctx* c, uint64_t, void*) {
    if (actions::consume_gift_override()) {
        uint16_t current = 0;
        read(c->x[19] + 0xc, &current);
        const int32_t raw = sw(c, 23);
        const int32_t delta = raw + (raw < 0);
        NXLOG_I("coremods", "gift support event: prior=%u native_delta=%d, standing multiplier bypassed",
                current, delta);
        return;
    }
    const int rate = get(Setting::Support);
    if (rate == 5) return;
    // The native Add has already rounded compatibility. At this site w23 plus
    // its sign bit is the native signed delta; w21 picks the gain/loss path.
    const int32_t raw = sw(c, 23);
    const int32_t delta = raw + (raw < 0);
    if (delta <= 0) return; // original deductions are never multiplied
    const int32_t scaled = scale_positive(delta, rate);
    if (scaled > 0) {
        putw(c, 23, scaled);
        putw(c, 21, 100);
    } else if (scaled < 0) {
        putw(c, 23, scaled - 1); // the following native ADD adds the sign bit
        putw(c, 21, -100);
    } else {
        putw(c, 23, 0);
        putw(c, 21, 100);
    }
}

// Keep the game's own rank floor and cap when changing mount support.
int mount_delta(uint64_t ride, uint64_t person, int delta) {
    const int rate = get(Setting::Mount);
    if (rate == 5 || delta <= 0 || !ride || !person) return delta;
    uint64_t node = 0, begin = 0, end = 0;
    uint8_t active = 0, key = 0;
    if (!read(ride, &active) || !active ||
        !read(person + 0x30, &key) || !read(ride + 0x18, &node) ||
        !read(state().base + 0x2219a38, &begin) ||
        !read(state().base + 0x2219a40, &end) ||
        !begin || end <= begin || end - begin > 256 || (end - begin) % 4) return delta;
    int current = 0;
    bool found = false;
    for (unsigned i = 0; node && i < 256; ++i) {
        uint8_t found_key = 0;
        if (!read(node + 0x1a, &found_key)) return delta;
        if (found_key == key) {
            uint16_t raw = 0;
            if (!read(node + 0x1c, &raw)) return delta;
            current = raw & 0xfff;
            found = true;
            break;
        }
        if (!read(node + (found_key < key ? 8 : 0), &node)) return delta;
    }
    if (node && !found) return delta; // malformed or cyclic tree
    int floor = 0, cap = 0;
    for (uint64_t ptr = begin; ptr < end; ptr += 4) {
        int threshold = 0;
        if (!read(ptr, &threshold) || threshold < cap || threshold > 4095) return delta;
        cap = threshold;
        if (threshold <= current) floor = threshold;
    }
    int scaled = scale_positive(delta, rate);
    scaled = std::max(scaled, floor - current);
    if (scaled > 0) scaled = std::min(scaled, std::max(0, cap - current));
    return scaled;
}

void mount_feed(::Ctx* c, uint64_t, void*) {
    uint64_t person = 0;
    if (read(c->x[1], &person)) putw(c, 2, mount_delta(c->x[0], person, sw(c, 2)));
}

void mount_result(uint64_t result, uint64_t unit) {
    int32_t delta = 0;
    uint64_t person = 0;
    if (!read(result + 0x10, &delta) || delta <= 0 ||
        !read(unit + 0x188, &person) || !person) return;
    const int adjusted = mount_delta(unit + 0x758, person, delta);
    write(result + 0x10, adjusted);
}

int mastery_cap() {
    uint64_t begin = 0, end = 0;
    if (!read(state().base + 0x2219a58, &begin) ||
        !read(state().base + 0x2219a60, &end) ||
        !begin || end <= begin || end - begin > 256 || (end - begin) % 4) return 65535;
    const uint64_t count = std::min<uint64_t>((end - begin) / 4, 12);
    if (!count) return 65535;
    int cap = 0;
    if (!read(begin + (count - 1) * 4, &cap)) return 65535;
    return std::clamp(cap, 0, 65535);
}

void points_adjust(::Ctx* c, uint64_t result) {
    uint64_t unit = 0;
    if (!read(result, &unit) || !player_unit(unit)) return;
    mount_result(result, unit);
    const int experience = get(Setting::Experience);
    const int class_rate = get(Setting::ClassMastery);
    const int weapon = get(Setting::Weapon), skill = get(Setting::Skill);
    int32_t xp = 0, class_mastery = 0, mount = 0;
    if (!read(result + 8, &xp) || !read(result + 0xc, &class_mastery) ||
        !read(result + 0x10, &mount)) return;
    const int32_t xp_scaled = experience != 5 && xp > 0 ? scale_positive(xp, experience) : xp;
    const bool xp_changed = xp_scaled != xp;
    const int32_t class_scaled = class_rate != 5 && class_mastery > 0
        ? scale_positive(class_mastery, class_rate) : class_mastery;
    const bool class_changed = class_scaled != class_mastery;
    if (!xp_changed && !class_changed && weapon == 5 && skill == 5) return;
    const uint64_t clamp_addr = state().base + 0x8cd4a0;
    if (!fn_lookup(clamp_addr)) return;
    const int cap = mastery_cap();
    int32_t old[13] = {}, adjusted[13] = {};
    unsigned changed = 0;
    for (unsigned i = 0; i < 13; ++i) {
        const uint64_t field = result + 0x14 + i * 4;
        if (!read(field, &old[i])) return;
        adjusted[i] = old[i];
        const int rate = i < 7 ? weapon : skill;
        if (rate == 5 || old[i] <= 0) continue;
        uint16_t current = 0;
        if (!read(unit + 0x202 + i * 2, &current)) return;
        int value = scale_positive(old[i], rate);
        value = std::max(value, -static_cast<int>(current));
        if (value > 0) value = std::min(value, std::max(0, cap - static_cast<int>(current)));
        adjusted[i] = value;
        changed |= 1u << i;
    }
    if (!xp_changed && !class_changed && !changed) return;
    if (xp_changed) write(result + 8, xp_scaled);
    if (class_changed) write(result + 0xc, class_scaled);
    for (unsigned i = 0; i < 13; ++i)
        if (changed & (1u << i)) write(result + 0x14 + i * 4, adjusted[i]);
    // The native clamp enforces the story level ceiling and mastery caps.
    // Preserve unrelated reward groups after it runs.
    ::Ctx nested = *c;
    nested.x[0] = result;
    dispatch_call(&nested, clamp_addr);
    if (xp_changed) {
        int32_t applied = 0;
        if (read(result + 8, &applied))
            NXLOG_I("coremods", "experience award: original=%d scaled=%d applied=%d (rate %.1fx)",
                    xp, xp_scaled, applied, experience / 5.0);
    } else write(result + 8, xp);
    if (class_changed) {
        int32_t applied = 0;
        if (read(result + 0xc, &applied))
            NXLOG_I("coremods", "class mastery award: original=%d scaled=%d applied=%d (rate %.1fx)",
                    class_mastery, class_scaled, applied, class_rate / 5.0);
    }
    if (!class_changed) write(result + 0xc, class_mastery);
    write(result + 0x10, mount);
    for (unsigned i = 0; i < 13; ++i)
        if (!(changed & (1u << i))) write(result + 0x14 + i * 4, old[i]);
}

void points_copy(::Ctx* c, uint64_t, void*) {
    if (mem::is_mapped(c->x[25], 0x5b)) points_adjust(c, c->x[25]);
}

bool points_apply_copy(::Ctx* c, uint64_t source, uint64_t return_addr) {
    if (get(Setting::Weapon) == 5 && get(Setting::Skill) == 5 &&
        get(Setting::Mount) == 5 && get(Setting::Experience) == 5 &&
        get(Setting::ClassMastery) == 5) return false;
    const uint64_t apply_addr = state().base + 0x8cd5a0;
    if (!source || !mem::is_mapped(source, 0x5b) || !fn_lookup(apply_addr)) return false;
    uint64_t unit = 0;
    if (!read(source, &unit) || !player_unit(unit)) return false;
    const uint64_t scratch = (c->x[31] - 0x100) & ~0xfull;
    const uint64_t copy = scratch + 0x80;
    if (!mem::is_mapped(scratch, 0x100)) return false;
    std::memcpy(reinterpret_cast<void*>(copy), reinterpret_cast<const void*>(source), 0x5b);
    ::Ctx nested = *c;
    nested.x[31] = scratch;
    points_adjust(&nested, copy);
    nested.x[0] = copy;
    nested.x[30] = return_addr;
    dispatch_call(&nested, apply_addr);
    const uint64_t caller_sp = c->x[31];
    *c = nested;
    c->x[31] = caller_sp;
    return true;
}

void points_direct(::Ctx* c, uint64_t address, void*) {
    if (points_apply_copy(c, c->x[0], address + 4)) return;
    c->x[30] = address + 4;
    dispatch_call(c, state().base + 0x8cd5a0);
}

int points_direct_tail(::Ctx* c, uint64_t, void*) {
    uint64_t source = 0;
    if (!read(c->x[0] + 0xc0, &source)) return 0;
    return points_apply_copy(c, source, c->x[30]) ? 1 : 0;
}

void fragment(::Ctx* c, uint64_t, void*) {
    const int rate = get(Setting::Fragment);
    if (rate == 5) putw(c, 9, static_cast<int32_t>(w(c, 10) + w(c, 9)));
    else putw(c, 9, fragment_total(sw(c, 10), sw(c, 9), rate));
}

struct MoneySite { uint64_t address; unsigned descriptor; };
constexpr MoneySite kMoneySites[] = {
    {0x8b6e2c, 0x8a8}, {0x8b7490, 0x8a8}, {0x8b89ac, 0x199},
    {0x8f4c2c, 0x9a9}, {0x8fbf6c, 0xaba}, {0xe1e414, 0x199},
    {0x10f9ffc, 0x1399}, {0x10fa2b4, 0x1399}, {0x11e2fc8, 0x1399},
    {0x1278a74, 0x199}, {0x1287dc4, 0x9a9}, {0x12dbaa8, 0xa88},
    {0x14064c0, 0x9a9}, {0x14e0188, 0x9a9}, {0x15578d8, 0x9a9},
    {0x15693b8, 0xaba}, {0x17378c8, 0x9b9}, {0x173dad4, 0x2ab8},
    {0x1999f5c, 0x8a8}, {0x19b5af4, 0x199},
};

void money(::Ctx* c, uint64_t, void* user) {
    const int rate = get(Setting::Money);
    const auto descriptor = static_cast<const MoneySite*>(user)->descriptor;
    const unsigned destination = descriptor & 15u;
    const int32_t current = sw(c, (descriptor >> 4) & 15u);
    uint32_t raw = w(c, (descriptor >> 8) & 31u);
    if (descriptor & 0x2000u) raw *= w(c, 8);
    const int32_t delta = static_cast<int32_t>(raw);
    if (delta <= 0 || rate == 5) { putw(c, destination, static_cast<int32_t>(static_cast<uint32_t>(current) + raw)); return; }
    const int64_t product = static_cast<int64_t>(delta) * rate;
    const int64_t amount = product > 0 ? (product + 4) / 5 : product / 5;
    putw(c, destination, static_cast<int32_t>(std::clamp<int64_t>(static_cast<int64_t>(current) + amount, 0, 9999999)));
}

void remember(uint32_t hook) {
    State& s = state();
    if (hook && s.hook_count < s.hooks.size()) s.hooks[s.hook_count++] = hook;
}
}  // namespace

int configured(Setting setting) { load(); return state().values[index(setting)].load(); }
int get(Setting setting) {
    const int value = configured(setting);
    if (setting == Setting::GrowthBonus && !state().growth_enabled.load()) return 0;
    if (setting >= Setting::Experience && setting <= Setting::ClassLevelAdjustment &&
        !state().rates_enabled.load()) {
        if (setting <= Setting::Money) return 5;
        return setting == Setting::MoveCost ? 20 : 0;
    }
    return value;
}
bool adjustments_enabled(AdjustmentPage page) {
    load();
    return page == AdjustmentPage::Rates ? state().rates_enabled.load() : state().growth_enabled.load();
}
bool set_adjustments_enabled(AdjustmentPage page, bool enabled, std::string* error) {
    load();
    const char* key = page == AdjustmentPage::Rates ? "rate_adjustments_enabled" : "growth_adjustments_enabled";
    if (!config_write("coremods", key, enabled ? "1" : "0", error)) return false;
    auto& state_flag = page == AdjustmentPage::Rates ? state().rates_enabled : state().growth_enabled;
    const bool previous = state_flag.exchange(enabled);
    if (page == AdjustmentPage::Rates && previous != enabled && configured(Setting::MoveCost) != 20)
        state().move_revision.fetch_add(1);
    return true;
}
int minimum(Setting setting) { return kSpecs[index(setting)].lo; }
int maximum(Setting setting) { return kSpecs[index(setting)].hi; }
int initial(Setting setting) { return kSpecs[index(setting)].def; }
const char* label(Setting setting) { return kSpecs[index(setting)].name; }

std::vector<GrowthCharacter> growth_characters() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.growth_mutex);
    return s.growth_roster;
}
std::string growth_character_label(const std::string& pid) {
    // Keep the exact PID for save keys. The extra prefix occurs in some game
    // records, and a selected character may temporarily leave the roster.
    const std::string display_pid = pid.rfind("PID_PID_", 0) == 0 ? pid.substr(4) : pid;
    for (const auto& name : kGrowthNames)
        if (display_pid == name.first) return name.second;
    return display_pid.rfind("PID_", 0) == 0 ? display_pid.substr(4) : display_pid;
}
GrowthSelection growth_selection() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.growth_mutex);
    return s.growth_selected;
}
void select_growth_character(const std::string& pid) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.growth_mutex);
    if (s.growth_selected.pid != pid) {
        s.growth_selected = {};
        s.growth_selected.pid = pid;
    }
}
bool set_growth_bonus(const std::string& pid, int stat, int bonus) {
    State& s = state();
    if (stat < 0 || stat >= 9 ||
        (bonus != INT_MAX && (bonus < -1000 || bonus > 1000))) return false;
    std::lock_guard<std::mutex> lock(s.growth_mutex);
    if (!s.growth_user || !s.growth_selected.loaded || s.growth_selected.pid != pid) return false;
    s.growth_pending.push_back({s.growth_user, pid, stat, bonus});
    s.growth_selected.bonus[stat] = bonus;
    return true;
}
bool reset_growth_bonuses() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.growth_mutex);
    if (!s.growth_user || s.growth_roster.empty()) return false;
    s.growth_pending.clear();
    s.growth_reset_pending = true;
    s.growth_reset_result.store(2);
    s.growth_selected.bonus.fill(INT_MAX);
    s.growth_selected.loaded = false;
    return true;
}
int take_growth_reset_result() {
    auto& result = state().growth_reset_result;
    const int value = result.load();
    if (value == 1 || value == -1) result.store(0);
    return value == 2 ? 0 : value;
}

bool set(Setting setting, int value, std::string* error) {
    load();
    value = clamp(setting, value);
    const int previous = get(setting);
    if (!config_write("coremods", kSpecs[index(setting)].key, std::to_string(value), error)) return false;
    state().values[index(setting)].store(value);
    if (setting == Setting::MoveCost && value != previous) state().move_revision.fetch_add(1);
    return true;
}

int scale_positive(int delta, int fifths) {
    if (delta <= 0) return delta;
    const int64_t product = static_cast<int64_t>(delta) * std::clamp(fifths, -50, 50);
    const int64_t result = product >= 0 ? (product + 4) / 5 : product / 5;
    return static_cast<int>(std::clamp<int64_t>(result, INT32_MIN, INT32_MAX));
}

int fragment_total(int current, int delta, int fifths) {
    int64_t amount = delta;
    if (delta > 0) {
        amount *= std::clamp(fifths, -50, 50);
        amount = amount >= 0 ? (amount + 4) / 5 : amount / 5;
    }
    return static_cast<int>(std::clamp<int64_t>(static_cast<int64_t>(current) + amount, 0, 99999));
}

bool renown_available() { return state().renown_user.load() && state().renown_limit.load() > 0; }
int renown_current() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.renown_mutex);
    return s.pending_value >= 0 ? s.pending_value : s.renown_value.load();
}
int renown_cap() { return state().renown_limit.load(); }
bool set_renown(int value) {
    State& s = state();
    const uint64_t user = s.renown_user.load();
    const int cap = s.renown_limit.load();
    if (!user || cap <= 0) return false;
    std::lock_guard<std::mutex> lock(s.renown_mutex);
    s.pending_user = user;
    s.pending_value = std::clamp(value, 0, cap);
    return true;
}

void renown_frame(::Ctx* context) {
    State& s = state();
    uint64_t user = 0;
    if (!read(s.base + 0x220ab10, &user) || !user) {
        s.renown_user.store(0);
        s.renown_limit.store(0);
        return;
    }
    int chapter = 0, phase = 0, current = 0;
    if (!read(user + 0x3c, &chapter) || !read(user + 0x40, &phase) ||
        !read(user + 0x4c, &current)) return;
    const uint64_t table = s.base + (chapter == 3 && phase > 1 ? 0x2238298 : 0x2238278);
    uint64_t begin = 0, end = 0;
    if (!read(table + 8, &begin) || !read(table + 16, &end) ||
        !begin || end <= begin || end - begin > 4096 || (end - begin) % 4) return;
    int cap = 0;
    if (!read(end - 4, &cap) || cap <= 0 || cap > 99999999) return;
    s.renown_value.store(current);
    s.renown_limit.store(cap);
    s.renown_user.store(user);
    int pending = -1;
    {
        std::lock_guard<std::mutex> lock(s.renown_mutex);
        if (s.pending_value >= 0) {
            if (s.pending_user == user) pending = std::clamp(s.pending_value, 0, cap);
            s.pending_value = -1;
            s.pending_user = 0;
        }
    }
    if (pending < 0 || !fn_lookup(s.base + 0x7188a0)) return;
    ::Ctx call = *context;
    call.x[0] = static_cast<uint32_t>(pending);
    dispatch_call(&call, s.base + 0x7188a0);
    if (read(user + 0x4c, &current)) s.renown_value.store(current);
    NXLOG_I("coremods", "renown adjusted: requested=%d current=%d cap=%d", pending, current, cap);
}

// Activity shortcuts operate at the same native stages as the game's own
// UI. A disabled switch always leaves the original function untouched.
int bird_question(::Ctx* c, uint64_t, void*) {
    if (!get(Setting::QuickBird)) return 0;
    const uint64_t scene = c->x[0];
    uint64_t questions = 0;
    if (!read(scene + 0xf0 + 0x18, &questions) || !questions ||
        !fn_lookup(state().base + 0x1622640)) return 0;
    std::array<uint64_t, 3> answers{};
    for (unsigned i = 0; i < answers.size(); ++i)
        if (!read(questions + 0x48 + 16 * i, &answers[i]) || !answers[i]) return 0;
    for (unsigned i = 0; i < answers.size(); ++i)
        if (!mem::is_mapped(scene + 0xf0 + 0x20 + 24 * i, 24)) return 0;
    for (unsigned i = 0; i < answers.size(); ++i) {
        const uint64_t record = scene + 0xf0 + 0x20 + 24 * i;
        const int32_t number = static_cast<int32_t>(i + 1);
        write(record, number);
        write(record + 8, answers[i]);
        write(record + 16, answers[i]);
    }
    ::Ctx call = *c;
    dispatch_call(&call, state().base + 0x1622640);
    c->x[0] = call.x[0];
    return 1;
}

void training_grade(::Ctx* c, uint64_t, void*) {
    if (get(Setting::QuickTraining) && (c->x[0] & 0xff)) putw(c, 0, 4);
}
int training_motion(::Ctx*, uint64_t, void*) {
    return get(Setting::QuickTraining) != 0;
}

void meal_grade(::Ctx* c, uint64_t, void*) {
    if (!get(Setting::QuickMeal)) return;
    const uint64_t args = c->x[0];
    if (!mem::is_mapped(args + 0x31, 3)) return;
    const uint8_t favourite = 3;
    for (unsigned i = 0; i < 3; ++i) write(args + 0x31 + i, favourite);
}
int meal_motion(::Ctx* c, uint64_t, void*) {
    if (!get(Setting::QuickMeal)) return 0;
    const uint64_t scene = c->x[0];
    uint64_t cursor = 0;
    if (!read(scene + 0x10, &cursor) || cursor != state().base + 0x21a6f20)
        return 0;
    return write(scene + 0x10, cursor + 0x40) ? 1 : 0;
}

void service_grade(::Ctx* c, uint64_t, void*) {
    if (get(Setting::QuickService) && (c->x[0] & 0xff)) putw(c, 0, 3);
}
int service_motion(::Ctx*, uint64_t, void*) {
    return get(Setting::QuickService) != 0;
}

int capture_result(::Ctx* c, uint64_t, void*) {
    if (!get(Setting::QuickCapture)) return 0;
    uint64_t data = 0, animal = 0;
    if (!read(c->x[0] + 0xd8, &data) || !data ||
        !read(data + 0x48, &animal)) return 0;
    int32_t result = 0;
    if (!read(data + 0x40, &result)) return 0;
    if (!result && fn_lookup(state().base + 0x1546740)) {
        ::Ctx call = *c;
        call.x[0] = data;
        dispatch_call(&call, state().base + 0x1546740);
        if (!read(data + 0x48, &animal)) return 0;
    }
    if (animal) {
        const int32_t success = 1;
        write(data + 0x40, success);
    }
    return 0;
}
int capture_motion(::Ctx* c, uint64_t, void*) {
    if (!get(Setting::QuickCapture)) return 0;
    uint64_t cursor = 0;
    if (!read(c->x[0] + 0x10, &cursor) ||
        cursor != state().base + 0x2261350) return 0;
    const uint64_t cleanup = state().base + 0x2261790;
    return write(c->x[0] + 0x10, cleanup) ? 1 : 0;
}

// Gmap Explore chooses BAD/GOOD/PERFECT from one random roll against two
// cumulative thresholds. Keep the native reward and time-commit path intact:
// only make the roll meet each threshold when the switch is enabled.
void explore_first_threshold(::Ctx* c, uint64_t, void*) {
    uint32_t threshold = 0;
    if (!read(c->x[24] + 0x20, &threshold)) return;
    putw(c, 9, static_cast<int32_t>(threshold)); // native ldr w9, [x24,#0x20]
    if (get(Setting::PerfectExplore)) putw(c, 8, static_cast<int32_t>(threshold));
}
void explore_final_threshold(::Ctx* c, uint64_t, void*) {
    const uint32_t threshold = w(c, 10) + w(c, 9); // native add w9,w10,w9
    putw(c, 9, static_cast<int32_t>(threshold));
    if (get(Setting::PerfectExplore)) putw(c, 8, static_cast<int32_t>(threshold));
}

// The native activity selectors return a bitmask of rejection reasons.
// Bit 17 alone means that this week's allowance was used. Leave every other
// reason intact (unlock, money, ingredients, participants and so on).
void weekly_reason(::Ctx* c, uint64_t, void* site) {
    if (c->x[0] & 0x20000ull) {
        static std::atomic<unsigned> samples{0};
        if (samples.fetch_add(1) < 24)
            NXLOG_I("weekly", "selector site=%llx reason=%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(site)), static_cast<unsigned long long>(c->x[0]));
    }
    if (get(Setting::UnlimitedWeeklyActivities)) c->x[0] &= ~0x20000ull;
}

// Both the gift menu and the gift action compare the saved next-available
// date in w8 against the current date in w9. Keep their other checks intact.
void weekly_gift_date(::Ctx* c, uint64_t, void* site) {
    static std::atomic<unsigned> samples{0};
    if (samples.fetch_add(1) < 12)
        NXLOG_I("weekly", "gift date site=%llx saved=%u now_prefix=%u", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(site)), w(c, 8), w(c, 10));
    if (get(Setting::UnlimitedWeeklyActivities)) c->x[8] = 0;
}

struct FlameSite { uint64_t address; uint8_t object_reg; uint8_t value_reg; };
constexpr FlameSite kFlameSites[] = {
    {0x93532c, 20, 3}, {0x93539c, 20, 3}, {0x93540c, 20, 3},
    {0x954a9c, 1, 3},
    {0x1406494, 1, 3}, {0x1406dc8, 1, 3}, {0x1406e20, 1, 3},
    {0x1408078, 1, 3}, {0x1437250, 19, 3},
    {0x1447e54, 1, 3}, {0x1448b38, 1, 3}, {0x14550d8, 1, 3},
    {0x1999b9c, 19, 3},
};

// The native activity handlers store each character's remaining flames at
// +0x82a. Preserve the current byte only when an activity would reduce it;
// native weekly resets and increases remain untouched. The hook runs just
// before the original strb, so the store itself still follows the game path.
void preserve_activity_flame(::Ctx* c, uint64_t, void* data) {
    if (!get(Setting::PreserveActivityFlames)) return;
    const auto& site = *static_cast<const FlameSite*>(data);
    uint8_t current = 0;
    if (!read(c->x[site.object_reg] + 0x82a, &current)) return;
    const uint8_t next = static_cast<uint8_t>(c->x[site.value_reg]);
    if (current <= 3 && next < current)
        c->x[site.value_reg] = (c->x[site.value_reg] & ~0xffull) | current;
}

int weekly_message_probe(::Ctx* c, uint64_t, void*) {
    if (c->x[1] & 0x20000ull) {
        static std::atomic<unsigned> samples{0};
        if (samples.fetch_add(1) < 32)
            NXLOG_I("weekly", "cause caller=%llx bits=%llx", static_cast<unsigned long long>(c->x[30] - state().base), static_cast<unsigned long long>(c->x[1]));
    }
    return 0;
}

void install(uint64_t main_base) {
    load();
    State& s = state();
    s.base = main_base;
    s.move_storage = mem::alloc_kernel(0x100);
    s.move_key_ready = false;
    s.fixed_value = mem::alloc_kernel(0x100);
    s.difficulty_text = mem::alloc_kernel(0x100);
    s.save_difficulty_text = mem::alloc_kernel(0x20);
    if (s.save_difficulty_text && mem::is_mapped(s.save_difficulty_text, 0x20)) {
        constexpr char name[] = "超难";
        std::memcpy(reinterpret_cast<void*>(s.save_difficulty_text), name, sizeof(name));
    }
    if (s.difficulty_text && mem::is_mapped(s.difficulty_text, 0x100)) {
        constexpr char name[] = "超难（Lunatic）";
        constexpr char help[] = "游戏原生第三档难度。仅在新游戏或再次降临时选择，并随存档保存。";
        static_assert(sizeof(name) <= 0x80 && sizeof(help) <= 0x80);
        std::memcpy(reinterpret_cast<void*>(s.difficulty_text), name, sizeof(name));
        std::memcpy(reinterpret_cast<void*>(s.difficulty_text + 0x80), help, sizeof(help));
    }
    s.class_lookup_id = mem::alloc_kernel(0x100);
    if (s.fixed_value) {
        const int32_t one = 1;
        write(s.fixed_value + 0x20, one);
        remember(hook_add_inline(main_base + 0x6e26e0, &growth_mode));
    }
    remember(hook_add_inline(main_base + 0x6d79f8, &growth));
    remember(hook_add_inline(main_base + 0xa1bd20, &difficulty_third_row));
    remember(hook_add_inline(main_base + 0xa1cc70, &difficulty_name));
    remember(hook_add_inline(main_base + 0xa1cd04, &difficulty_help));
    remember(hook_add_inline(main_base + 0x16dbf48, &save_preview_difficulty));
    remember(hook_add_inline(main_base + 0x6d9d20, &recruit_prepare));
    remember(hook_add_inline(main_base + 0x6d9d24, &recruit_prepared));
    remember(hook_add_inline(main_base + 0x6da024, &recruit_prepare));
    remember(hook_add_inline(main_base + 0x6da028, &recruit_prepared));
    remember(hook_add_inline(main_base + 0xf89618, &recruit_floor));
    remember(hook_add_inline(main_base + 0x14fa098, &move_date));
    remember(hook_add_inline(main_base + 0x14fa034, &move_immediate));
    remember(hook_add_inline(main_base + 0x96a7a8, &class_success));
    remember(hook_add_inline(main_base + 0x676bac, &class_level));
    remember(hook_add_inline(main_base + 0x96c6f8, &class_chapter_rank));
    remember(hook_add_inline(main_base + 0x1889980, &class_chapter_menu));
    remember(hook_add_inline(main_base + 0x1613534, &class_chapter_list));
    remember(hook_add_inline(main_base + 0x96a9b0, &class_chapter_exam));
    remember(hook_add_inline(main_base + 0x149c0bc, &class_chapter_actor));
    remember(hook_add_inline(main_base + 0x6ea750, &class_weapon_equip));
    remember(hook_add_inline(main_base + 0x6eaac8, &class_weapon_use));
    remember(hook_add_inline(main_base + 0x15bbffc, &class_certificate_count));
    remember(hook_add_inline(main_base + 0x96accc, &class_missing_ticket));
    remember(hook_add_inline(main_base + 0x8bff40, &class_preserve_certificate));
    remember(hook_add_inline(main_base + 0x8c1b98, &class_suppress_refund));
    remember(hook_add_entry(main_base + 0x5d170, &renown_frame_hook));
    remember(hook_add_inline(main_base + 0x8ec69c, &support));
    remember(hook_add_inline(main_base + 0x1988a04, &mount_feed));
    remember(hook_add_inline(main_base + 0x7ff60c, &points_copy));
    remember(hook_add_inline(main_base + 0xdbaf0c, &points_direct));
    remember(hook_add_inline(main_base + 0x1561b40, &points_direct));
    remember(hook_add_entry(main_base + 0xfc2500, &points_direct_tail));
    for (uint64_t off : {0x89504cull, 0x1966ec0ull, 0x19673fcull})
        remember(hook_add_inline(main_base + off, &fragment));
    for (const MoneySite& site : kMoneySites)
        remember(hook_add_inline(main_base + site.address, &money, const_cast<MoneySite*>(&site)));
    remember(hook_add_entry(main_base + 0x1621c10, &bird_question));
    remember(hook_add_inline(main_base + 0x1452de4, &training_grade));
    remember(hook_add_entry(main_base + 0x19b0b00, &training_motion));
    remember(hook_add_inline(main_base + 0x934b70, &meal_grade));
    remember(hook_add_entry(main_base + 0x19a0150, &meal_motion));
    remember(hook_add_inline(main_base + 0x1447d5c, &service_grade));
    remember(hook_add_entry(main_base + 0x19b1f30, &service_motion));
    remember(hook_add_entry(main_base + 0x15031e0, &capture_result));
    remember(hook_add_entry(main_base + 0xf5a5f0, &capture_motion));
    remember(hook_add_inline(main_base + 0x1383f5c, &explore_first_threshold));
    remember(hook_add_inline(main_base + 0x138464c, &explore_final_threshold));
    for (uint64_t off : {0x954728ull, 0x954918ull, 0x954f98ull,
                         0x9559b8ull, 0x9561c4ull, 0x9563c4ull,
                         0x956b70ull, 0x956d24ull, 0x95711cull, 0x957308ull,
                         0x958ad4ull, 0x958fd0ull,
                         0x1426bb0ull, 0x1426bc8ull,
                         0x1960c20ull, 0x1960c34ull,
                         0x19b3258ull, 0x19b326cull})
        remember(hook_add_inline(main_base + off, &weekly_reason, reinterpret_cast<void*>(off)));
    for (uint64_t off : {0xeb2798ull, 0xeb289cull})
        remember(hook_add_inline(main_base + off, &weekly_gift_date, reinterpret_cast<void*>(off)));
    for (const FlameSite& site : kFlameSites)
        remember(hook_add_inline(main_base + site.address, &preserve_activity_flame,
                                 const_cast<FlameSite*>(&site)));
    remember(hook_add_entry(main_base + 0x161a470, &weekly_message_probe));
    NXLOG_I("coremods", "installed %zu progression sites", s.hook_count);
}

void remove() {
    State& s = state();
    while (s.hook_count) hook_remove(s.hooks[--s.hook_count]);
    s.renown_user.store(0);
    s.renown_limit.store(0);
    std::lock_guard<std::mutex> lock(s.renown_mutex);
    s.pending_user = 0;
    s.pending_value = -1;
    {
        std::lock_guard<std::mutex> growth_lock(s.growth_mutex);
        s.growth_user = 0;
        s.growth_roster.clear();
        s.growth_selected = {};
        s.growth_pending.clear();
        s.growth_reset_pending = false;
        s.growth_reset_result.store(0);
    }
}

}  // namespace nxrt::games::fefw::coremods
