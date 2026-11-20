// SPDX-License-Identifier: GPL-3.0-or-later
#include "fefw_deployment.h"
#include "fs_romfs.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/log.h"

namespace nxrt::services {
namespace {
namespace pathfs = std::filesystem;
constexpr size_t kRow = 0x148;

uint32_t read32(const std::vector<uint8_t>& b, size_t at) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(b[at + i]) << (8 * i);
    return value;
}
uint64_t read64(const std::vector<uint8_t>& b, size_t at) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(b[at + i]) << (8 * i);
    return value;
}
void write32(std::vector<uint8_t>& b, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) b[at + i] = uint8_t(value >> (8 * i));
}
void write64(std::vector<uint8_t>& b, size_t at, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) b[at + i] = uint8_t(value >> (8 * i));
}

struct Layout {
    size_t data_end = 0, ptr_end = 0, labels = 0;
    uint32_t count = 0, refs = 0;
    std::vector<size_t> locations;
};
bool inspect(const std::vector<uint8_t>& b, Layout& l) {
    if (b.size() < 0x40 || read32(b, 0) != b.size()) return false;
    const uint64_t data_end = 0x20ull + read32(b, 4);
    const uint64_t ptr_end = data_end + 8ull * read32(b, 8);
    const uint64_t labels = ptr_end + 8ull * read32(b, 12);
    if (data_end > b.size() || ptr_end > b.size() || labels > b.size() || (data_end & 7)) return false;
    l.data_end = static_cast<size_t>(data_end);
    l.ptr_end = static_cast<size_t>(ptr_end);
    l.labels = static_cast<size_t>(labels);
    l.count = read32(b, 8);
    l.refs = read32(b, 12);
    l.locations.clear();
    l.locations.reserve(l.count);
    for (uint32_t i = 0; i < l.count; ++i) {
        const uint64_t at = read64(b, l.data_end + size_t(i) * 8) + 0x20;
        if (at < 0x20 || at + 8 > l.data_end) return false;
        l.locations.push_back(static_cast<size_t>(at));
    }
    return true;
}

size_t label_offset(const std::vector<uint8_t>& b, const Layout& l, const std::string& name) {
    const auto it = std::search(b.begin() + l.labels, b.end(), name.begin(), name.end());
    if (it == b.end() || it + name.size() == b.end() || it[name.size()] != 0) return 0;
    return static_cast<size_t>(it - b.begin());
}

struct Group {
    size_t object = 0, first = 0;
    uint16_t count = 0;
    size_t blank = 0;
    size_t selectable = 0;
    std::vector<std::pair<int, int>> positions;
};
bool player_group(const std::vector<uint8_t>& b, const Layout& l,
                  const std::string& name, Group& out) {
    const size_t label = label_offset(b, l, name);
    if (!label) return false;
    std::vector<size_t> refs;
    for (size_t at : l.locations)
        if (read64(b, at) == label - 0x20) refs.push_back(at);
    if (refs.empty() || refs.front() < 0x28) return false;
    const size_t object = refs.front() - 8;
    if (object + 0x28 > l.data_end) return false;
    const uint64_t packed = read64(b, object + 0x10);
    const uint16_t count = uint16_t((packed >> 16) & 0xffff);
    const uint64_t first = read64(b, object + 0x20) + 0x20;
    // An expanded file retains the original array as unreachable data, so
    // its pointer references remain alongside the new Player array.
    if (count < 6 || count > 64 || first + uint64_t(count) * kRow > l.data_end ||
        refs.size() < size_t(count) + 1) return false;
    out.object = object;
    out.first = static_cast<size_t>(first);
    out.count = count;
    out.blank = count;
    out.selectable = 0;
    out.positions.clear();
    for (size_t i = 0; i < count; ++i) {
        const size_t row = out.first + i * kRow;
        if (read64(b, row + 8) != label - 0x20) return false;
        out.positions.emplace_back(static_cast<int8_t>(b[row + 0x36]),
                                   static_cast<int8_t>(b[row + 0x37]));
        const uint64_t pid = read64(b, row + 0x20);
        if (!pid) {
            if (out.blank == count) out.blank = i;
            ++out.selectable;
            continue;
        }
        const uint64_t p = pid + 0x20;
        constexpr char kNone[] = "PID_無し";
        if (p + sizeof(kNone) <= b.size() &&
            std::memcmp(b.data() + p, kNone, sizeof(kNone)) == 0) {
            if (out.blank == count) out.blank = i;
            ++out.selectable;
        }
    }
    return out.blank != count;
}

std::set<std::pair<int, int>> occupied_positions(const std::vector<uint8_t>& b,
                                                  const Layout& l, const std::string& selected_group,
                                                  uint16_t* highest_id) {
    std::set<std::pair<int, int>> used;
    *highest_id = 0;
    for (size_t at : l.locations) {
        if (at < 0x28 || at + 0x20 >= l.data_end) continue;
        const uint64_t ptr = read64(b, at) + 0x20;
        if (ptr < l.labels || ptr >= b.size()) continue;
        const char* label = reinterpret_cast<const char*>(b.data() + ptr);
        if (selected_group.rfind("Player_", 0) == 0 &&
            std::strncmp(label, "Player_", 7) == 0 &&
            selected_group != label) continue; // route-specific formations are alternatives
        const size_t object = at - 8;
        const uint64_t packed = read64(b, object + 0x10);
        const uint16_t n = uint16_t((packed >> 16) & 0xffff);
        const uint64_t first = read64(b, object + 0x20) + 0x20;
        if (n == 0 || n > 100 || first + uint64_t(n) * kRow > l.data_end ||
            read64(b, static_cast<size_t>(first) + 8) != read64(b, at)) continue;
        for (size_t i = 0; i < n; ++i) {
            const size_t row = static_cast<size_t>(first) + i * kRow;
            if (read64(b, row + 8) != read64(b, at)) break;
            *highest_id = std::max(*highest_id, uint16_t(b[row + 0x16] | (uint16_t(b[row + 0x17]) << 8)));
            used.emplace(static_cast<int8_t>(b[row + 0x36]), static_cast<int8_t>(b[row + 0x37]));
        }
    }
    return used;
}

std::vector<std::pair<int, int>> choose_positions(const Group& group,
                                                   std::set<std::pair<int, int>> used,
                                                   size_t needed, const std::string& file,
                                                   const std::string& name) {
    // The first four positions of this encounter layout were checked in battle.
    std::vector<std::pair<int, int>> result;
    if (file == "Tr058E_3.bin") {
        const std::array<std::pair<int, int>, 4> p1{{{11,21},{11,23},{10,24},{10,26}}};
        const std::array<std::pair<int, int>, 4> p2{{{10,20},{11,20},{10,27},{10,28}}};
        const auto& known = name == "Player1" ? p1 : p2;
        for (auto p : known) {
            if (result.size() == needed) break;
            if (used.insert(p).second) result.push_back(p);
        }
    }
    if (result.size() == needed) return result;
    int min_x = 127, max_x = -128, min_z = 127, max_z = -128;
    for (auto [x, z] : group.positions) {
        min_x = std::min(min_x, x); max_x = std::max(max_x, x);
        min_z = std::min(min_z, z); max_z = std::max(max_z, z);
    }
    struct Candidate { int score, x, z; };
    std::vector<Candidate> candidates;
    for (int x = std::max(-127, min_x - 6); x <= std::min(127, max_x + 6); ++x) {
        for (int z = std::max(-127, min_z - 6); z <= std::min(127, max_z + 6); ++z) {
            if (used.count({x, z})) continue;
            int nearest = 1000;
            for (auto [px, pz] : group.positions)
                nearest = std::min(nearest, std::abs(x - px) + std::abs(z - pz));
            const int outside = std::max(0, min_x - x) + std::max(0, x - max_x) +
                                std::max(0, min_z - z) + std::max(0, z - max_z);
            candidates.push_back({nearest * 100 + outside * 30 + std::abs(x) % 3 + std::abs(z) % 3, x, z});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score < b.score;
        if (a.z != b.z) return a.z < b.z;
        return a.x < b.x;
    });
    for (const Candidate& c : candidates) {
        if (result.size() == needed) break;
        if (used.insert({c.x, c.z}).second) result.emplace_back(c.x, c.z);
    }
    if (result.size() != needed) result.clear();
    return result;
}

bool patch_group(std::vector<uint8_t>& b, const std::string& file,
                 const std::string& name, int selectable_target) {
    Layout l;
    Group group;
    if (!inspect(b, l)) {
        if (file == "Tr058E_3.bin") NXLOG_W("coremods", "deployment %s %s: layout rejected", file.c_str(), name.c_str());
        return false;
    }
    if (!player_group(b, l, name, group)) {
        if (file == "Tr058E_3.bin") NXLOG_W("coremods", "deployment %s %s: group rejected", file.c_str(), name.c_str());
        return false;
    }
    if (group.count > 25 || selectable_target > 25) return false;
    const size_t fixed = group.count - group.selectable;
    const size_t allowed = std::min<size_t>(selectable_target, 25 - fixed);
    if (allowed <= group.selectable) return false;
    const size_t added = allowed - group.selectable;
    const size_t target = group.count + added;
    if (target > 64) return false;
    uint16_t highest_id = 0;
    auto used = occupied_positions(b, l, name, &highest_id);
    if (highest_id + added > 65535) return false;
    const auto positions = choose_positions(group, std::move(used), added, file, name);
    if (positions.size() != added) return false;
    const size_t array_bytes = size_t(target) * kRow;
    std::vector<uint8_t> extra(array_bytes);
    std::memcpy(extra.data(), b.data() + group.first, size_t(group.count) * kRow);
    const size_t template_at = group.first + group.blank * kRow;
    for (size_t i = 0; i < added; ++i) {
        const size_t row = (size_t(group.count) + i) * kRow;
        std::memcpy(extra.data() + row, b.data() + template_at, kRow);
        extra[row + 0x10] = uint8_t(group.count + i);
        extra[row + 0x11] = 0;
        const uint16_t id = highest_id + uint16_t(i) + 1;
        extra[row + 0x16] = uint8_t(id);
        extra[row + 0x17] = uint8_t(id >> 8);
        extra[row + 0x36] = uint8_t(positions[i].first);
        extra[row + 0x37] = uint8_t(positions[i].second);
    }
    std::set<size_t> new_ptrs;
    for (size_t at : l.locations) {
        new_ptrs.insert(at - 0x20);
        if (at >= group.first && at < group.first + size_t(group.count) * kRow)
            new_ptrs.insert(l.data_end + (at - group.first) - 0x20);
        if (at >= template_at && at < template_at + kRow)
            for (size_t i = 0; i < added; ++i)
                new_ptrs.insert(l.data_end + (size_t(group.count) + i) * kRow + (at - template_at) - 0x20);
    }
    const size_t shift = extra.size() + 8 * (new_ptrs.size() - l.count);
    std::vector<uint8_t> out;
    out.reserve(b.size() + shift);
    out.insert(out.end(), b.begin(), b.begin() + l.data_end);
    out.insert(out.end(), extra.begin(), extra.end());
    for (size_t relative : new_ptrs) {
        const size_t at = relative + 0x20;
        const uint64_t value = read64(out, at);
        if (value && value + 0x20 >= l.labels) write64(out, at, value + shift);
    }
    const uint64_t packed = read64(out, group.object + 0x10);
    write64(out, group.object + 0x10, (packed & ~(0xffffull << 16)) | (uint64_t(target) << 16));
    write64(out, group.object + 0x20, l.data_end - 0x20);
    for (size_t relative : new_ptrs)
        for (unsigned i = 0; i < 8; ++i) out.push_back(uint8_t(uint64_t(relative) >> (8 * i)));
    out.insert(out.end(), b.begin() + l.ptr_end, b.end());
    write32(out, 0, static_cast<uint32_t>(out.size()));
    write32(out, 4, static_cast<uint32_t>(l.data_end - 0x20 + extra.size()));
    write32(out, 8, static_cast<uint32_t>(new_ptrs.size()));
    Layout check;
    if (!inspect(out, check)) {
        if (file == "Tr058E_3.bin") NXLOG_W("coremods", "deployment %s %s: output layout rejected", file.c_str(), name.c_str());
        return false;
    }
    Group after;
    if (!player_group(out, check, name, after) || after.count != target ||
        after.selectable != allowed) {
        if (file == "Tr058E_3.bin") NXLOG_W("coremods", "deployment %s %s: output group rejected", file.c_str(), name.c_str());
        return false;
    }
    b = std::move(out);
    return true;
}

} // namespace

std::string fefw_deployment_overlay(const fs::RomfsImage& image,
                                     const std::string& romfs_dir,
                                     const std::vector<std::string>& existing_overlays,
                                     int target) {
    NXLOG_I("coremods", "deployment configuration target %d", target);
    if (target <= 6 || target > 25) return {};
    std::set<std::string> names;
    const auto collect = [&](const std::string& root) {
        std::error_code ec;
        const auto folder = pathfs::u8path(root) / "GameData" / "Dispos";
        for (pathfs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string name = it->path().filename().string();
            if (name.size() > 4 && name.substr(name.size() - 4) == ".bin" &&
                (name.rfind("SC", 0) == 0 || name.rfind("Tr", 0) == 0)) names.insert(name);
        }
    };
    collect(romfs_dir);
    for (const auto& root : existing_overlays) collect(root);
    NXLOG_I("coremods", "deployment candidate files: %zu", names.size());
    pathfs::path generated;
    size_t modified = 0, groups = 0;
    for (const auto& name : names) {
        const std::string path = "/GameData/Dispos/" + name;
        uint64_t at = 0, size = 0;
        if (!image.file_extent(path, &at, &size) || size < 0x40 || size > 4 * 1024 * 1024) continue;
        std::vector<uint8_t> data(static_cast<size_t>(size));
        if (!image.read(at, data.data(), size)) continue;
        if (name == "Tr058E_3.bin") NXLOG_I("coremods", "deployment probe file %s read (%llu bytes)", name.c_str(), static_cast<unsigned long long>(size));
        bool changed = false;
        Layout layout;
        if (!inspect(data, layout)) continue;
        std::set<std::string> player_names;
        for (size_t at = layout.labels; at < data.size();) {
            auto end = std::find(data.begin() + at, data.end(), uint8_t(0));
            if (end == data.end()) break;
            const size_t next = static_cast<size_t>(end - data.begin()) + 1;
            if (next - at > 1 && next - at <= 65 &&
                next - at >= 7 && std::memcmp(data.data() + at, "Player", 6) == 0)
                player_names.emplace(reinterpret_cast<const char*>(data.data() + at), next - at - 1);
            at = next;
        }
        for (const auto& player_name : player_names) {
            if (patch_group(data, name, player_name, target)) {
                changed = true;
                ++groups;
            }
        }
        if (!changed) continue;
        if (generated.empty()) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            generated = pathfs::temp_directory_path() / ("fefw-deployment-" + std::to_string(stamp));
        }
        std::error_code ec;
        const auto folder = generated / "GameData" / "Dispos";
        pathfs::create_directories(folder, ec);
        if (ec) continue;
        std::ofstream out(folder / name, std::ios::binary);
        if (!out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) continue;
        ++modified;
    }
    if (!modified) return {};
    NXLOG_I("coremods", "deployment target %d: expanded %zu Dispos files, %zu Player groups", target, modified, groups);
    return generated.string();
}

} // namespace nxrt::services
