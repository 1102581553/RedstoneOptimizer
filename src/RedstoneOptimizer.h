#pragma once

#include <ll/api/Config.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <memory>

namespace redstone_optimizer {

struct CacheEntry {
    uint64_t inputHash;
    int lastOutputStrength;
    uint64_t lastUpdateTick;
};

struct Config {
    int version = 1;
    bool enabled = true;
    bool debug = false;
    size_t maxCacheSize = 1000000;  // 默认最大缓存条目数（约 128 MB）
};

Config& getConfig();
bool loadConfig();
bool saveConfig();

void clearCache();                   // 清空缓存

ll::io::Logger& logger();

class PluginImpl {
public:
    static PluginImpl& getInstance();

    PluginImpl() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace redstone_optimizer
