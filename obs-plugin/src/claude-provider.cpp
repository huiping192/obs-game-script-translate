#include "llm-providers-internal.h"

std::string ClaudeProvider::endpoint_url() const
{
    return "https://api.anthropic.com/v1/messages";
}

struct curl_slist *ClaudeProvider::build_headers() const
{
    struct curl_slist *h = nullptr;
    std::string auth = "x-api-key: " + api_key_;
    h = curl_slist_append(h, auth.c_str());
    h = curl_slist_append(h, "anthropic-version: 2023-06-01");
    h = curl_slist_append(h, "content-type: application/json");
    return h;
}

nlohmann::json ClaudeProvider::build_request_body(const std::string &b64,
                                                   const std::string &media_type,
                                                   const std::string &system_prompt,
                                                   const char *user_message) const
{
    return {
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
                {{"type", "text"}, {"text", user_message}}
            }}
        }}}
    };
}

std::string ClaudeProvider::extract_response_text(const nlohmann::json &resp) const
{
    if (resp.contains("content") && resp["content"].is_array()
        && !resp["content"].empty()) {
        const auto &first = resp["content"][0];
        if (first.value("type", "") == "text")
            return first["text"].get<std::string>();
    }
    return {};
}
