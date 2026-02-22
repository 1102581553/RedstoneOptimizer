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

namespace redstone_optimizer {

static Config config;
static std::shared_ptr<ll::io::Logger> log;
static std::unordered_map<void*, CacheEntry> cache;

// 缓存条目结构
struct CacheEntry {
    uint64_t inputHash;
    int lastOutputStrength;
    uint64_t lastUpdateTick;
};

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

// ============================
// 辅助函数
// ============================
uint64_t getCurrentTickID() {
    auto level = ll::service::getLevel();
    if (!level) return 0;
    return level->getCurrentTick().tickID;
}

bool hasInternalTimer(BaseCircuitComponent* comp) {
    auto type = comp->getCircuitComponentType();
    return type == CircuitComponentType::Repeater
        || type == CircuitComponentType::Comparator
        || type == CircuitComponentType::RedstoneTorch
        || type == CircuitComponentType::PulseCapacitor;
}

uint64_t computeInputHash(ConsumerComponent* comp) {
    uint64_t hash = 0;
    for (const auto& item : comp->mSources.mComponents) {
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

// ============================
// 调试任务：每秒输出缓存统计
// ============================
void startDebugTask() {
    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (true) {
            co_await std::chrono::seconds(1);
            if (getConfig().debug) {
                logger().info("Cache size: {}", getCache().size());
                // 可扩展更多统计，如命中率等
            }
        }
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

// ============================
// 钩子定义（非自动注册）
// ============================
LL_TYPE_INSTANCE_HOOK(
    CircuitSceneGraphAddHook,
    ll::memory::HookPriority::Normal,
    CircuitSceneGraph,
    &CircuitSceneGraph::add,
    void,
    BlockPos const& pos,
    std::unique_ptr<BaseCircuitComponent> component
);

LL_TYPE_INSTANCE_HOOK(
    ConsumerComponentEvaluateHook,
    ll::memory::HookPriority::Normal,
    ConsumerComponent,
    &ConsumerComponent::evaluate,
    bool,
    CircuitSystem& system,
    BlockPos const& pos
);

LL_TYPE_INSTANCE_HOOK(
    CircuitSceneGraphRemoveComponentHook,
    ll::memory::HookPriority::Normal,
    CircuitSceneGraph,
    &CircuitSceneGraph::removeComponent,
    void,
    BlockPos const& pos
);

// ============================
// 钩子实现
// ============================
bool g_hooksInstalled = false;

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

    std::sort(chunkList.mComponents.begin(), chunkList.mComponents.end(),
        [](const ChunkCircuitComponentList::Item& a, const ChunkCircuitComponentList::Item& b) {
            if (a.mPos.x != b.mPos.x) return a.mPos.x < b.mPos.x;
            if (a.mPos.z != b.mPos.z) return a.mPos.z < b.mPos.z;
            return a.mPos.y < b.mPos.y;
        });

    chunkList.bShouldEvaluate = true;
}

LL_TYPE_INSTANCE_HOOK(
    ConsumerComponentEvaluateHook,
    ll::memory::HookPriority::Normal,
    ConsumerComponent,
    &ConsumerComponent::evaluate,
    bool,
    CircuitSystem& system,
    BlockPos const& pos
) {
    if (!getConfig().enabled) {
        return origin(system, pos);
    }

    uint64_t currentHash = computeInputHash(this);
    auto& cache = getCache();

    auto it = cache.find(this);
    if (it != cache.end()) {
        auto& entry = it->second;
        if (entry.inputHash == currentHash) {
            if (hasInternalTimer(this)) {
                return origin(system, pos);
            }
            this->setStrength(entry.lastOutputStrength);
            if (getConfig().debug) {
                logger().debug("Cache hit for component at ({},{},{})", pos.x, pos.y, pos.z);
            }
            return true;
        }
    }

    bool result = origin(system, pos);

    cache[this] = {
        .inputHash = currentHash,
        .lastOutputStrength = this->getStrength(),
        .lastUpdateTick = getCurrentTickID()
    };

    if (getConfig().debug) {
        logger().debug("Cache miss for component at ({},{},{})", pos.x, pos.y, pos.z);
    }

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
        auto it = this->mAllComponents.find(pos);
        if (it != this->mAllComponents.end()) {
            BaseCircuitComponent* comp = it->second.get();
            getCache().erase(comp);
        }
    }
    origin(pos);
}

// ============================
// 插件实现
// ============================
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
    logger().info("Plugin loaded. Redstone optimization: {}, debug: {}",
                  config.enabled ? "enabled" : "disabled",
                  config.debug ? "enabled" : "disabled");
    return true;
}

bool PluginImpl::enable() {
    if (!mHooksInstalled) {
        CircuitSceneGraphAddHook::hook();
        ConsumerComponentEvaluateHook::hook();
        CircuitSceneGraphRemoveComponentHook::hook();
        mHooksInstalled = true;
        logger().debug("Hooks installed");
    }

    if (config.debug) {
        startDebugTask();
    }

    logger().info("Plugin enabled");
    return true;
}

bool PluginImpl::disable() {
    if (mHooksInstalled) {
        CircuitSceneGraphAddHook::unhook();
        ConsumerComponentEvaluateHook::unhook();
        CircuitSceneGraphRemoveComponentHook::unhook();
        mHooksInstalled = false;
        clearCache();
        logger().debug("Hooks uninstalled and cache cleared");
    }

    logger().info("Plugin disabled");
    return true;
}

} // namespace redstone_optimizer

LL_REGISTER_MOD(redstone_optimizer::PluginImpl, redstone_optimizer::PluginImpl::getInstance());
