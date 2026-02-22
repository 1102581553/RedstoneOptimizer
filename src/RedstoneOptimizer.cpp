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
// 注意：RedstoneTorchCapacitor 通常是内部类，头文件可能不可用，我们依赖 CapacitorComponent 检查
#include <algorithm>
#include <filesystem>
#include <typeinfo>
#include <atomic>
#include <mutex>

namespace redstone_optimizer {

static Config config;
static std::shared_ptr<ll::io::Logger> log;
static std::unordered_map<void*, CacheEntry> cache;
static std::mutex cacheMutex;  // 缓存线程安全锁
static bool hookInstalled = false;
static std::atomic<bool> debugTaskRunning = false;  // 调试任务运行标志

// 统计计数器 (使用原子类型保证线程安全)
static std::atomic<size_t> cacheHitCount = 0;
static std::atomic<size_t> cacheMissCount = 0;
static std::atomic<size_t> cacheSkipCount = 0;

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

// 判断是否为时序元件（中继器、比较器、红石火把等）
// 根据文档，RedstoneTorchCapacitor 继承自 CapacitorComponent，所以会被此检查捕获
bool hasInternalTimer(BaseCircuitComponent* comp) {
    return dynamic_cast<CapacitorComponent*>(comp) != nullptr;
}

// 改进的输入哈希：加入源组件类型信息
uint64_t computeInputHash(ConsumerComponent* comp) {
    // mSources 是 TypedStorage<CircuitComponentList*>，需要 operator->() 获取指针
    auto* sources = comp->mSources.operator->(); 
    if (!sources) return 0;

    uint64_t hash = 0;
    // 修复：mComponents 是普通 std::vector，直接遍历 (不要调用 operator->())
    // 文档确认：CircuitComponentList 包含 mComponents
    for (const auto& item : sources->mComponents) { 
        BaseCircuitComponent* source = item.mComponent;
        if (!source) continue;
        size_t typeHash = typeid(*source).hash_code();
        int strength = source->getStrength();
        hash = hash * 31 + typeHash;
        hash = hash * 31 + strength;
        hash = hash * 31 + item.mDampening;
        hash = hash * 31 + (item.mDirectlyPowered ? 1 : 0);
        hash = hash * 31 + item.mDirection;
        hash = hash * 31 + item.mData;
    }
    return hash;
}

// 安全的调试任务：带停止标志
void startDebugTask() {
    if (debugTaskRunning.exchange(true)) return;  // 防止重复启动
    
    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (debugTaskRunning) {  // 可终止的循环
            // 修复：使用 LL 协程睡眠 API，而不是 std::chrono
            co_await ll::coro::sleep(std::chrono::seconds(1)); 
            ll::thread::ServerThreadExecutor::getDefault().execute([]{
                if (!getConfig().debug) return;
                size_t total = cacheHitCount + cacheMissCount;
                double hitRate = total > 0 ? (100.0 * cacheHitCount / total) : 0.0;
                // 访问缓存大小时加锁
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

    // 修复：mComponents 是 std::vector，直接访问 (不要调用 operator->())
    auto& compVec = chunkList.mComponents;
    if (!compVec.empty()) {
        std::sort(compVec.begin(), compVec.end(),
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
        ++cacheSkipCount;
        return origin(system, pos);
    }

    // 1. 跳过时序元件 (中继器、比较器、红石火把)
    // 根据文档，RedstoneTorchCapacitor 继承自 CapacitorComponent，会被此检查捕获
    // 这防止了缓存破坏红石火把的烧毁机制
    if (hasInternalTimer(this)) {
        ++cacheSkipCount;
        return origin(system, pos);
    }

    uint64_t currentHash = computeInputHash(this);
    
    // 2. 线程安全访问缓存
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = getCache().find(this);

    if (it != getCache().end() && it->second.inputHash == currentHash) {
        // 3. 修复返回值逻辑：仅当强度变化时才返回 true
        int oldStrength = this->getStrength();
        int cachedStrength = it->second.lastOutputStrength;
        
        if (oldStrength != cachedStrength) {
            this->setStrength(cachedStrength);
            if (getConfig().debug) {
                logger().debug("Cache hit & updated at ({},{},{})", pos.x, pos.y, pos.z);
            }
            ++cacheHitCount;
            return true;  // 状态改变，通知邻居
        } else {
            if (getConfig().debug) {
                logger().debug("Cache hit (no change) at ({},{},{})", pos.x, pos.y, pos.z);
            }
            ++cacheHitCount;
            return false;  // 状态未变，不通知邻居 (关键优化)
        }
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
    stopDebugTask();  // 修复：停止调试任务
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
