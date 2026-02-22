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
#include <atomic>
#include <mutex>
#include <unordered_set>

namespace redstone_optimizer {

static Config config;
static std::shared_ptr<ll::io::Logger> log;
static std::unordered_map<void*, CacheEntry> cache;
static std::mutex cacheMutex;
static bool hookInstalled = false;
static std::atomic<bool> debugTaskRunning = false;

static std::atomic<size_t> cacheHitCount = 0;
static std::atomic<size_t> cacheMissCount = 0;
static std::atomic<size_t> cacheSkipCount = 0;

thread_local int evaluateDepth = 0;
constexpr int MAX_EVALUATE_DEPTH = 500;

// 用于抑制重复日志
static std::unordered_set<BlockPos> warnedPositions;
static std::mutex warnedMutex;

Config& getConfig() { return config; }

std::unordered_map<void*, CacheEntry>& getCache() { return cache; }

void clearCache() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache.clear();
}

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

// 不再使用 RTTI，仅根据数据计算哈希
uint64_t computeInputHash(ConsumerComponent* comp) {
    uint64_t hash = 0;
    // 即使 comp 无效，后续访问成员可能崩溃，但外层有 SEH 保护，这里不再额外处理
    auto* sources = comp->mSources.operator->();
    if (!sources) return 0;

    for (const auto& item : sources->mComponents) {
        BaseCircuitComponent* source = item.mComponent;
        if (!source) continue;
        // 移除 typeid，仅使用数据
        int strength = source->getStrength(); // 可能崩溃，但外层 SEH 会捕获
        hash = hash * 31 + strength;
        hash = hash * 31 + item.mDampening;
        hash = hash * 31 + (item.mDirectlyPowered ? 1 : 0);
        hash = hash * 31 + item.mDirection;
        hash = hash * 31 + item.mData;
    }
    return hash;
}

void startDebugTask() {
    if (debugTaskRunning.exchange(true)) return;
    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (debugTaskRunning) {
            co_await std::chrono::seconds(1);
            ll::thread::ServerThreadExecutor::getDefault().execute([]{
                if (!getConfig().debug) return;
                size_t total = cacheHitCount + cacheMissCount;
                double hitRate = total > 0 ? (100.0 * cacheHitCount / total) : 0.0;
                std::lock_guard<std::mutex> lock(cacheMutex);
                logger().info("Cache stats: hits={}, misses={}, skip={}, size={}, hitRate={:.1f}%",
                              cacheHitCount.load(), cacheMissCount.load(), cacheSkipCount.load(),
                              getCache().size(), hitRate);
            });
        }
        debugTaskRunning = false;
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

void stopDebugTask() {
    debugTaskRunning = false;
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
    auto& chunkList = this->mActiveComponentsPerChunk[chunkBlockPos];

    auto* pCompVec = chunkList.mComponents.operator->();
    if (pCompVec && !pCompVec->empty()) {
        std::sort(pCompVec->begin(), pCompVec->end(),
            [](const ChunkCircuitComponentList::Item& a, const ChunkCircuitComponentList::Item& b) {
                if (a.mPos->x != b.mPos->x) return a.mPos->x < b.mPos->x;
                if (a.mPos->z != b.mPos->z) return a.mPos->z < b.mPos->z;
                return a.mPos->y < b.mPos->y;
            });
    }
    chunkList.bShouldEvaluate = true;
}

// 使用 SEH 保护钩子主体
LL_TYPE_INSTANCE_HOOK(
    ConsumerComponentEvaluateHook,
    ll::memory::HookPriority::Normal,
    ConsumerComponent,
    &ConsumerComponent::$evaluate,
    bool,
    CircuitSystem& system,
    BlockPos const& pos
) {
    __try {
        ++evaluateDepth;
        if (evaluateDepth > MAX_EVALUATE_DEPTH) {
            logger().warn("Evaluate depth exceeded at ({},{},{}) depth={}", pos.x, pos.y, pos.z, evaluateDepth);
            bool result = origin(system, pos);
            --evaluateDepth;
            return result;
        }

        if (!getConfig().enabled) {
            ++cacheSkipCount;
            bool result = origin(system, pos);
            --evaluateDepth;
            return result;
        }

        // 不再检查时序元件，所有 ConsumerComponent 都缓存
        uint64_t currentHash = computeInputHash(this);

        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = getCache().find(this);

        if (it != getCache().end() && it->second.inputHash == currentHash) {
            int oldStrength = this->getStrength();
            int cachedStrength = it->second.lastOutputStrength;

            if (oldStrength != cachedStrength) {
                this->setStrength(cachedStrength);
                if (getConfig().debug) {
                    logger().debug("Cache hit & updated at ({},{},{})", pos.x, pos.y, pos.z);
                }
                ++cacheHitCount;
                --evaluateDepth;
                return true;
            } else {
                if (getConfig().debug) {
                    logger().debug("Cache hit (no change) at ({},{},{})", pos.x, pos.y, pos.z);
                }
                ++cacheHitCount;
                --evaluateDepth;
                return false;
            }
        }

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
        --evaluateDepth;
        return result;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // 任何结构化异常（如访问违例）都直接调用原版函数
        --evaluateDepth;  // 注意：如果异常发生在 depth 递增之前，这里可能负值，但概率极低
        return origin(system, pos);
    }
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
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = this->mAllComponents.find(pos);
        if (it != this->mAllComponents.end()) {
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
    stopDebugTask();
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
