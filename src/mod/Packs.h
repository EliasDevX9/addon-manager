#pragma once

#include <array>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace addon {

enum class PackKind { Behavior, Resource };

struct PackInfo {
    std::string uuid; // lowercase
    std::string name;
    std::array<int, 3> version{1, 0, 0};
    PackKind kind{PackKind::Behavior};
    std::filesystem::path dir;
    std::vector<std::string> dependencies; // uuids (lowercase)
};

struct WorldInfo {
    std::string folder;
    std::string name;
    std::filesystem::path dir;
    std::filesystem::file_time_type lastWrite{};
};

struct ImportResult {
    int imported = 0;
    std::vector<std::string> importedNames;
    std::vector<std::string> errors;
};

// Finds the games/com.mojang directory (config override, then ancestors of the mod dir,
// then well-known Android locations). Returns empty path when nothing was found.
std::filesystem::path findComMojang(const std::filesystem::path &modDir,
                                    const std::string &overridePath);

std::vector<WorldInfo> listWorlds(const std::filesystem::path &comMojang);
std::vector<PackInfo> listInstalledPacks(const std::filesystem::path &comMojang);

// Unzips a .mcaddon / .mcpack / .zip into behavior_packs / resource_packs.
ImportResult importArchive(const std::filesystem::path &comMojang,
                           const std::filesystem::path &archive);

std::set<std::string> activePackUuids(const std::filesystem::path &worldDir, PackKind kind);

// Adds/removes the pack (and, when activating, installed dependencies) in the world's
// world_behavior_packs.json / world_resource_packs.json. `changed` = how many entries changed.
bool setPackActive(const std::filesystem::path &worldDir, const PackInfo &pack, bool active,
                   const std::vector<PackInfo> &installed, int &changed, std::string &error);

std::string stripFormatting(const std::string &text);

} // namespace addon
