#include "RedstoneOptimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/io/LoggerRegistry.h>
#include <ll/api/chrono/GameChrono.h>
#include <ll/api/coro/CoroTask.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <mc/world/level/Level.h>
#include <mc/world/redstone/circuit/CircuitSceneGraph.h>
#include <mc/world/redstone/circuit/ChunkCircuitComponentList.h>
#include <mc/world/redstone/circuit/components/ConsumerComponent.h>
#include <mc/world/redstone/circuit/components/CapacitorComponent.h>
#include <algorithm>
#include <filesystem>
#include <typeinfo>   // 用于 typeid

namespace redstone_optimizer {

static Config config;
static std::shared_ptr<ll::io::Logger> log;
static std::unordered_map<void*, CacheEntry> cache;
static bool hookInstalled = false;

// 统计计数器
static size_t cacheHitCount  = 0;
static size_t cacheMissCount = 0;
static size_t cacheSkipCount = 0;   // 因时序元件跳过缓存的次数

Config& getConfig() { return config; }

std::unordered_map<void*, CacheEntry>& getCache() { return cache; }
void clearCache() { cache.clear(); }

bool loadConfig() {
    auto path = PluginImpl::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::loadConfig(config, path);
}

bool saveConfig() {
    auto path = PluginImpl::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::saveConfig(config, path);
}

ll::io::Logger& logger() {
    if (!log) {
        log = ll::io::LoggerRegistry::getInstance().getOrCreate("RedstoneOptimizer");
    }
    return *log;
}

uint64_t getCurrentTickID() {
    auto level = ll::service::getLevel();
    if (!level) return 0;
    return level->getCurrentTick().tickID;
}

// 判断是否为时序元件（中继器、比较器等）
bool hasInternalTimer(BaseCircuitComponent* comp) {
    return dynamic_cast<CapacitorComponent*>(comp) != nullptr;
}

// 改进的输入哈希：加入源组件类型信息
// 修复：增加对 mSources 指针的空值检查，避免缓冲区溢出
// 修复：处理 TypedStorageImpl 包装器，不能直接使用 ! 运算符
uint64_t computeInputHash(ConsumerComponent* comp) {
    // 修复：显式调用 operator->() 获取原始指针进行判空
    // mSources 是 ll::TypedStorageImpl 包装器，不支持 operator!
    auto* sources = comp->mSources.operator->(); 
    if (!sources) return 0;   // 没有输入源时直接返回 0，避免访问空指针

    uint64_t hash = 0;
    // 使用获取到的 sources 指针访问成员
    for (const auto& item : sources->mComponents) { 
        BaseCircuitComponent* source = item.mComponent;
        if (!source) continue;
        size_t typeHash = typeid(*source).hash_code();   // 组件类型标识
        int strength = source->getStrength();
        // 组合哈希
        hash = hash * 31 + typeHash;
        hash = hash * 31 + strength;
        hash = hash * 31 + item.mDampening;
        hash = hash * 31 + (item.mDirectlyPowered ? 1 : 0);
        hash = hash * 31 + item.mDirection;
        hash = hash * 31 + item.mData;
    }
    return hash;
}

// 安全的调试任务：每 20 tick 在主线程输出统计
void startDebugTask() {
    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (true) {
            co_await std::chrono::seconds(1);   // 约 1 秒
            // 将打印任务调度到主线程，避免与钩子并发访问
            ll::thread::ServerThreadExecutor::getDefault().execute([]{
                if (!getConfig().debug) return;
                size_t total = cacheHitCount + cacheMissCount;
                double hitRate = total > 0 ? (100.0 * cacheHitCount / total) : 0.0;
                logger().info("Cache stats: hits={}, misses={}, skip={}, size={}, hitRate={:.1f}%",
                              cacheHitCount, cacheMissCount, cacheSkipCount,
                              getCache().size(), hitRate);
            });
        }
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}


LL_TYPE_INSTANCE_HOOK(
    CircuitSceneGraphAddHook,
    ll::memory::HookPriority::Normal,
    CircuitSceneGraph,
    &CircuitSceneGraph::add,
    void,
    BlockPos const& pos,
    std::unique_ptr<BaseCircuitComponent> component
) {
    origin(pos, std::move(component));
    if (!getConfig().enabled) return;

    ChunkPos chunkPos(pos);
    BlockPos chunkBlockPos(chunkPos.x, 0, chunkPos.z);
    
    // 修复：mActiveComponentsPerChunk 也是 TypedStorageImpl 包装器，需使用 .get() 获取底层容器
    auto& chunkMap = this->mActiveComponentsPerChunk.get();
    auto& chunkList = chunkMap[chunkBlockPos];

    // 修复：mComponents 也是包装器，显式获取指针以防万一
    auto* compVec = chunkList.mComponents.operator->();
    if (compVec) {
        std::sort(compVec->begin(), compVec->end(),
            [](const ChunkCircuitComponentList::Item& a, const ChunkCircuitComponentList::Item& b) {
                if (a.mPos->x != b.mPos->x) return a.mPos->x < b.mPos->x;
                if (a.mPos->z != b.mPos->z) return a.mPos->z < b.mPos->z;
                return a.mPos->y < b.mPos->y;
            });
    }
    chunkList.bShouldEvaluate = true;
}

LL_TYPE_INSTANCE_HOOK(
    ConsumerComponentEvaluateHook,
    ll::memory::HookPriority::Normal,
    ConsumerComponent,
    &ConsumerComponent::$evaluate,
    bool,
    CircuitSystem& system,
    BlockPos const& pos
) {
    if (!getConfig().enabled) {
        ++cacheSkipCount;   // 统计因配置禁用而跳过的元件
        return origin(system, pos);
    }

    uint64_t currentHash = computeInputHash(this);
    auto it = getCache().find(this);

    if (it != getCache().end() && it->second.inputHash == currentHash) {
        // 命中缓存，但需判断是否为时序元件
        if (hasInternalTimer(this)) {
            ++cacheSkipCount;
            return origin(system, pos);   // 时序元件不缓存
        }
        // 缓存命中且非时序，直接复用
        this->setStrength(it->second.lastOutputStrength);
        if (getConfig().debug) {
            logger().debug("Cache hit at ({},{},{})", pos.x, pos.y, pos.z);
        }
        ++cacheHitCount;
        return true;
    }

    // 缓存未命中，执行原版 evaluate
    bool result = origin(system, pos);
    getCache()[this] = CacheEntry{
        .inputHash = currentHash,
        .lastOutputStrength = this->getStrength(),
        .lastUpdateTick = getCurrentTickID()
    };
    if (getConfig().debug) {
        logger().debug("Cache miss at ({},{},{})", pos.x, pos.y, pos.z);
    }
    ++cacheMissCount;
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    CircuitSceneGraphRemoveComponentHook,
    ll::memory::HookPriority::Normal,
    CircuitSceneGraph,
    &CircuitSceneGraph::removeComponent,
    void,
    BlockPos const& pos
) {
    if (getConfig().enabled) {
        // 修复：mAllComponents 也是 TypedStorageImpl 包装器，需使用 .get() 获取底层容器
        auto& allCompMap = this->mAllComponents.get();
        auto it = allCompMap.find(pos);
        if (it != allCompMap.end()) {
            getCache().erase(it->second.get());
        }
    }
    origin(pos);
}

PluginImpl& PluginImpl::getInstance() {
    static PluginImpl instance;
    return instance;
}

bool PluginImpl::load() {
    std::filesystem::create_directories(getSelf().getConfigDir());
    if (!loadConfig()) {
        logger().warn("Failed to load config, using default values and saving");
        saveConfig();
    }
    logger().info("Plugin loaded. enabled: {}, debug: {}", config.enabled, config.debug);
    return true;
}

bool PluginImpl::enable() {
    if (!hookInstalled) {
        CircuitSceneGraphAddHook::hook();
        ConsumerComponentEvaluateHook::hook();
        CircuitSceneGraphRemoveComponentHook::hook();
        hookInstalled = true;
        logger().debug("Hooks installed");
    }
    if (config.debug) startDebugTask();
    logger().info("Plugin enabled");
    return true;
}

bool PluginImpl::disable() {
    if (hookInstalled) {
        CircuitSceneGraphAddHook::unhook();
        ConsumerComponentEvaluateHook::unhook();
        CircuitSceneGraphRemoveComponentHook::unhook();
        hookInstalled = false;
        clearCache();

        cacheHitCount = cacheMissCount = cacheSkipCount = 0;
        logger().debug("Hooks uninstalled, cache cleared, counters reset");
    }
    logger().info("Plugin disabled");
    return true;
}

} // namespace redstone_optimizer

LL_REGISTER_MOD(redstone_optimizer::PluginImpl, redstone_optimizer::PluginImpl::getInstance());
