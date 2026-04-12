# obs-game-script-translate

An OBS plugin that captures your game screen and translates story dialogue using a vision LLM. The translation is rendered as a text overlay directly on the OBS source.

## Requirements

- macOS (arm64), deployment target 12.0
- OBS Studio 29+
- An API key from Anthropic (Claude) or Zhipu AI (GLM)

## Features

- **Two LLM backends** — Claude Vision (claude-3-5-haiku) or GLM-4V
- **Three output languages** — Simplified Chinese, Japanese, English
- **Two capture modes:**
  - *Current scene* — hooks into OBS raw video callback, zero extra overhead
  - *Specific source* — off-screen renders a named OBS source via `gs_texrender`
- **Manual translate** — press **F9** in OBS
- **Clear translation** — press **F10**
- **Auto-clear** — translation disappears after a configurable number of seconds
- **Text overlay** — uses OBS FreeType text source; configurable font, color, background opacity, and width

Only story dialogue is translated. UI labels, HUD elements, skill names, and proper nouns are intentionally ignored.

## Build

```bash
# First run: downloads OBS headers, nlohmann/json, stb via CMake FetchContent
cmake -S obs-plugin -B obs-plugin/build

# Build
cmake --build obs-plugin/build

# Or use the Makefile shortcuts
make build
make install   # copies plugin to ~/Library/Application Support/obs-studio/plugins/
```

## Setup in OBS

1. Restart OBS after installing the plugin.
2. In a scene, click **+** → **Game Translator**.
3. Open **Properties** and fill in:

| Field | Description |
|-------|-------------|
| Target Source | Leave blank to capture the whole scene, or pick a specific source |
| LLM Provider | `Claude` or `GLM` |
| Target Language | `Chinese` / `Japanese` / `English` |
| API Key | Your Anthropic or Zhipu AI key |
| Overlay Font | Font for the translated text |
| Overlay Color | Text color |
| Background Opacity | Semi-transparent black background (0–100%) |
| Overlay Width | Fixed pixel width of the text block |
| Auto Clear (seconds) | How long before the translation fades out |

4. Press **F9** in OBS to translate.

## How it works

```
Game frame
  → BGRA pixels captured from OBS callback or gs_texrender
  → resized to 480px wide (stb_image_resize)
  → JPEG encoded at quality 50 (stb_image_write)
  → base64 encoded
  → sent to LLM vision API (libcurl POST)
  → response parsed (nlohmann/json)
  → displayed via private OBS FreeType text source
```

The API call runs on a background thread so it never blocks OBS rendering.

## License

MIT
