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
#include <mc/world/redstone/circuit/components/CapacitorComponent.h>

namespace redstone_optimizer {

static Config config;
static std::shared_ptr<ll::io::Logger> log;
static std::unordered_map<void*, CacheEntry> cache;
static bool hookInstalled = false;

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

bool hasInternalTimer(BaseCircuitComponent* comp) {
    return dynamic_cast<CapacitorComponent*>(comp) != nullptr;
}

uint64_t computeInputHash(ConsumerComponent* comp) {
    uint64_t hash = 0;
    for (const auto& item : comp->mSources->mComponents) {
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

void startDebugTask() {
    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (true) {
            co_await std::chrono::seconds(1);
            if (getConfig().debug) {
                logger().info("Cache size: {}", getCache().size());
            }
        }
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

// ============================ 钩子定义 ============================
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

    std::sort(chunkList.mComponents->begin(), chunkList.mComponents->end(),
        [](const ChunkCircuitComponentList::Item& a, const ChunkCircuitComponentList::Item& b) {
            if (a.mPos->x != b.mPos->x) return a.mPos->x < b.mPos->x;
            if (a.mPos->z != b.mPos->z) return a.mPos->z < b.mPos->z;
            return a.mPos->y < b.mPos->y;
        });
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
    if (!getConfig().enabled) return origin(system, pos);

    uint64_t currentHash = computeInputHash(this);
    auto it = getCache().find(this);
    if (it != getCache().end() && it->second.inputHash == currentHash) {
        if (hasInternalTimer(this)) return origin(system, pos);
        this->setStrength(it->second.lastOutputStrength);
        if (getConfig().debug) logger().debug("Cache hit at ({},{},{})", pos.x, pos.y, pos.z);
        return true;
    }

    bool result = origin(system, pos);
    getCache()[this] = CacheEntry{
        .inputHash = currentHash,
        .lastOutputStrength = this->getStrength(),
        .lastUpdateTick = getCurrentTickID()
    };
    if (getConfig().debug) logger().debug("Cache miss at ({},{},{})", pos.x, pos.y, pos.z);
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
            getCache().erase(it->second.get());
        }
    }
    origin(pos);
}

// ============================ 插件实现 ============================
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
        logger().debug("Hooks uninstalled and cache cleared");
    }
    logger().info("Plugin disabled");
    return true;
}

} // namespace redstone_optimizer

LL_REGISTER_MOD(redstone_optimizer::PluginImpl, redstone_optimizer::PluginImpl::getInstance());
