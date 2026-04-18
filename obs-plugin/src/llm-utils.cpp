#include "llm-utils.h"
#include <curl/curl.h>

// Declared by OBS_MODULE_USE_DEFAULT_LOCALE in plugin-main.cpp (C linkage)
extern "C" const char *obs_module_text(const char *val);

// ── Per-language config ───────────────────────────────────────────────────

const LangConfig &get_lang_config(const std::string &lang)
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

std::string build_system_prompt(const std::string &target_language)
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

std::string base64_encode(const std::vector<uint8_t> &data)
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

// ── Base64 decode ─────────────────────────────────────────────────────────

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<uint8_t> base64_decode(const std::string &encoded)
{
    std::vector<uint8_t> out;
    out.reserve(encoded.size() * 3 / 4);
    int buf = 0, bits = 0;
    for (char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = b64_val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// ── CURL helpers ──────────────────────────────────────────────────────────

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *buf = static_cast<std::string *>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t write_bytes_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *buf = static_cast<std::vector<uint8_t> *>(userdata);
    buf->insert(buf->end(), (const uint8_t *)ptr, (const uint8_t *)ptr + size * nmemb);
    return size * nmemb;
}

std::string do_get(const char *url, struct curl_slist *headers)
{
    CURL *curl = curl_easy_init();
    if (!curl) return {};

    std::string response_buf;
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return {};
    return response_buf;
}

std::vector<uint8_t> do_get_bytes(const char *url, struct curl_slist *headers)
{
    CURL *curl = curl_easy_init();
    if (!curl) return {};

    std::vector<uint8_t> buf;
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_bytes_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       60L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return {};
    return buf;
}

std::string do_post(const char *url,
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
