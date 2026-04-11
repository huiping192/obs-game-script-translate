#include "claude-api.h"
#include <cstdlib>
#include <string>
#include <vector>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Same system prompt as the Python prototype (prototype/analyze.py:20-26)
static const char *SYSTEM_PROMPT =
    "你是一位游戏剧情翻译助手。\n\n"
    "用户会发送游戏截图。请只识别并翻译剧情相关的英文文本，"
    "包括：NPC 对话、主角台词、旁白、剧情字幕、信件/日记等叙事内容。\n\n"
    "忽略以下内容，不要翻译：\n"
    "- UI 控件标签（按钮、菜单项、选项卡名称）\n"
    "- 系统提示（存档、加载、设置、教程提示）\n"
    "- HUD 元素（血量、地图、计时器、物品栏标签）\n"
    "- 固有名词（角色名、地名、技能名、道具名）\n\n"
    "输出格式要求（严格遵守）：\n"
    "- 只输出纯文本，禁止使用任何 Markdown 格式\n"
    "- 不使用 **粗体**、*斜体*、# 标题、--- 分割线、- 列表等标记\n"
    "- 如果截图中没有需要翻译的剧情文本，直接回复：截图中未检测到剧情文本。\n\n"
    "只输出翻译内容，不需要其他说明。";

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

// ── Core API call (shared by both public functions) ───────────────────────

static std::string claude_call_api(const std::string &b64,
                                   const char *media_type,
                                   const std::string &api_key)
{
    // Build request JSON (mirrors prototype/analyze.py:56-76)
    json body = {
        {"model",      "claude-sonnet-4-6"},
        {"max_tokens", 2048},
        {"system",     SYSTEM_PROMPT},
        {"messages", {
            {
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
                    {
                        {"type", "text"},
                        {"text", "请翻译这张游戏截图中的剧情对话内容。"}
                    }
                }}
            }
        }}
    };
    std::string body_str = body.dump();

    // CURL
    CURL *curl = curl_easy_init();
    if (!curl) return "错误：curl_easy_init 失败";

    std::string response_buf;
    struct curl_slist *headers = nullptr;
    std::string auth_hdr = "x-api-key: " + api_key;
    headers = curl_slist_append(headers, auth_hdr.c_str());
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    headers = curl_slist_append(headers, "content-type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,           "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_str.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       60L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return std::string("网络错误：") + curl_easy_strerror(res);

    // Parse response
    try {
        json resp = json::parse(response_buf);

        if (resp.contains("error") && resp["error"].is_object())
            return "API 错误：" + resp["error"]["message"].get<std::string>();

        if (resp.contains("content") && resp["content"].is_array()
            && !resp["content"].empty()) {
            auto &first = resp["content"][0];
            if (first.value("type", "") == "text")
                return first["text"].get<std::string>();
        }

        return "错误：无法解析 API 响应：" + response_buf.substr(0, 200);
    } catch (const json::exception &e) {
        return std::string("JSON 解析错误：") + e.what();
    }
}

// ── Public API ────────────────────────────────────────────────────────────

std::string claude_analyze_image_data(const std::vector<uint8_t> &image_data,
                                      const std::string &media_type,
                                      const std::string &api_key_arg)
{
    if (api_key_arg.empty())
        return "错误：请在属性面板中设置 Anthropic API Key";
    const std::string &api_key = api_key_arg;

    if (image_data.empty())
        return "错误：图像数据为空";

    return claude_call_api(base64_encode(image_data), media_type.c_str(), api_key);
}
