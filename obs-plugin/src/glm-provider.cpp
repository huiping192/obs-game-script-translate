#include "llm-providers-internal.h"

std::string GlmProvider::endpoint_url() const
{
    return "https://api.z.ai/api/coding/paas/v4/chat/completions";
}

struct curl_slist *GlmProvider::build_headers() const
{
    struct curl_slist *h = nullptr;
    std::string auth = "Authorization: Bearer " + api_key_;
    h = curl_slist_append(h, auth.c_str());
    h = curl_slist_append(h, "content-type: application/json");
    return h;
}

nlohmann::json GlmProvider::build_request_body(const std::string &b64,
                                                const std::string &media_type,
                                                const std::string &system_prompt,
                                                const char *user_message) const
{
    std::string data_url = std::string("data:") + media_type + ";base64," + b64;
    return {
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
                    {{"type", "text"}, {"text", user_message}}
                }}
            }
        }}
    };
}

std::string GlmProvider::extract_response_text(const nlohmann::json &resp) const
{
    if (resp.contains("choices") && resp["choices"].is_array()
        && !resp["choices"].empty()) {
        const auto &choice = resp["choices"][0];
        if (choice.contains("message") && choice["message"].contains("content"))
            return choice["message"]["content"].get<std::string>();
    }
    return {};
}
