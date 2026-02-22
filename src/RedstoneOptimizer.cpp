#include "RedstoneOptimizer.h"
#include <ll/api/io/Logger.h>
#include <ll/api/memory/Hook.h>
#include <ll/api/service/Bedrock.h>
#include <mc/world/level/Level.h>
#include <mc/world/redstone/circuit/CircuitSceneGraph.h>
#include <mc/world/redstone/circuit/ChunkCircuitComponentList.h>
#include <mc/world/redstone/circuit/components/ConsumerComponent.h>
#include <mc/world/redstone/circuit/components/CapacitorComponent.h>
#include <algorithm>

namespace redstone_optimizer {

// ============================
// 全局缓存访问（通过插件实例）
// ============================
std::unordered_map<void*, CacheEntry>& RedstoneOptimizer::getCache() {
    return getInstance().mCache;
}

RedstoneOptimizer& RedstoneOptimizer::getInstance() {
    static RedstoneOptimizer instance;
    return instance;
}

bool RedstoneOptimizer::load() {
    getLogger().info("RedstoneOptimizer loading...");
    return true;
}

bool RedstoneOptimizer::enable() {
    getLogger().info("RedstoneOptimizer enabled.");
    return true;
}

bool RedstoneOptimizer::disable() {
    getLogger().info("RedstoneOptimizer disabled.");
    getCache().clear();
    return true;
}

bool RedstoneOptimizer::unload() {
    return true;
}

// ============================
// 辅助函数
// ============================
uint64_t getCurrentTickID() {
    auto level = ll::service::getLevel();
    if (!level) return 0;
    return level->getCurrentTick().tickID;
}

// 判断元件是否有内部定时器（时序元件）
bool hasInternalTimer(BaseCircuitComponent* comp) {
    auto type = comp->getCircuitComponentType();
    return type == CircuitComponentType::Repeater
        || type == CircuitComponentType::Comparator
        || type == CircuitComponentType::RedstoneTorch
        || type == CircuitComponentType::PulseCapacitor;
}

// 计算输入哈希（基于 ConsumerComponent 的 mSources）
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
// 有序列表优化：Hook CircuitSceneGraph::add
// ============================
LL_AUTO_TYPE_INSTANCE_HOOK(
    CircuitSceneGraphAddHook,
    ll::memory::HookPriority::Normal,
    CircuitSceneGraph,
    &CircuitSceneGraph::add,
    void,
    BlockPos const& pos,
    std::unique_ptr<BaseCircuitComponent> component
) {
    origin(pos, std::move(component));

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

// ============================
// 缓存优化：Hook ConsumerComponent::evaluate
// ============================
LL_AUTO_TYPE_INSTANCE_HOOK(
    ConsumerComponentEvaluateHook,
    ll::memory::HookPriority::Normal,
    ConsumerComponent,
    &ConsumerComponent::evaluate,
    bool,
    CircuitSystem& system,
    BlockPos const& pos
) {
    uint64_t currentHash = computeInputHash(this);
    auto& cache = redstone_optimizer::RedstoneOptimizer::getCache();

    auto it = cache.find(this);
    if (it != cache.end()) {
        auto& entry = it->second;
        if (entry.inputHash == currentHash) {
            if (hasInternalTimer(this)) {
                return origin(system, pos);
            }
            this->setStrength(entry.lastOutputStrength);
            return true;
        }
    }

    bool result = origin(system, pos);

    cache[this] = {
        .inputHash = currentHash,
        .lastOutputStrength = this->getStrength(),
        .lastUpdateTick = getCurrentTickID()
    };

    return result;
}

// ============================
// 缓存失效：元件被移除时清理缓存
// ============================
LL_AUTO_TYPE_INSTANCE_HOOK(
    CircuitSceneGraphRemoveComponentHook,
    ll::memory::HookPriority::Normal,
    CircuitSceneGraph,
    &CircuitSceneGraph::removeComponent,
    void,
    BlockPos const& pos
) {
    auto it = this->mAllComponents.find(pos);
    if (it != this->mAllComponents.end()) {
        BaseCircuitComponent* comp = it->second.get();
        redstone_optimizer::RedstoneOptimizer::getCache().erase(comp);
    }
    origin(pos);
}

} // namespace redstone_optimizer

// ============================
// 插件入口点
// ============================
#include <ll/api/plugin/RegisterHelper.h>

namespace redstone_optimizer {

static bool registerPlugin() {
    auto& mod = RedstoneOptimizer::getInstance();
    return ll::plugin::registerPlugin(mod);
}

static bool registered = registerPlugin();

} // namespace redstone_optimizer
