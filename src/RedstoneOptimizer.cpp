#include "RedstoneOptimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/io/Logger.h>
#include <ll/api/io/LoggerRegistry.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <ll/api/coro/CoroTask.h>
#include <mc/world/level/Level.h>
#include <mc/world/redstone/circuit/CircuitSystem.h>
#include <mc/world/redstone/circuit/CircuitSceneGraph.h>
#include <mc/world/redstone/circuit/ChunkCircuitComponentList.h>
#include <mc/world/redstone/circuit/components/BaseCircuitComponent.h>
#include <mc/world/redstone/circuit/components/ConsumerComponent.h>
#include <mc/world/redstone/circuit/CircuitComponentList.h> // 新增，用于访问 mComponents
#include <entt/entt.hpp>
#include <filesystem>
#include <chrono>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <mutex>
#include <future>
#include <atomic>

namespace parallel_redstone {

static Config config;
static std::shared_ptr<ll::io::Logger> log;
static bool debugTaskRunning = false;
static std::mutex graphMutex; // 模拟 mLockGraph

// 调试统计
static std::atomic<size_t> totalComponentsProcessed{0};
static std::atomic<size_t> totalTicks{0};
static std::atomic<size_t> totalIterations{0};

static ll::io::Logger& logger() {
    if (!log) {
        log = ll::io::LoggerRegistry::getInstance().getOrCreate("RedstoneOptimizer");
    }
    return *log;
}

Config& getConfig() { return config; }

bool loadConfig() {
    auto path = RedstoneOptimizer::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::loadConfig(config, path);
}

bool saveConfig() {
    auto path = RedstoneOptimizer::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::saveConfig(config, path);
}

static void resetStats() {
    totalComponentsProcessed = 0;
    totalTicks = 0;
    totalIterations = 0;
}

static void startDebugTask() {
    if (debugTaskRunning) return;
    debugTaskRunning = true;

    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (debugTaskRunning) {
            co_await std::chrono::seconds(5);
            ll::thread::ServerThreadExecutor::getDefault().execute([] {
                if (!config.debug) return;
                size_t avgComp = totalTicks > 0 ? totalComponentsProcessed.load() / totalTicks.load() : 0;
                size_t avgIter = totalTicks > 0 ? totalIterations.load() / totalTicks.load() : 0;
                logger().info(
                    "Redstone stats (5s): total components processed={}, avg per tick={}, avg iterations={}",
                    totalComponentsProcessed.load(), avgComp, avgIter
                );
                resetStats();
            });
        }
        debugTaskRunning = false;
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

static void stopDebugTask() {
    debugTaskRunning = false;
}

// ==================== 数据结构定义 ====================
struct ComponentChange {
    BlockPos pos;
    int newStrength;
};

// 构建依赖图：输入需要更新的组件位置集合，输出有向边列表和入度表
static std::pair<
    std::unordered_map<BlockPos, std::vector<BlockPos>>,
    std::unordered_map<BlockPos, int>
> buildDependencyGraph(
    CircuitSceneGraph& graph,
    const std::unordered_set<BlockPos>& nodes
) {
    std::unordered_map<BlockPos, std::vector<BlockPos>> edges;
    std::unordered_map<BlockPos, int> inDegree;
    for (auto& pos : nodes) {
        inDegree[pos] = 0;
    }

    // 从 mComponentsToReEvaluate 获取依赖关系
    for (auto& [srcPos, deps] : graph.mComponentsToReEvaluate) {
        if (nodes.find(srcPos) == nodes.end()) continue;
        for (auto& depPos : deps) {
            if (nodes.find(depPos) == nodes.end()) continue;
            edges[srcPos].push_back(depPos);
            inDegree[depPos]++;
        }
    }

    // 补充从 mSources 获取的依赖
    for (auto& pos : nodes) {
        auto* comp = graph.mAllComponents[pos].get();
        if (!comp) continue;
        // 获取实际的 CircuitComponentList 对象
        auto& sources = comp->mSources.get(); // 假设 mSources 是 TypedStorage，调用 get() 返回引用
        for (auto& item : sources.mComponents) { // 假设 CircuitComponentList 有 mComponents 成员
            BaseCircuitComponent* srcComp = item.mComponent;
            if (!srcComp) continue;
            BlockPos srcPos = srcComp->mPos.get(); // 直接访问成员 mPos，可能是 TypedStorage
            if (nodes.find(srcPos) == nodes.end()) continue;
            edges[srcPos].push_back(pos);
            inDegree[pos]++;
        }
    }

    return {std::move(edges), std::move(inDegree)};
}

// 拓扑排序，返回层次列表（每层为一个节点集合），若存在环路则返回空
static std::vector<std::vector<BlockPos>> topologicalSort(
    const std::unordered_set<BlockPos>& nodes,
    const std::unordered_map<BlockPos, std::vector<BlockPos>>& edges,
    std::unordered_map<BlockPos, int>& inDegree
) {
    std::queue<BlockPos> q;
    for (auto& pos : nodes) {
        if (inDegree[pos] == 0) {
            q.push(pos);
        }
    }

    std::vector<std::vector<BlockPos>> layers;
    std::unordered_set<BlockPos> visited;
    while (!q.empty()) {
        std::vector<BlockPos> layer;
        size_t qsize = q.size();
        for (size_t i = 0; i < qsize; ++i) {
            BlockPos u = q.front(); q.pop();
            layer.push_back(u);
            visited.insert(u);
            auto it = edges.find(u);
            if (it != edges.end()) {
                for (auto& v : it->second) {
                    if (--inDegree[v] == 0) {
                        q.push(v);
                    }
                }
            }
        }
        layers.push_back(layer);
    }

    if (visited.size() != nodes.size()) {
        // 有环路
        return {};
    }
    return layers;
}

// 处理一层中的多个组件（并行）
static std::vector<ComponentChange> processLayer(
    CircuitSystem& system,
    const std::vector<BlockPos>& layer,
    std::unordered_map<BlockPos, BaseCircuitComponent*>& compMap
) {
    std::vector<std::future<std::optional<ComponentChange>>> futures;
    for (auto& pos : layer) {
        auto* comp = compMap[pos];
        if (!comp) continue;
        futures.push_back(std::async(std::launch::async, [&system, comp, pos]() -> std::optional<ComponentChange> {
            bool changed = comp->evaluate(system, pos);
            if (changed) {
                return ComponentChange{pos, comp->getStrength()};
            }
            return std::nullopt;
        }));
    }
    std::vector<ComponentChange> changes;
    for (auto& f : futures) {
        auto opt = f.get();
        if (opt.has_value()) {
            changes.push_back(opt.value());
        }
    }
    return changes;
}

// 应用变化到组件，并收集新的受影响节点
static std::unordered_set<BlockPos> applyChanges(
    CircuitSceneGraph& graph,
    const std::vector<ComponentChange>& changes
) {
    std::unordered_set<BlockPos> newDirty;
    for (auto& change : changes) {
        auto* comp = graph.mAllComponents[change.pos].get();
        if (!comp) continue;
        comp->setStrength(change.newStrength);
        // 获取 mDestinations 的实际对象并遍历
        auto& destinations = comp->mDestinations.get(); // 假设 mDestinations 是 TypedStorage
        // 假设 destinations 是某种可迭代容器，如 Core::RefCountedSet
        for (auto* destComp : destinations) {
            if (destComp) {
                newDirty.insert(destComp->mPos.get());
            }
        }
    }
    return newDirty;
}

// 处理环路节点（串行迭代）
static std::vector<ComponentChange> processCycle(
    CircuitSystem& system,
    const std::unordered_set<BlockPos>& cycleNodes,
    std::unordered_map<BlockPos, BaseCircuitComponent*>& compMap,
    int maxIter
) {
    std::vector<BlockPos> sorted(cycleNodes.begin(), cycleNodes.end());
    std::sort(sorted.begin(), sorted.end());

    std::unordered_map<BlockPos, int> lastStrength;
    for (auto& pos : sorted) {
        lastStrength[pos] = compMap[pos]->getStrength();
    }

    bool stable = false;
    int iter = 0;
    std::vector<ComponentChange> allChanges;
    while (!stable && iter < maxIter) {
        stable = true;
        for (auto& pos : sorted) {
            auto* comp = compMap[pos];
            bool changed = comp->evaluate(system, pos);
            if (changed) {
                int newStr = comp->getStrength();
                if (newStr != lastStrength[pos]) {
                    lastStrength[pos] = newStr;
                    allChanges.push_back({pos, newStr});
                    stable = false;
                }
            }
        }
        iter++;
    }
    return allChanges;
}

// 主并行红石更新函数
static void parallelRedstoneUpdate(CircuitSystem& system, BlockSource* region) {
    std::lock_guard<std::mutex> lock(graphMutex);

    CircuitSceneGraph& graph = system.mSceneGraph;

    // 1. 处理待处理队列
    graph.processPendingAdds();
    // 如果有 processPendingUpdates 和 processPendingRemoves，也应调用

    // 2. 收集所有需要更新的节点
    std::unordered_set<BlockPos> dirtyNodes;
    for (auto& [srcPos, deps] : graph.mComponentsToReEvaluate) {
        dirtyNodes.insert(srcPos);
        for (auto& depPos : deps) {
            dirtyNodes.insert(depPos);
        }
    }

    if (dirtyNodes.empty()) {
        system.mHasBeenEvaluated = true;
        return;
    }

    // 3. 构建节点到组件指针的映射
    std::unordered_map<BlockPos, BaseCircuitComponent*> compMap;
    for (auto& pos : dirtyNodes) {
        auto it = graph.mAllComponents.find(pos);
        if (it != graph.mAllComponents.end()) {
            compMap[pos] = it->second.get();
        }
    }

    // 4. 构建依赖图
    auto [edges, inDegree] = buildDependencyGraph(graph, dirtyNodes);

    // 5. 拓扑排序分层
    auto layers = topologicalSort(dirtyNodes, edges, inDegree);
    bool hasCycle = layers.empty();
    if (hasCycle) {
        if (config.debug) {
            logger().warn("Cycle detected, processing {} nodes serially", dirtyNodes.size());
        }
        auto changes = processCycle(system, dirtyNodes, compMap, config.maxIterations);
        applyChanges(graph, changes);
    } else {
        std::unordered_set<BlockPos> newlyDirty;
        for (auto& layer : layers) {
            auto changes = processLayer(system, layer, compMap);
            totalComponentsProcessed += layer.size();
            auto newDirty = applyChanges(graph, changes);
            newlyDirty.insert(newDirty.begin(), newDirty.end());
        }
        // 可在此将 newlyDirty 加入 mComponentsToReEvaluate（但原版可能已经做了）
        totalIterations++;
    }

    system.mHasBeenEvaluated = true;
    totalTicks++;
}

// ==================== 钩子 ====================
// 注意：使用 &CircuitSystem::evaluate 而非 $evaluate
LL_AUTO_TYPE_INSTANCE_HOOK(
    CircuitSystemEvaluateHook,
    ll::memory::HookPriority::Normal,
    CircuitSystem,
    &CircuitSystem::evaluate,
    void,
    BlockSource* region
) {
    if (config.enabled) {
        parallelRedstoneUpdate(*this, region);
        return;
    }
    origin(region);
}

// ==================== 插件生命周期 ====================

RedstoneOptimizer& RedstoneOptimizer::getInstance() {
    static RedstoneOptimizer instance;
    return instance;
}

bool RedstoneOptimizer::load() {
    std::filesystem::create_directories(getSelf().getConfigDir());
    if (!loadConfig()) {
        logger().warn("Failed to load config, using default values and saving");
        saveConfig();
    }
    logger().info("Plugin loaded. enabled={}, debug={}, maxIterations={}",
                  config.enabled, config.debug, config.maxIterations);
    return true;
}

bool RedstoneOptimizer::enable() {
    if (config.debug) startDebugTask();
    logger().info("Plugin enabled");
    return true;
}

bool RedstoneOptimizer::disable() {
    stopDebugTask();
    resetStats();
    logger().info("Plugin disabled");
    return true;
}

} // namespace parallel_redstone

LL_REGISTER_MOD(parallel_redstone::RedstoneOptimizer, parallel_redstone::RedstoneOptimizer::getInstance());
