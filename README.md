# RedstoneOptimizer

[![License](https://img.shields.io/badge/license-CC0-blue.svg)](LICENSE)
[![LeviLamina](https://img.shields.io/badge/LeviLamina-1.9.x-green)](https://github.com/LiteLDev/LeviLamina)

**RedstoneOptimizer** 是一个 LeviLamina 插件，旨在优化 Minecraft 基岩版服务端（BDS）的红石系统性能。通过重新排列红石元件的更新顺序并缓存输入不变的元件，该插件能够显著提升大型红石电路（如计算器、显示器、红石计算机）的 TPS，同时使红石行为更加可预测。

## ✨ 功能

- **有序更新**：将原版随机顺序的 C-tick 更新改为按位置排序（先 x 轴，再 z 轴，最后 y 轴），消除红石行为的随机性。
- **输入缓存**：对于输入信号未发生变化的非时序元件（如红石线、红石灯等），直接复用上次输出，避免重复计算，降低 CPU 开销。
- **时序元件兼容**：对于中继器、比较器等含有内部延迟的元件，插件会自动跳过缓存，保持原版行为，确保电路逻辑正确。
- **可配置开关**：可通过配置文件开启/关闭优化功能，便于对比测试。
- **调试模式**：启用调试模式后，插件会输出缓存命中统计等信息，帮助分析优化效果。

## 📥 安装

### 使用 lip（推荐）
1. 确保已安装 [lip](https://github.com/LiteLDev/lip) 包管理器。
2. 在服务器根目录运行以下命令：
   ```bash
   lip install github.com/1102581553/RedstoneOptimizer
