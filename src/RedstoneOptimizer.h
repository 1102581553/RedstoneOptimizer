#pragma once

#include <ll/api/Config.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <memory>
#include <unordered_map>

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
};

Config& getConfig();
bool loadConfig();
bool saveConfig();

std::unordered_map<void*, CacheEntry>& getCache();
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
