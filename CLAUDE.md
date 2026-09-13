# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

OBS 插件，通过 LLM Vision API（支持 Claude 和 GLM）实时识别 Switch 游戏画面中的剧情文本并翻译。OBS Source 类型，在属性面板中配置，手动触发（快捷键）。

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
- `translate-source.cpp` — 核心逻辑：注册 `game_translator_source` OBS Source、帧捕获、定时器、快捷键回调
- `llm-provider.cpp` / `llm-provider.h` — `LlmProvider` 抽象基类 + `create_provider()` 工厂
- `claude-provider.cpp` — Claude Messages API 实现
- `glm-provider.cpp` — GLM（智谱 AI）API 实现
- `llm-utils.cpp` / `llm-utils.h` — 语言配置、`build_system_prompt()`、base64 编码、libcurl HTTP POST
- `image-encode.cpp` — BGRA→RGB 转换 → stb_image_resize（缩至 max_width=480）→ stb_image_write JPEG（quality=50）
- `tts-local.cpp` / `tts-local.h` — F8 朗读：POST 截图到本地 TTS 服务，拿回 wav
- `audio-output.cpp` — miniaudio 解码 wav → PCM 推进 OBS 音轨

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

### F8 朗读（本地服务，零 API 成本）

F8 和 F9 是两条独立的链路：**F9 看懂剧情（LLM 翻译后显示译文）**，**F8 听发音（朗读画面上的英文原文）**。

F8 不走 LLM。截图 POST 给 `tts-server/`（本机跑的 Python 服务）→ macOS Vision framework 做 OCR → 置信度+词数两条规则筛出剧情对白 → `say -v Zoe` 合成 → 返回 wav → 推进 OBS 音轨。

- 服务地址在属性面板「TTS 服务地址」配置，默认 `http://127.0.0.1:8765`
- Windows 端填这台 mac 的局域网地址即可共用同一个服务，插件侧无平台相关代码
- 画面没有对白时服务返回 **204**，插件静默不报错
- 部署、接口、阈值调整见 `tts-server/README.md`

### System Prompt 位置

在 `obs-plugin/src/llm-utils.cpp:31` 的 `build_system_prompt(target_language)` 中动态构建，不是常量。根据用户配置的目标语言（`get_lang_config()`，支持 zh/ja/en）拼出对应的 language name 和 "no text detected" 回复文案。Claude 和 GLM provider 共用同一份 system prompt。

---

## API Key 配置

属性面板有两个关键字段：

- **LLM Provider**：下拉选择 `claude` 或 `glm`（默认 `claude`）
- **API Key**：对应所选 provider 的密钥，必填，不设置则无法翻译（仅 F9 翻译需要，F8 朗读不需要任何 key）
