#include "llm-providers-internal.h"

std::string DeepSeekProvider::endpoint_url() const
{
    return "https://api.deepseek.com/chat/completions";
}

struct curl_slist *DeepSeekProvider::build_headers() const
{
    struct curl_slist *h = nullptr;
    std::string auth = "Authorization: Bearer " + api_key_;
    h = curl_slist_append(h, auth.c_str());
    h = curl_slist_append(h, "content-type: application/json");
    return h;
}

nlohmann::json DeepSeekProvider::build_request_body(const std::string &b64,
                                                     const std::string &media_type,
                                                     const std::string &system_prompt,
                                                     const char *user_message) const
{
    std::string data_url = std::string("data:") + media_type + ";base64," + b64;
    return {
        {"model",      "deepseek-flash"},
        {"max_tokens", 2048},
        // deepseek-flash reasons by default; its reasoning tokens are billed as
        // output and dominate cost (~18x more output tokens) without improving
        // translation quality here. Never use reasoning_effort instead — the
        // "minimal" value burns all of max_tokens on reasoning and returns "".
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
                    {{"type", "text"}, {"text", user_message}}
                }}
            }
        }}
    };
}

std::string DeepSeekProvider::extract_response_text(const nlohmann::json &resp) const
{
    if (resp.contains("choices") && resp["choices"].is_array()
        && !resp["choices"].empty()) {
        const auto &choice = resp["choices"][0];
        if (choice.contains("message") && choice["message"].contains("content"))
            return choice["message"]["content"].get<std::string>();
    }
    return {};
}
