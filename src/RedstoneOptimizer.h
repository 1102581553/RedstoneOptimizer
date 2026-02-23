#pragma once

#include <ll/api/Config.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <memory>

namespace redstone_optimizer {

struct CacheEntry {
    uint64_t inputHash;
    int lastOutputStrength;
};

struct Config {
    int version = 1;
    bool enabled = true;
    bool debug = false;
    size_t maxCacheSize = 1000000;
};

Config& getConfig();
bool loadConfig();
bool saveConfig();

void clearCache();

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
