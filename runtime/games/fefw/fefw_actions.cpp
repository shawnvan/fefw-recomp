// SPDX-License-Identifier: GPL-3.0-or-later
// F1 runtime actions equivalent to the existing inventory.add and
// friendship.adjust packages. The guest entry points and item hash were
// researched in FEFW Tool portable v1.0.0, attributed to Bilibili creator
// 桃園奈 (user-supplied attribution: https://space.bilibili.com/8292668/).
// This PC runtime implementation does not include those Python scripts.
#include "fefw_actions.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_set>
#include <utility>

#include "../../third_party/nlohmann/json.hpp"
#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::fefw::actions {
namespace {

constexpr uint64_t kLookup = 0x6ea2c0;
constexpr uint64_t kCommit = 0x8a25c0;
constexpr uint64_t kInventoryRoot = 0x216afb8;
constexpr uint64_t kGiftReaction = 0x10260e0;
constexpr uint64_t kScoreWriter = 0x8ec500;

struct State {
    uint64_t base = mem::kMainBase;
    std::mutex mutex;
    std::vector<ItemRequest> pending_items;
    bool direct = false;
    std::string item_message = "没有待添加的物品。";
    std::string gift_message = "没有待处理的送礼调整。";
    std::atomic<int> gift_amount{0};
    uint32_t hooks[3]{};
};
State& state() { static State s; return s; }

template <typename T> bool read(uint64_t address, T* output) {
    if (!address || !mem::is_mapped(address, sizeof(T))) return false;
    std::memcpy(output, reinterpret_cast<const void*>(address), sizeof(T));
    return true;
}

uint64_t item_hash(const std::string& iid) {
    uint64_t value = 0xcbf29ce484222325ull;
    for (unsigned char byte : iid) value = (value * 0x100000001b3ull) ^ byte;
    return value ? value : UINT64_MAX;
}

thread_local bool t_injected_commit = false;
thread_local int t_gift_amount = 0;
thread_local std::chrono::steady_clock::time_point t_gift_until{};
thread_local bool t_skip_support_rate = false;
thread_local std::chrono::steady_clock::time_point t_skip_until{};

void add_items(::Ctx* c, uint64_t inventory, uint64_t result_pointer,
               std::vector<ItemRequest> items) {
    State& s = state();
    const uint64_t lookup = s.base + kLookup;
    const uint64_t commit = s.base + kCommit;
    if (!fn_lookup(lookup) || !fn_lookup(commit)) {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.item_message = "添加失败：游戏物品功能不可用。";
        return;
    }
    const uint64_t scratch = (c->x[31] - 0x300) & ~0xfull;
    const uint64_t descriptor = scratch + 0x40, hashed = scratch + 0x50;
    if (!mem::is_mapped(scratch, 0x300)) {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.item_message = "添加失败：游戏运行状态不可用。";
        return;
    }
    int accepted = 0, invalid = 0, rejected = 0;
    for (const ItemRequest& item : items) {
        const uint64_t zero = 0, hash = item_hash(item.iid);
        std::memcpy(reinterpret_cast<void*>(descriptor), &zero, 8);
        std::memcpy(reinterpret_cast<void*>(hashed), &hash, 8);
        ::Ctx call = *c;
        call.x[31] = scratch;
        call.x[0] = descriptor;
        call.x[1] = hashed;
        dispatch_call(&call, lookup);
        uint16_t index = 0;
        if (!read(descriptor, &index) || !index) {
            ++invalid;
            NXLOG_W("coremods", "inventory.add: invalid iid %s", item.iid.c_str());
            continue;
        }
        call = *c;
        call.x[31] = scratch;
        call.x[0] = inventory;
        call.x[1] = descriptor;
        call.x[2] = static_cast<uint32_t>(item.quantity);
        call.x[8] = result_pointer;
        t_injected_commit = true;
        dispatch_call(&call, commit);
        t_injected_commit = false;
        int32_t slot = -1;
        if (read(result_pointer + 4, &slot) && slot >= 0) {
            ++accepted;
            NXLOG_I("coremods", "inventory.add: %s x%d (index %u, slot %d)",
                    item.iid.c_str(), item.quantity, index, slot);
        } else {
            ++rejected;
            NXLOG_W("coremods", "inventory.add: game rejected %s (index %u)",
                    item.iid.c_str(), index);
        }
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.item_message = "已入库 " + std::to_string(accepted) + " 种；游戏拒绝 " +
            std::to_string(rejected) + " 种；编号无效 " + std::to_string(invalid) +
            " 种。请在背包确认数量。";
    }
}

int inventory_commit(::Ctx* c, uint64_t, void*) {
    if (t_injected_commit) return 0;
    State& s = state();
    std::vector<ItemRequest> items;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.direct || s.pending_items.empty()) return 0;
        items = std::move(s.pending_items);
        s.item_message = "正在本次物品获取时添加所选物品……";
    }
    add_items(c, c->x[0], c->x[8], std::move(items));
    return 0; // the original purchase/pickup still runs normally
}

void gift_reaction(::Ctx*, uint64_t, void*) {
    State& s = state();
    const int pending = s.gift_amount.exchange(0);
    if (!pending) return;
    t_gift_amount = pending;
    t_gift_until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
}

void score_writer(::Ctx* c, uint64_t, void*) {
    if (!t_gift_amount) return;
    State& s = state();
    if (std::chrono::steady_clock::now() > t_gift_until) {
        t_gift_amount = 0;
        std::lock_guard<std::mutex> lock(s.mutex);
        s.gift_message = "送礼调整已过期，本次支援值未修改。";
        return;
    }
    const int amount = std::exchange(t_gift_amount, 0);
    c->x[1] = static_cast<uint32_t>(amount);
    t_skip_support_rate = true;
    t_skip_until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.gift_message = "本次送礼支援值已设为 " + std::to_string(amount) + "。";
    }
    NXLOG_I("coremods", "friendship.adjust: next gift amount %d", amount);
}

ItemCategory classify_item(int index) {
    if ((index >= 1 && index <= 640) || (index >= 768 && index <= 773) ||
        (index >= 780 && index <= 786) || (index >= 885 && index <= 891)) return ItemCategory::Weapon;
    if (index >= 988 && index <= 1006) return ItemCategory::Upgrade;
    // The two leftover ingredients are story items and stay in Other.
    if (index >= 1007 && index <= 1113 && index != 1020 && index != 1049) return ItemCategory::Cooking;
    if (index >= 1155 && index <= 1281) return ItemCategory::Gift;
    if (index >= 658 && index <= 679) return ItemCategory::Growth;
    return ItemCategory::Other;
}

std::vector<Item> load_catalog() {
    std::vector<Item> items;
    std::ifstream file("mods/item-add/items.json", std::ios::binary);
    if (!file) return items;
    try {
        const auto source = nlohmann::json::parse(file);
        if (!source.is_array()) return items;
        for (const auto& row : source) {
            if (!row.is_object() || !row.contains("iid") || !row["iid"].is_string()) continue;
            Item item;
            item.iid = row["iid"].get<std::string>();
            item.item_index = row.value("item_index", -1);
            item.can_add = row.value("can_add", true);
            item.monster_weapon = row.value("monster_weapon", false);
            item.magic_skill = row.value("magic_skill", false);
            item.category = item.monster_weapon ? ItemCategory::Unavailable :
                item.magic_skill ? ItemCategory::MagicUnavailable : classify_item(item.item_index);
            item.search_text = item.iid;
            for (const char* key : {"name_zh_cn", "name_jp", "name_zh_tw"}) {
                if (row.contains(key) && row[key].is_string()) {
                    const std::string candidate = row[key].get<std::string>();
                    if (item.name.empty()) item.name = candidate;
                    item.search_text += ' ';
                    item.search_text += candidate;
                }
            }
            items.push_back(std::move(item));
        }
    } catch (const std::exception& error) {
        NXLOG_W("coremods", "cannot read item catalog: %s", error.what());
    }
    return items;
}
}  // namespace

void install(uint64_t main_base) {
    State& s = state();
    s.base = main_base;
    s.hooks[0] = hook_add_entry(main_base + kCommit, &inventory_commit);
    s.hooks[1] = hook_add_inline(main_base + kGiftReaction, &gift_reaction);
    s.hooks[2] = hook_add_inline(main_base + kScoreWriter, &score_writer);
}

void remove() {
    State& s = state();
    for (uint32_t& hook : s.hooks) {
        if (hook) hook_remove(hook);
        hook = 0;
    }
    cancel_item();
    cancel_gift();
}

const std::vector<Item>& item_catalog() {
    static const std::vector<Item> catalog = load_catalog();
    return catalog;
}

bool queue_item(const std::string& iid, int quantity) {
    return queue_items({ItemRequest{iid, quantity}}, false);
}

bool queue_items(const std::vector<ItemRequest>& items, bool direct) {
    if (items.empty() || items.size() > 25) return false;
    std::unordered_set<std::string> seen;
    for (const ItemRequest& item : items) {
        if (item.iid.size() < 5 || item.iid.size() > 256 ||
            item.iid.rfind("IID_", 0) != 0 || item.quantity < 1 ||
            item.quantity > 500 || !seen.insert(item.iid).second) return false;
        const auto& catalog = item_catalog();
        const auto known = std::find_if(catalog.begin(), catalog.end(), [&](const Item& entry) {
            return entry.iid == item.iid;
        });
        if (known != catalog.end() && !known->can_add) return false;
    }
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.pending_items = items;
    s.direct = direct;
    s.item_message = direct ? "正在直接入库，请稍候……" :
        "已排队；购买或拾取一件普通物品后添加。";
    return true;
}

void direct_frame(::Ctx* c) {
    State& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.direct || s.pending_items.empty()) return;
    }
    uint64_t first = 0, second = 0, inventory = 0;
    if (!read(s.base + kInventoryRoot, &first) ||
        !read(first, &second) || !read(second + 8, &inventory) ||
        !mem::is_mapped(inventory, 0x50)) return;
    const uint64_t scratch = (c->x[31] - 0x300) & ~0xfull;
    if (!mem::is_mapped(scratch, 0x300)) return;
    std::vector<ItemRequest> items;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.direct || s.pending_items.empty()) return;
        items = std::move(s.pending_items);
        s.direct = false;
        s.item_message = "正在将所选物品直接入库……";
    }
    const uint64_t result_pointer = scratch + 0x60;
    const uint64_t zero = 0;
    std::memcpy(reinterpret_cast<void*>(result_pointer), &zero, sizeof(zero));
    add_items(c, inventory, result_pointer, std::move(items));
}

bool queue_gift(int amount) {
    if (amount < 1 || amount > 50000) return false;
    State& s = state();
    s.gift_amount.store(amount);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.gift_message = "已排队；送出一件普通礼物后生效。";
    return true;
}

void cancel_item() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.pending_items.clear();
    s.direct = false;
    s.item_message = "已取消添加物品。";
}
void cancel_gift() {
    State& s = state();
    s.gift_amount.store(0);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.gift_message = "已取消送礼调整。";
}
std::string item_status() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.item_message;
}
std::string gift_status() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.gift_message;
}

bool consume_gift_override() {
    if (!std::exchange(t_skip_support_rate, false)) return false;
    return std::chrono::steady_clock::now() <= t_skip_until;
}

}  // namespace nxrt::games::fefw::actions
