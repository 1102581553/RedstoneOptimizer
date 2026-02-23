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
#include <excpt.h>
#include <list>

namespace redstone_optimizer {

static Config config;
static std::shared_ptr<ll::io::Logger> log;

// LRU 缓存结构
static std::list<void*> lruList;                                   // 链表头部最新，尾部最旧
static std::unordered_map<void*, std::pair<CacheEntry, std::list<void*>::iterator>> cacheMap; // 键 -> (条目, 链表迭代器)
static std::recursive_mutex cacheMutex;                            // 改为递归互斥锁，防止递归调用时死锁

static bool hookInstalled = false;
static std::atomic<bool> debugTaskRunning = false;

static std::atomic<size_t> cacheHitCount = 0;
static std::atomic<size_t> cacheMissCount = 0;
static std::atomic<size_t> cacheSkipCount = 0;

thread_local int evaluateDepth = 0;
constexpr int MAX_EVALUATE_DEPTH = 500;

Config& getConfig() { return config; }

void clearCache() {
    std::lock_guard<std::recursive_mutex> lock(cacheMutex);
    lruList.clear();
    cacheMap.clear();
}

bool loadConfig() {
    auto path = PluginImpl::getInstance().getSelf().getConfigDir() / "config.json";
    bool loaded = ll::config::loadConfig(config, path);
    // 确保 maxCacheSize 有效
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

uint64_t getCurrentTickID() {
    auto level = ll::service::getLevel();
    if (!level) return 0;
    return level->getCurrentTick().tickID;
}

// 仅使用数据计算哈希，避免 RTTI
uint64_t computeInputHash(ConsumerComponent* comp) {
    uint64_t hash = 0;
    auto* sources = comp->mSources.operator->();
    if (!sources) return 0;

    for (const auto& item : sources->mComponents) {
        BaseCircuitComponent* source = item.mComponent;
        if (!source) continue;
        int strength = source->getStrength();
        hash = hash * 31 + strength;
        hash = hash * 31 + item.mDampening;
        hash = hash * 31 + (item.mDirectlyPowered ? 1 : 0);
        hash = hash * 31 + item.mDirection;
        hash = hash * 31 + item.mData;
    }
    return hash;
}

// 尝试计算哈希，如果崩溃返回 false
bool tryComputeHash(ConsumerComponent* comp, uint64_t& outHash) {
    __try {
        outHash = computeInputHash(comp);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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
                std::lock_guard<std::recursive_mutex> lock(cacheMutex);
                logger().info("Cache stats: hits={}, misses={}, skip={}, size={}, hitRate={:.1f}%",
                              cacheHitCount.load(), cacheMissCount.load(), cacheSkipCount.load(),
                              cacheMap.size(), hitRate);
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

LL_TYPE_INSTANCE_HOOK(
    ConsumerComponentEvaluateHook,
    ll::memory::HookPriority::Normal,
    ConsumerComponent,
    &ConsumerComponent::$evaluate,
    bool,
    CircuitSystem& system,
    BlockPos const& pos
) {
    ++evaluateDepth;
    if (evaluateDepth > MAX_EVALUATE_DEPTH) {
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

    uint64_t currentHash = 0;
    if (!tryComputeHash(this, currentHash)) {
        // 计算哈希时崩溃，直接回退
        bool result = origin(system, pos);
        --evaluateDepth;
        return result;
    }

    std::lock_guard<std::recursive_mutex> lock(cacheMutex);

    auto it = cacheMap.find(this);

    if (it != cacheMap.end() && it->second.first.inputHash == currentHash) {
        // 命中缓存：将对应节点移到链表头部（最近使用）
        lruList.splice(lruList.begin(), lruList, it->second.second);

        int oldStrength = this->getStrength();
        int cachedStrength = it->second.first.lastOutputStrength;

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
    } else {
        // 未命中：调用原函数，然后插入新缓存
        bool result = origin(system, pos);

        // 插入前检查是否需要淘汰最久未使用的条目
        if (cacheMap.size() >= getConfig().maxCacheSize) {
            // 淘汰链表尾部（最久未使用）
            void* oldestKey = lruList.back();
            cacheMap.erase(oldestKey);
            lruList.pop_back();
        }

        // 在链表头部插入新键
        lruList.push_front(this);
        // 存储缓存条目及链表迭代器
        cacheMap[this] = {
            CacheEntry{
                .inputHash = currentHash,
                .lastOutputStrength = this->getStrength(),
                .lastUpdateTick = getCurrentTickID()
            },
            lruList.begin()
        };

        if (getConfig().debug) {
            logger().debug("Cache miss at ({},{},{})", pos.x, pos.y, pos.z);
        }
        ++cacheMissCount;
        --evaluateDepth;
        return result;
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
        std::lock_guard<std::recursive_mutex> lock(cacheMutex);
        auto it = this->mAllComponents.find(pos);
        if (it != this->mAllComponents.end()) {
            void* compPtr = it->second.get();
            auto cacheIt = cacheMap.find(compPtr);
            if (cacheIt != cacheMap.end()) {
                // 从链表中删除该键
                lruList.erase(cacheIt->second.second);
                // 从 map 中删除
                cacheMap.erase(cacheIt);
            }
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
