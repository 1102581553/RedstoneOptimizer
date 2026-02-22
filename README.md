# RedstoneOptimizer

[![License](https://img.shields.io/badge/license-CC0-blue.svg)](LICENSE)
[![LeviLamina](https://img.shields.io/badge/LeviLamina-1.9.x-green)](https://github.com/LiteLDev/LeviLamina)

**RedstoneOptimizer** 是一个 LeviLamina 插件，旨在优化 Minecraft 基岩版服务端（BDS）的红石系统性能。通过重新排列红石元件的更新顺序并缓存输入不变的元件，该插件能够显著提升大型红石电路（如计算器、显示器、红石计算机）的 TPS，同时使红石行为更加可预测。

## ✨ 功能

- **有序更新**：将原版随机顺序的 C-tick 更新改为按位置排序（先 x 轴，再 z 轴，最后 y 轴），消除红石行为的随机性。
- **输入缓存**：对于输入信号未发生变化的非时序元件（如红石线、红石灯等），直接复用上次输出，避免重复计算，降低 CPU 开销。
- **时序元件兼容**：对于中继器、比较器等含有内部延迟的元件，插件会自动跳过缓存，保持原版行为，确保电路逻辑正确。

## 📥 安装

### 使用 lip（推荐）
1. 确保已安装 [lip](https://github.com/LiteLDev/lip) 包管理器。
2. 在服务器根目录运行以下命令：
   ```bash
   lip install github.com/yourusername/RedstoneOptimizer
   ```
   将 `yourusername` 替换为实际 GitHub 用户名。

### 手动安装
1. 从 [Releases](https://github.com/yourusername/RedstoneOptimizer/releases) 下载最新版本的 `RedstoneOptimizer-windows-x64.zip`。
2. 解压后将 `RedstoneOptimizer.dll` 和 `manifest.json` 放入服务器的 `plugins/RedstoneOptimizer/` 目录（如无该目录请自行创建）。
3. 重启服务器。

## 🚀 使用

插件加载后自动生效，无需任何配置命令。您只需照常建造红石电路，即可感受到性能提升和更稳定的行为。

> **注意**：由于更新顺序的改变，某些依赖随机性（如使用乱序红石作为随机数发生器）的设计可能会受到影响。请根据实际需求调整。

## ⚙️ 编译

如果您希望自行从源码构建，请确保满足以下环境：
- [xmake](https://xmake.io) 构建工具
- Visual Studio 2022（Windows）或 GCC 11+（Linux）
- LeviLamina 1.9.x 开发环境

1. 克隆仓库：
   ```bash
   git clone https://github.com/yourusername/RedstoneOptimizer.git
   cd RedstoneOptimizer
   ```
2. 修改 `xmake.lua` 中的 LeviLamina 依赖版本（若需要）。
3. 构建：
   ```bash
   xmake config -m release
   xmake
   ```
4. 生成的插件位于 `bin/RedstoneOptimizer.dll`。

## 📄 许可证

本项目采用 [CC0 1.0 通用](LICENSE) 许可证，您可自由使用、修改和分发。

## 🤝 贡献

欢迎提交 Issue 或 Pull Request 来帮助改进插件。

---

**RedstoneOptimizer** —— 让您的红石电路更加流畅、可预测！
