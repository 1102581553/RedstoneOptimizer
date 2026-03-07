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
#include <shared_mutex>
#include <atomic>

namespace redstone_optimizer {

static Config config;
static std::shared_ptr<ll::io::Logger> log;

// 扁平缓存：无淘汰，只增不减
static std::unordered_map<void*, CacheEntry> cacheMap;
static std::shared_mutex cacheMapMutex;

static bool hookInstalled = false;
static bool debugTaskRunning = false;

static std::atomic<size_t> cacheHitCount{0};
static std::atomic<size_t> cacheMissCount{0};
static std::atomic<size_t> cacheSkipCount{0};

thread_local int evaluateDepth = 0;
constexpr int MAX_EVALUATE_DEPTH = 512; // 可根据实际调整

Config& getConfig() { return config; }

void clearCache() {
    std::unique_lock lock(cacheMapMutex);
    cacheMap.clear();
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

static void startDebugTask() {
    if (debugTaskRunning) return;
    debugTaskRunning = true;

    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (debugTaskRunning) {
            co_await std::chrono::seconds(5);
            ll::thread::ServerThreadExecutor::getDefault().execute([] {
                if (!config.debug) return;
                size_t total = cacheHitCount.load() + cacheMissCount.load();
                double hitRate = total > 0 ? (100.0 * cacheHitCount.load() / total) : 0.0;
                logger().info(
                    "Cache stats: hits={}, misses={}, skip={}, size={}, hitRate={:.1f}%",
                    cacheHitCount.load(), cacheMissCount.load(), cacheSkipCount.load(),
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
        auto cmp = [](const ChunkCircuitComponentList::Item& a,
                      const ChunkCircuitComponentList::Item& b) {
            if (a.mPos->x != b.mPos->x) return a.mPos->x < b.mPos->x;
            if (a.mPos->z != b.mPos->z) return a.mPos->z < b.mPos->z;
            return a.mPos->y < b.mPos->y;
        };
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
    if (evaluateDepth > MAX_EVALUATE_DEPTH || !config.enabled) {
        if (config.enabled) ++cacheSkipCount;
        bool result = origin(system, pos);
        --evaluateDepth;
        return result;
    }

    uint64_t currentHash = computeInputHash(this);
    void* key = this;

    // 读缓存（共享锁）
    {
        std::shared_lock lock(cacheMapMutex);
        auto it = cacheMap.find(key);
        if (it != cacheMap.end() && it->second.inputHash == currentHash) {
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
    }

    bool result = origin(system, pos);

    // 写缓存（独占锁），无淘汰
    {
        std::unique_lock lock(cacheMapMutex);
        cacheMap[key] = CacheEntry{
            .inputHash = currentHash,
            .lastOutputStrength = this->getStrength()
        };
    }

    ++cacheMissCount;
    --evaluateDepth;
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
    if (redstone_optimizer::config.enabled) {
        auto compIt = this->mAllComponents.find(pos);
        if (compIt != this->mAllComponents.end()) {
            std::unique_lock lock(redstone_optimizer::cacheMapMutex);
            redstone_optimizer::cacheMap.erase(compIt->second.get());
        }
    }
    origin(pos);
}

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
    logger().info("Plugin loaded. enabled: {}, debug: {}", config.enabled, config.debug);
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
        cacheHitCount = 0;
        cacheMissCount = 0;
        cacheSkipCount = 0;
    }
    logger().info("Plugin disabled");
    return true;
}

} // namespace redstone_optimizer

LL_REGISTER_MOD(redstone_optimizer::PluginImpl, redstone_optimizer::PluginImpl::getInstance());
