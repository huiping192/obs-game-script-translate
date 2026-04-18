#include "llm-provider.h"
#include "llm-providers-internal.h"
#include "llm-utils.h"
#include <curl/curl.h>
#include <memory>

using json = nlohmann::json;

// Declared by OBS_MODULE_USE_DEFAULT_LOCALE in plugin-main.cpp (C linkage)
extern "C" const char *obs_module_text(const char *val);

LlmProvider::LlmProvider(std::string api_key) : api_key_(std::move(api_key)) {}

std::string LlmProvider::analyze_image(const std::vector<uint8_t> &image_data,
                                        const std::string &media_type,
                                        const std::string &target_language)
{
    if (api_key_.empty())
        return obs_module_text("Error.NoAPIKey");
    if (image_data.empty())
        return obs_module_text("Error.EmptyImage");

    std::string b64            = base64_encode(image_data);
    std::string system_prompt  = build_system_prompt(target_language);
    const LangConfig &lc       = get_lang_config(target_language);

    json body                  = build_request_body(b64, media_type, system_prompt,
                                                    lc.user_message);
    struct curl_slist *headers = build_headers();
    std::string raw            = do_post(endpoint_url().c_str(), headers, body.dump());
    curl_slist_free_all(headers);

    try {
        json resp = json::parse(raw);
        if (resp.contains("error") && resp["error"].is_object())
            return std::string(obs_module_text("Error.API"))
                   + resp["error"]["message"].get<std::string>();
        std::string text = extract_response_text(resp);
        if (!text.empty())
            return text;
        return std::string(obs_module_text("Error.ParseResponse"))
               + raw.substr(0, 200);
    } catch (const json::exception &e) {
        return std::string(obs_module_text("Error.JSONParse")) + e.what();
    }
}

std::string LlmProvider::analyze_image_custom(const std::vector<uint8_t> &image_data,
                                               const std::string &media_type,
                                               const std::string &system_prompt,
                                               const std::string &user_message)
{
    if (api_key_.empty() || image_data.empty())
        return {};

    std::string b64            = base64_encode(image_data);
    json body                  = build_request_body(b64, media_type, system_prompt,
                                                    user_message.c_str());
    struct curl_slist *headers = build_headers();
    std::string raw            = do_post(endpoint_url().c_str(), headers, body.dump());
    curl_slist_free_all(headers);

    try {
        json resp = json::parse(raw);
        if (resp.contains("error") && resp["error"].is_object())
            return {};
        return extract_response_text(resp);
    } catch (...) {
        return {};
    }
}

std::unique_ptr<LlmProvider> LlmProvider::create(const std::string &provider,
                                                   const std::string &api_key)
{
    if (provider == "glm")
        return std::make_unique<GlmProvider>(api_key);
    return std::make_unique<ClaudeProvider>(api_key);
}

std::string analyze_image_data(const std::vector<uint8_t> &image_data,
                                const std::string &media_type,
                                const std::string &api_key,
                                const std::string &provider,
                                const std::string &target_language)
{
    auto llm = LlmProvider::create(provider, api_key);
    return llm->analyze_image(image_data, media_type, target_language);
}

std::string analyze_image_custom(const std::vector<uint8_t> &image_data,
                                 const std::string &media_type,
                                 const std::string &api_key,
                                 const std::string &provider,
                                 const std::string &system_prompt,
                                 const std::string &user_message)
{
    auto llm = LlmProvider::create(provider, api_key);
    return llm->analyze_image_custom(image_data, media_type, system_prompt, user_message);
}
