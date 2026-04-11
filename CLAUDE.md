# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

OBS 插件，通过 Claude Vision API 实时识别 Switch 游戏画面中的英文并翻译成中文。OBS Source 类型，在属性面板中配置，支持手动触发或定时自动翻译。

---

## OBS 插件（C++）

**平台**：macOS only（arm64，deployment target 12.0）  
**依赖**：libcurl（系统）、nlohmann/json（CMake FetchContent）、stb（CMake FetchContent，image write + resize）、OBS 头文件（首次构建时 git sparse clone OBS 29.1.3）

```bash
# 配置（首次会下载 OBS headers、nlohmann/json、stb）
cmake -S obs-plugin -B obs-plugin/build

# 构建
cmake --build obs-plugin/build

# 安装到 OBS 插件目录
cmake --build obs-plugin/build --target install-plugin
```

安装路径：`~/Library/Application Support/obs-studio/plugins/obs-game-translator.plugin/`

---

## 架构要点

### 源文件（`obs-plugin/src/`）

- `plugin-main.cpp` — OBS 模块入口，仅调用 `register_translate_source()`
- `translate-source.cpp` — 核心逻辑：注册 `game_translator_source` OBS Source、帧捕获、定时器、按钮回调
- `claude-api.cpp` — Claude API 调用：base64 编码 → libcurl POST → nlohmann/json 解析响应
- `image-encode.cpp` — BGRA→RGB 转换 → stb_image_resize（缩至 max_width=960）→ stb_image_write JPEG（quality=50）

### 双捕获模式（最重要的架构概念）

`TranslateData::target_source_name` 决定当前工作在哪种模式：

| 模式 | 触发条件 | 捕获方式 |
|------|----------|----------|
| **当前场景**（空字符串） | `obs_add_raw_video_callback` 每帧回调 | 在 `on_raw_video()` 中直接拿 `video_data*` 像素，用时间戳节流 |
| **具体 Source**（源名字） | `translate_video_tick()` 累计秒数 | `gs_texrender` 离屏渲染 + `gs_stagesurf` 回读像素 |

手动触发时：当前场景模式设 `manual_capture_requested` 原子标志，由下一帧 raw video callback 读取；具体 source 模式在按钮回调中直接调用 `capture_source_frame()`。

### 线程安全约束

- Claude API 调用在 `data->worker`（`std::thread`）中执行，不能阻塞 OBS 主线程
- `api_key`、`translation` 通过 `result_mutex` 保护
- `translating`（`std::atomic<bool>`）防止并发重复触发
- `gs_texrender` / `gs_stagesurf` 的创建/销毁必须在 `obs_enter_graphics()` / `obs_leave_graphics()` 中

### System Prompt 位置

仅存在于 `obs-plugin/src/claude-api.cpp` 顶部的 `SYSTEM_PROMPT` 常量。

---

## API Key 配置

属性面板的"Anthropic API Key"字段输入，留空则回退到 `ANTHROPIC_API_KEY` 环境变量。
