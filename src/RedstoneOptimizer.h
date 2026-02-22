#pragma once

#include <ll/api/Config.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <memory>
#include <unordered_map>

namespace redstone_optimizer {

// 配置结构
struct Config {
    int version = 1;
    bool enabled = true;      // 红石优化总开关
    bool debug = false;       // 调试开关，输出缓存命中等信息
};

Config& getConfig();
bool loadConfig();
bool saveConfig();

// 缓存访问（供Hook使用）
std::unordered_map<void*, class CacheEntry>& getCache();
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
    bool mHooksInstalled = false;
};

} // namespace redstone_optimizer
