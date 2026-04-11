#include "claude-api.h"
#include <cstdlib>
#include <string>
#include <vector>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Declared by OBS_MODULE_USE_DEFAULT_LOCALE in plugin-main.cpp (C linkage)
extern "C" const char *obs_module_text(const char *val);

// ── Per-language config ───────────────────────────────────────────────────

struct LangConfig {
    const char *language_name;    // shown in the system prompt
    const char *no_text_response; // LLM outputs this when no story text found
    const char *user_message;     // user turn text sent alongside the screenshot
};

static const LangConfig &get_lang_config(const std::string &lang)
{
    static const LangConfig zh = {
        "Simplified Chinese (中文)",
        "截图中未检测到剧情文本。",
        "请翻译这张游戏截图中的剧情对话内容。"
    };
    static const LangConfig ja = {
        "Japanese (日本語)",
        "スクリーンショットにストーリーテキストは検出されませんでした。",
        "このゲームのスクリーンショットにあるストーリーの台詞を翻訳してください。"
    };
    static const LangConfig en = {
        "English",
        "No story dialogue text detected in screenshot.",
        "Please translate the story dialogue in this game screenshot."
    };
    if (lang == "ja") return ja;
    if (lang == "en") return en;
    return zh;
}

static std::string build_system_prompt(const std::string &target_language)
{
    const LangConfig &lc = get_lang_config(target_language);
    return std::string(
        "You are a game story translation assistant.\n\n"
        "The user will send a game screenshot. Identify and translate ONLY "
        "story-related text (in any language) into ") + lc.language_name + ", "
        "including: NPC dialogue, protagonist lines, narration, story subtitles, "
        "letters/journals and other narrative content.\n\n"
        "Ignore and do NOT translate:\n"
        "- UI control labels (buttons, menu items, tab names)\n"
        "- System prompts (save, load, settings, tutorial tips)\n"
        "- HUD elements (health, map, timer, inventory labels)\n"
        "- Proper nouns (character names, place names, skill names, item names)\n\n"
        "Output format requirements (strict):\n"
        "- Output only plain text, no Markdown formatting whatsoever\n"
        "- Do not use **bold**, *italic*, # headings, --- dividers, - lists, etc.\n"
        "- If no translatable story text is found, respond exactly: "
        + std::string(lc.no_text_response) + "\n\n"
        "Output only the translation, no other explanation.";
}

// ── Base64 ────────────────────────────────────────────────────────────────

static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const std::vector<uint8_t> &data)
{
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t n = ((uint32_t)data[i] << 16)
                   | ((uint32_t)data[i + 1] << 8)
                   |  (uint32_t)data[i + 2];
        out += B64_CHARS[(n >> 18) & 63];
        out += B64_CHARS[(n >> 12) & 63];
        out += B64_CHARS[(n >>  6) & 63];
        out += B64_CHARS[ n        & 63];
    }
    if (i + 1 == data.size()) {
        uint32_t n = (uint32_t)data[i] << 16;
        out += B64_CHARS[(n >> 18) & 63];
        out += B64_CHARS[(n >> 12) & 63];
        out += '=';
        out += '=';
    } else if (i + 2 == data.size()) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out += B64_CHARS[(n >> 18) & 63];
        out += B64_CHARS[(n >> 12) & 63];
        out += B64_CHARS[(n >>  6) & 63];
        out += '=';
    }
    return out;
}

// ── CURL response accumulator ─────────────────────────────────────────────

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *buf = static_cast<std::string *>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// ── CURL helper ───────────────────────────────────────────────────────────

static std::string do_post(const char *url,
                           struct curl_slist *headers,
                           const std::string &body_str)
{
    CURL *curl = curl_easy_init();
    if (!curl) return obs_module_text("Error.CurlInitFailed");

    std::string response_buf;
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_str.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       60L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return std::string(obs_module_text("Error.Network")) + curl_easy_strerror(res);

    return response_buf;
}

// ── Claude API ────────────────────────────────────────────────────────────

static std::string call_claude(const std::string &b64,
                                const char *media_type,
                                const std::string &api_key,
                                const std::string &target_language)
{
    const LangConfig &lc = get_lang_config(target_language);
    std::string system_prompt = build_system_prompt(target_language);

    json body = {
        {"model",      "claude-sonnet-4-6"},
        {"max_tokens", 2048},
        {"system",     system_prompt},
        {"messages", {{
            {"role", "user"},
            {"content", {
                {
                    {"type", "image"},
                    {"source", {
                        {"type",       "base64"},
                        {"media_type", media_type},
                        {"data",       b64}
                    }}
                },
                {{"type", "text"}, {"text", lc.user_message}}
            }}
        }}}
    };

    struct curl_slist *headers = nullptr;
    std::string auth_hdr = "x-api-key: " + api_key;
    headers = curl_slist_append(headers, auth_hdr.c_str());
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    headers = curl_slist_append(headers, "content-type: application/json");

    std::string raw = do_post("https://api.anthropic.com/v1/messages",
                              headers, body.dump());
    curl_slist_free_all(headers);

    try {
        json resp = json::parse(raw);
        if (resp.contains("error") && resp["error"].is_object())
            return std::string(obs_module_text("Error.API")) + resp["error"]["message"].get<std::string>();
        if (resp.contains("content") && resp["content"].is_array()
            && !resp["content"].empty()) {
            auto &first = resp["content"][0];
            if (first.value("type", "") == "text")
                return first["text"].get<std::string>();
        }
        return std::string(obs_module_text("Error.ParseResponse")) + raw.substr(0, 200);
    } catch (const json::exception &e) {
        return std::string(obs_module_text("Error.JSONParse")) + e.what();
    }
}

// ── GLM API (OpenAI-compatible) ───────────────────────────────────────────

static std::string call_glm(const std::string &b64,
                             const char *media_type,
                             const std::string &api_key,
                             const std::string &target_language)
{
    const LangConfig &lc = get_lang_config(target_language);
    std::string system_prompt = build_system_prompt(target_language);
    std::string data_url = std::string("data:") + media_type + ";base64," + b64;

    json body = {
        {"model",      "glm-4.6v"},
        {"max_tokens", 2048},
        {"thinking",   {{"type", "disabled"}}},
        {"messages", {
            {{"role", "system"}, {"content", system_prompt}},
            {
                {"role", "user"},
                {"content", {
                    {
                        {"type",      "image_url"},
                        {"image_url", {{"url", data_url}}}
                    },
                    {{"type", "text"}, {"text", lc.user_message}}
                }}
            }
        }}
    };

    struct curl_slist *headers = nullptr;
    std::string auth_hdr = "Authorization: Bearer " + api_key;
    headers = curl_slist_append(headers, auth_hdr.c_str());
    headers = curl_slist_append(headers, "content-type: application/json");

    std::string raw = do_post("https://api.z.ai/api/coding/paas/v4/chat/completions",
                              headers, body.dump());
    curl_slist_free_all(headers);

    try {
        json resp = json::parse(raw);
        if (resp.contains("error") && resp["error"].is_object())
            return std::string(obs_module_text("Error.API")) + resp["error"]["message"].get<std::string>();
        if (resp.contains("choices") && resp["choices"].is_array()
            && !resp["choices"].empty()) {
            auto &choice = resp["choices"][0];
            if (choice.contains("message") && choice["message"].contains("content"))
                return choice["message"]["content"].get<std::string>();
        }
        return std::string(obs_module_text("Error.ParseResponse")) + raw.substr(0, 200);
    } catch (const json::exception &e) {
        return std::string(obs_module_text("Error.JSONParse")) + e.what();
    }
}

// ── Public API ────────────────────────────────────────────────────────────

std::string analyze_image_data(const std::vector<uint8_t> &image_data,
                                const std::string &media_type,
                                const std::string &api_key,
                                const std::string &provider,
                                const std::string &target_language)
{
    if (api_key.empty())
        return obs_module_text("Error.NoAPIKey");
    if (image_data.empty())
        return obs_module_text("Error.EmptyImage");

    std::string b64 = base64_encode(image_data);

    if (provider == "glm")
        return call_glm(b64, media_type.c_str(), api_key, target_language);

    return call_claude(b64, media_type.c_str(), api_key, target_language);
}
