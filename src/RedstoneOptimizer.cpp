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
#include <mc/world/redstone/circuit/components/CircuitComponentList.h>
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
static std::mutex graphMutex;

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

struct ComponentChange {
    BlockPos pos;
    int newStrength;
};

static std::pair<
    std::unordered_map<BlockPos, std::vector<BlockPos>>,
    std::unordered_map<BlockPos, int>
> buildDependencyGraph(
    CircuitSceneGraph& graph,
    const std::unordered_set<BlockPos>& nodes,
    bool debug
) {
    std::unordered_map<BlockPos, std::vector<BlockPos>> edges;
    std::unordered_map<BlockPos, int> inDegree;
    for (auto& pos : nodes) inDegree[pos] = 0;

    size_t edgeCount = 0;

    // 从 mComponentsToReEvaluate 获取依赖
    for (auto& [srcPos, deps] : graph.mComponentsToReEvaluate) {
        if (nodes.find(srcPos) == nodes.end()) continue;
        for (auto& depPos : deps) {
            if (nodes.find(depPos) == nodes.end()) continue;
            edges[srcPos].push_back(depPos);
            inDegree[depPos]++;
            edgeCount++;
            if (debug) logger().info("  Dep (mComponentsToReEvaluate): {} -> {}", srcPos.toNbt(), depPos.toNbt());
        }
    }

    // 从 mSources 获取反向依赖
    for (auto& pos : nodes) {
        auto* comp = graph.mAllComponents[pos].get();
        if (!comp) continue;
        auto& sources = comp->mSources.get();
        for (auto& item : sources.mComponents) {
            BaseCircuitComponent* srcComp = item.mComponent;
            if (!srcComp) continue;
            BlockPos srcPos = srcComp->mPos.get();
            if (nodes.find(srcPos) == nodes.end()) continue;
            edges[srcPos].push_back(pos);
            inDegree[pos]++;
            edgeCount++;
            if (debug) logger().info("  Dep (mSources): {} -> {}", srcPos.toNbt(), pos.toNbt());
        }
    }

    if (debug) logger().info("  Total edges added: {}", edgeCount);
    return {std::move(edges), std::move(inDegree)};
}

static std::vector<std::vector<BlockPos>> topologicalSort(
    const std::unordered_set<BlockPos>& nodes,
    const std::unordered_map<BlockPos, std::vector<BlockPos>>& edges,
    std::unordered_map<BlockPos, int>& inDegree,
    bool debug
) {
    std::queue<BlockPos> q;
    for (auto& pos : nodes) if (inDegree[pos] == 0) q.push(pos);

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
                    if (--inDegree[v] == 0) q.push(v);
                }
            }
        }
        layers.push_back(layer);
        if (debug) logger().info("  Layer {} size: {}", layers.size(), layer.size());
    }
    if (visited.size() != nodes.size()) {
        if (debug) logger().info("  Cycle detected: visited {}/{} nodes", visited.size(), nodes.size());
        return {};
    }
    if (debug) logger().info("  Topological sort succeeded, total layers: {}", layers.size());
    return layers;
}

static std::vector<ComponentChange> processLayer(
    CircuitSystem& system,
    const std::vector<BlockPos>& layer,
    std::unordered_map<BlockPos, BaseCircuitComponent*>& compMap,
    bool debug
) {
    if (debug) logger().info("  Processing layer with {} components", layer.size());
    std::vector<std::future<std::optional<ComponentChange>>> futures;
    for (auto& pos : layer) {
        auto* comp = compMap[pos];
        if (!comp) continue;
        futures.push_back(std::async(std::launch::async, [&system, comp, pos, debug]() -> std::optional<ComponentChange> {
            bool changed = comp->evaluate(system, pos);
            if (changed) {
                int newStr = comp->getStrength();
                if (debug) logger().info("    Component {} changed to strength {}", pos.toNbt(), newStr);
                return ComponentChange{pos, newStr};
            }
            return std::nullopt;
        }));
    }
    std::vector<ComponentChange> changes;
    for (auto& f : futures) {
        auto opt = f.get();
        if (opt.has_value()) changes.push_back(opt.value());
    }
    if (debug) logger().info("  Layer processed, {} changes", changes.size());
    return changes;
}

static void applyChanges(CircuitSceneGraph& graph, const std::vector<ComponentChange>& changes, bool debug) {
    for (auto& change : changes) {
        auto* comp = graph.mAllComponents[change.pos].get();
        if (comp) {
            comp->setStrength(change.newStrength);
            if (debug) logger().info("    Applied change at {}", change.pos.toNbt());
        }
    }
}

static std::vector<ComponentChange> processCycle(
    CircuitSystem& system,
    const std::unordered_set<BlockPos>& cycleNodes,
    std::unordered_map<BlockPos, BaseCircuitComponent*>& compMap,
    int maxIter,
    bool debug
) {
    std::vector<BlockPos> sorted(cycleNodes.begin(), cycleNodes.end());
    std::sort(sorted.begin(), sorted.end());

    if (debug) logger().info("  Processing cycle with {} nodes, max iterations {}", sorted.size(), maxIter);

    std::unordered_map<BlockPos, int> lastStrength;
    for (auto& pos : sorted) lastStrength[pos] = compMap[pos]->getStrength();

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
                    if (debug) logger().info("    Iter {}: {} changed to {}", iter, pos.toNbt(), newStr);
                }
            }
        }
        iter++;
    }
    if (debug) logger().info("  Cycle processed in {} iterations, {} changes total", iter, allChanges.size());
    return allChanges;
}

static void parallelRedstoneUpdate(CircuitSystem& system, BlockSource* region) {
    auto start = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(graphMutex);

    CircuitSceneGraph& graph = system.mSceneGraph;

    // 1. 处理待处理队列
    graph.processPendingAdds();
    // 如果有其他待处理队列，也应处理

    // 2. 收集脏节点
    std::unordered_set<BlockPos> dirtyNodes;
    for (auto& [srcPos, deps] : graph.mComponentsToReEvaluate) {
        dirtyNodes.insert(srcPos);
        for (auto& depPos : deps) dirtyNodes.insert(depPos);
    }

    auto endCollect = std::chrono::steady_clock::now();
    auto collectUs = std::chrono::duration_cast<std::chrono::microseconds>(endCollect - start).count();

    if (dirtyNodes.empty()) {
        logger().info("No dirty nodes, skipping parallel update");
        system.mHasBeenEvaluated = true;
        return;
    }

    logger().info("Dirty nodes count: {}", dirtyNodes.size());

    // 3. 构建映射
    std::unordered_map<BlockPos, BaseCircuitComponent*> compMap;
    for (auto& pos : dirtyNodes) {
        auto it = graph.mAllComponents.find(pos);
        if (it != graph.mAllComponents.end()) compMap[pos] = it->second.get();
        else logger().warn("Component at {} not found in mAllComponents", pos.toNbt());
    }

    auto endMap = std::chrono::steady_clock::now();
    auto mapUs = std::chrono::duration_cast<std::chrono::microseconds>(endMap - endCollect).count();

    // 4. 构建依赖图
    bool debug = config.debug;
    auto [edges, inDegree] = buildDependencyGraph(graph, dirtyNodes, debug);
    auto layers = topologicalSort(dirtyNodes, edges, inDegree, debug);
    bool hasCycle = layers.empty();

    auto endGraph = std::chrono::steady_clock::now();
    auto graphUs = std::chrono::duration_cast<std::chrono::microseconds>(endGraph - endMap).count();

    if (hasCycle) {
        logger().info("Cycle detected, processing {} nodes serially", dirtyNodes.size());
        auto changes = processCycle(system, dirtyNodes, compMap, config.maxIterations, debug);
        applyChanges(graph, changes, debug);
        totalComponentsProcessed += dirtyNodes.size();
    } else {
        logger().info("Layers: {}", layers.size());
        for (auto& layer : layers) {
            auto changes = processLayer(system, layer, compMap, debug);
            totalComponentsProcessed += layer.size();
            applyChanges(graph, changes, debug);
        }
        totalIterations++;
    }

    auto end = std::chrono::steady_clock::now();
    auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    logger().info("Parallel update completed: dirty={}, collect={}us, map={}us, graph={}us, total={}us",
                  dirtyNodes.size(), collectUs, mapUs, graphUs, totalUs);

    system.mHasBeenEvaluated = true;
    totalTicks++;
}

// ==================== 钩子 ====================
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
    }
    origin(region); // 始终调用原版确保红石工作
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
