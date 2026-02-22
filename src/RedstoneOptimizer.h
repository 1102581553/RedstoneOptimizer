#pragma once

#include <ll/api/Mod.h>
#include <unordered_map>

namespace redstone_optimizer {

// 缓存条目结构
struct CacheEntry {
    uint64_t inputHash;          // 输入哈希值
    int lastOutputStrength;       // 上次计算的输出强度
    uint64_t lastUpdateTick;      // 上次更新时的游戏刻ID
};

// 插件主类
class RedstoneOptimizer : public ll::Mod {
public:
    // 获取插件实例
    static RedstoneOptimizer& getInstance();

    // 生命周期方法
    bool load() override;
    bool enable() override;
    bool disable() override;
    bool unload() override;

    // 缓存访问（供Hook使用，键为 BaseCircuitComponent*）
    static std::unordered_map<void*, CacheEntry>& getCache();

private:
    std::unordered_map<void*, CacheEntry> mCache;
};

} // namespace redstone_optimizer
