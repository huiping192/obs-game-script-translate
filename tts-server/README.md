# 本地 TTS 服务（F8 朗读）

给 OBS 插件的 F8 用：**游戏截图 → Vision OCR → macOS `say` → wav**。
全程本地，零 API 成本。Windows 端通过局域网调同一个服务，不需要另写平台代码。

## 依赖

- macOS（用到 Vision framework 和 `say`）
- 语音包：系统设置 → 辅助功能 → 朗读内容 → 系统声音 → 管理声音 → 下载 **Zoe (Premium)**
- Python 3.10+

```bash
python3 -m venv venv
./venv/bin/pip install pyobjc-framework-Vision
```

## 运行

手动：

```bash
./venv/bin/python server.py
```

开机自启（已配置）：

```bash
launchctl load   ~/Library/LaunchAgents/com.huiping.obs-game-translator-tts.plist
launchctl unload ~/Library/LaunchAgents/com.huiping.obs-game-translator-tts.plist
tail -f ~/Library/Logs/obs-game-translator-tts.log
```

## 接口

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/f8` | body 是 jpeg。返回 wav；**画面没有对白时返回 204**，插件据此静默 |
| POST | `/f8/debug` | 同上，但返回 JSON：OCR 全部结果 + 过滤后文本，用来调阈值 |
| POST | `/speak` | body 是纯文本，返回 wav。慢速重听用 `?rate=110` |
| GET | `/health` | 存活检查 |

通用 query 参数：`?voice=Zoe&rate=175`。

## 对白怎么挑出来的

Vision OCR 会把画面上所有文字都识别出来，包括 UI、人名标签、被英文模型误认的日文。
过滤规则只有两条（`MIN_CONF` / `MIN_WORDS`）：

| 内容 | conf | 词数 | 结果 |
|------|------|------|------|
| 剧情对白 | 1.00 | ≥4 | ✅ 保留 |
| `Correction`（特效字）、`th` | 1.00 | 1 | ❌ 词数不够 |
| `Ryunosuke`（人名标签） | 0.50 | 1 | ❌ 两条都不过 |
| 日文竖排被误认成的乱码 | 0.30–0.50 | — | ❌ 置信度不够 |

念到不该念的东西，或该念的被吞了，先用 `/f8/debug` 看原始识别结果，再调阈值：

```bash
curl -s -X POST --data-binary @shot.jpg http://127.0.0.1:8765/f8/debug | python3 -m json.tool
```

阈值走环境变量，在 plist 里加 `EnvironmentVariables` 即可：
`TTS_MIN_CONF`、`TTS_MIN_WORDS`、`TTS_VOICE`、`TTS_RATE`、`TTS_PORT`。

## Windows 端

插件属性面板「TTS 服务地址」填这台 mac 的局域网地址，例如 `http://192.168.11.75:8765`。
服务绑在 `0.0.0.0`，局域网内可直接访问（家用网络够用，公网环境别这么开）。
另外这台 mac 不能睡眠，否则 Windows 端会随机调不通：`sudo pmset -a sleep 0`。

## 缓存

合成结果按 `sha256(voice|rate|text)` 存在 `~/Library/Caches/obs-game-translator-tts/`。
同一句第二次触发直接命中（约 0.12s），反复听零成本。清缓存直接删该目录。
