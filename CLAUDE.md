# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

OBS 插件 + Python 原型，通过 Claude Vision API 实时识别 Switch 游戏画面中的英文并翻译成中文。

两个独立子项目：
- `prototype/` — Python CLI 原型（阶段一，已可用）
- `obs-plugin/` — C++ OBS 插件（阶段三目标）

---

## Python 原型

**环境要求**：`ANTHROPIC_API_KEY` 环境变量

```bash
# 安装依赖（使用 uv）
cd prototype
uv sync

# 运行
uv run python analyze.py <图片路径>
uv run python analyze.py <图片路径> --model claude-opus-4-6
```

---

## OBS 插件（C++）

**平台**：macOS only（arm64，deployment target 12.0）  
**依赖**：libcurl（系统）、nlohmann/json（CMake FetchContent 自动下载）、OBS 头文件（首次构建时 git sparse clone）

```bash
# 配置（首次会下载 OBS headers 和 nlohmann/json）
cmake -S obs-plugin -B obs-plugin/build

# 构建
cmake --build obs-plugin/build

# 安装到 OBS 插件目录
cmake --build obs-plugin/build --target install-plugin
```

安装路径：`~/Library/Application Support/obs-studio/plugins/obs-game-translator.plugin/`

---

## 架构要点

### Python 原型 (`prototype/analyze.py`)
单文件，读取图片 → base64 编码 → 调用 `client.messages.create` → 打印翻译。逻辑即模板，后续阶段会加入变化检测。

### OBS 插件 (`obs-plugin/src/`)
- `plugin-main.cpp` — OBS 模块入口，仅调用 `register_translate_source()`
- `translate-source.h/.cpp` — 注册 `game_translator_source` OBS Source 类型（含 UI 属性、帧回调）
- `claude-api.h/.cpp` — 独立封装：读图片 → base64 → libcurl POST → nlohmann/json 解析响应

**关键设计约束**：Claude API 调用必须在独立线程，不能阻塞 OBS 主线程（OBS tick callback 在主线程）。

### 两个组件共享同一个 system prompt
Python 版在 `prototype/analyze.py:20-26`，C++ 版在 `obs-plugin/src/claude-api.cpp:14-19`，修改时需同步。

---

## API Key 配置

- Python：`ANTHROPIC_API_KEY` 环境变量
- OBS 插件：属性面板输入，或回退到 `ANTHROPIC_API_KEY` 环境变量
