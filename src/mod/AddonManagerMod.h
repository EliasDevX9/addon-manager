#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mod/Config.h"
#include "mod/Packs.h"

#include <pl/Mod.hpp>

namespace addon_manager {

class AddonManagerMod {
  public:
    static AddonManagerMod &instance();

    AddonManagerMod();

    [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

  private:
    void onConfigChanged(std::string_view key, std::string_view value);
    void startImport();
    void runImport(std::string path, std::string displayName);
    void togglePack(const std::string &key, bool active);
    void rescan();
    void publishMenu();
    std::string buildSchema();
    void setStatus(std::string text);

    ll::mod::NativeMod &mSelf;
    ModConfig mConfig;
    std::optional<pl::config::ConfigFile<ModConfig>> mConfigFile;

    std::mutex mMutex;
    std::filesystem::path mComMojang;
    std::vector<addon::WorldInfo> mWorlds;
    std::vector<addon::PackInfo> mPacks;
    std::string mSelectedWorld;
    std::string mStatus;

    std::atomic<bool> mImporting{false};
    std::atomic<bool> mRegistered{false};
    std::thread mWorker;
};

} // namespace addon_manager
