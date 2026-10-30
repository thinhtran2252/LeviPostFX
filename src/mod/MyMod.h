#pragma once

#include <mutex>
#include <optional>
#include <string_view>

#include "mod/Config.h"

#include <pl/Mod.hpp>

namespace clange_me {

class ClangeMeMod {
  public:
    static ClangeMeMod &instance();

    ClangeMeMod();

    [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

  private:
    void setMenuEnabled(bool enabled);
    void setMenuConfig(std::string_view key, std::string_view value);

    ll::mod::NativeMod &mSelf;
    ModConfig mConfig;
    std::optional<pl::config::ConfigFile<ModConfig>> mConfigFile;
    std::mutex mConfigMutex;
};

} // namespace clange_me
