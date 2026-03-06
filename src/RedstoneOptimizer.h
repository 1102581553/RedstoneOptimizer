#pragma once
#include <ll/api/Config.h>
#include <ll/api/mod/NativeMod.h>
#include <memory>

namespace parallel_redstone {

struct Config {
    int  version = 1;
    bool enabled = true;
    bool debug   = false;
    int  maxIterations = 10;        // 最大迭代次数，防止无限循环
    int  maxThreads = 0;             // 0 表示自动检测
};

Config& getConfig();
bool    loadConfig();
bool    saveConfig();

class RedstoneOptimizer {
public:
    static RedstoneOptimizer& getInstance();

    RedstoneOptimizer() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace parallel_redstone
