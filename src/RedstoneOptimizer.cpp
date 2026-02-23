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
#include <algorithm>
#include <filesystem>

namespace redstone_optimizer {

// ====================== 全局状态（主线程独占） ======================
static Config config;
static std::shared_ptr<ll::io::Logger> log;

// 扁平缓存：去掉 LRU 链表，用时间戳做淘汰
// key = ConsumerComponent*，value = CacheEntry
static std::unordered_map<void*, CacheEntry> cacheMap;
static uint64_t cacheGeneration = 0; // 单调递增，用于淘汰判断

static bool hookInstalled = false;
static bool debugTaskRunning = false;

static size_t cacheHitCount = 0;
static size_t cacheMissCount = 0;
static size_t cacheSkipCount = 0;

// 递归深度保护
thread_local int evaluateDepth = 0;
constexpr int MAX_EVALUATE_DEPTH = 500;

Config& getConfig() { return config; }

void clearCache() {
    cacheMap.clear();
    cacheGeneration = 0;
}

bool loadConfig() {
    auto path = PluginImpl::getInstance().getSelf().getConfigDir() / "config.json";
    bool loaded = ll::config::loadConfig(config, path);
    if (config.maxCacheSize == 0) {
        config.maxCacheSize = 1000000;
    }
    return loaded;
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

// ====================== 哈希计算 ======================
static uint64_t computeInputHash(ConsumerComponent* comp) {
    if (!comp) return 0;

    auto* sources = comp->mSources.operator->();
    if (!sources) return 0;

    uint64_t hash = 0;
    for (const auto& item : sources->mComponents) {
        BaseCircuitComponent* source = item.mComponent;
        if (!source) continue;

        int strength = source->getStrength();
        hash = hash * 131 + static_cast<uint64_t>(strength);
        hash = hash * 131 + static_cast<uint64_t>(item.mDampening);
        hash = hash * 131 + (item.mDirectlyPowered ? 1ULL : 0ULL);
        hash = hash * 131 + static_cast<uint64_t>(item.mDirection);
        hash = hash * 131 + static_cast<uint64_t>(item.mData);
    }
    return hash;
}

// ====================== 调试任务 ======================
static void startDebugTask() {
    if (debugTaskRunning) return;
    debugTaskRunning = true;

    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (debugTaskRunning) {
            co_await std::chrono::seconds(1);
            ll::thread::ServerThreadExecutor::getDefault().execute([] {
                if (!config.debug) return;
                size_t total = cacheHitCount + cacheMissCount;
                double hitRate = total > 0 ? (100.0 * cacheHitCount / total) : 0.0;
                logger().info(
                    "Cache stats: hits={}, misses={}, skip={}, size={}, hitRate={:.1f}%",
                    cacheHitCount, cacheMissCount, cacheSkipCount,
                    cacheMap.size(), hitRate
                );
            });
        }
        debugTaskRunning = false;
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

static void stopDebugTask() {
    debugTaskRunning = false;
}

} // namespace redstone_optimizer

// ====================== Hooks ======================

// 组件添加时有序插入，替代全量排序
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
    if (!redstone_optimizer::config.enabled) return;

    ChunkPos chunkPos(pos);
    BlockPos chunkBlockPos(chunkPos.x, 0, chunkPos.z);
    auto& chunkList = this->mActiveComponentsPerChunk[chunkBlockPos];

    auto* pCompVec = chunkList.mComponents.operator->();
    if (pCompVec && pCompVec->size() > 1) {
        // 只有最后一个元素是新插入的，对已排序数组做一次插入排序 O(n)
        auto cmp = [](const ChunkCircuitComponentList::Item& a,
                      const ChunkCircuitComponentList::Item& b) {
            if (a.mPos->x != b.mPos->x) return a.mPos->x < b.mPos->x;
            if (a.mPos->z != b.mPos->z) return a.mPos->z < b.mPos->z;
            return a.mPos->y < b.mPos->y;
        };

        // 从倒数第二个开始向前冒泡
        for (size_t i = pCompVec->size() - 1; i > 0; --i) {
            if (cmp((*pCompVec)[i], (*pCompVec)[i - 1])) {
                std::swap((*pCompVec)[i], (*pCompVec)[i - 1]);
            } else {
                break;
            }
        }
    }
    chunkList.bShouldEvaluate = true;
}

// 核心：ConsumerComponent::evaluate 缓存
LL_TYPE_INSTANCE_HOOK(
    ConsumerComponentEvaluateHook,
    ll::memory::HookPriority::Normal,
    ConsumerComponent,
    &ConsumerComponent::$evaluate,
    bool,
    CircuitSystem& system,
    BlockPos const& pos
) {
    using namespace redstone_optimizer;

    ++evaluateDepth;

    // 递归深度保护 / 插件未启用
    if (evaluateDepth > MAX_EVALUATE_DEPTH || !config.enabled) {
        if (config.enabled) ++cacheSkipCount;
        bool result = origin(system, pos);
        --evaluateDepth;
        return result;
    }

    uint64_t currentHash = computeInputHash(this);
    void* key = this;

    // 查缓存
    auto it = cacheMap.find(key);
    if (it != cacheMap.end() && it->second.inputHash == currentHash) {
        // 缓存命中
        int oldStrength = this->getStrength();
        int cachedStrength = it->second.lastOutputStrength;
        ++cacheHitCount;
        --evaluateDepth;

        if (oldStrength != cachedStrength) {
            this->setStrength(cachedStrength);
            return true;
        }
        return false;
    }

    // 缓存未命中：执行原函数
    // 注意：origin() 可能递归触发其他 evaluate，修改 cacheMap
    // 所以 origin() 之后不能使用之前的迭代器
    bool result = origin(system, pos);

    // origin() 返回后重新操作 cacheMap（迭代器已失效，不复用）
    // 简单淘汰：超过上限时清空一半（摊销 O(1)，避免 LRU 链表开销）
    if (cacheMap.size() >= config.maxCacheSize) {
        // 批量清理：直接清空，下一轮重新填充
        // 对于红石电路，活跃组件会很快重新缓存
        cacheMap.clear();
        ++cacheGeneration;
    }

    // 直接插入或覆盖
    cacheMap[key] = CacheEntry{
        .inputHash = currentHash,
        .lastOutputStrength = this->getStrength()
    };

    ++cacheMissCount;
    --evaluateDepth;
    return result;
}

// 组件移除时清理缓存
LL_TYPE_INSTANCE_HOOK(
    CircuitSceneGraphRemoveComponentHook,
    ll::memory::HookPriority::Normal,
    CircuitSceneGraph,
    &CircuitSceneGraph::removeComponent,
    void,
    BlockPos const& pos
) {
    if (redstone_optimizer::config.enabled) {
        auto compIt = this->mAllComponents.find(pos);
        if (compIt != this->mAllComponents.end()) {
            redstone_optimizer::cacheMap.erase(compIt->second.get());
        }
    }
    origin(pos);
}

// ====================== 插件生命周期 ======================
namespace redstone_optimizer {

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
    logger().info("Plugin loaded. enabled: {}, debug: {}, maxCacheSize: {}",
                  config.enabled, config.debug, config.maxCacheSize);
    return true;
}

bool PluginImpl::enable() {
    if (!hookInstalled) {
        CircuitSceneGraphAddHook::hook();
        ConsumerComponentEvaluateHook::hook();
        CircuitSceneGraphRemoveComponentHook::hook();
        hookInstalled = true;
    }
    if (config.debug) startDebugTask();
    logger().info("Plugin enabled");
    return true;
}

bool PluginImpl::disable() {
    stopDebugTask();
    if (hookInstalled) {
        CircuitSceneGraphAddHook::unhook();
        ConsumerComponentEvaluateHook::unhook();
        CircuitSceneGraphRemoveComponentHook::unhook();
        hookInstalled = false;
        clearCache();
        cacheHitCount = cacheMissCount = cacheSkipCount = 0;
    }
    logger().info("Plugin disabled");
    return true;
}

} // namespace redstone_optimizer

LL_REGISTER_MOD(redstone_optimizer::PluginImpl, redstone_optimizer::PluginImpl::getInstance());
